# 5. Lexer spec sketch (`llex.c` analogue)

```c
/* (A) start here: 16 bytes, everything inline, written in place into ls->t */
typedef struct Token {
  u8  kind;                /* TK_* */
  u8  flags;               /* int suffix (i8..u64), string-had-escapes, ... */
  u16 _pad;
  u32 offset;              /* byte offset of token start: the only position info */
  union {
    u64    i;              /* INT */
    double f;              /* FLOAT */
    u32    name;           /* NAME/TYPENAME: intern-table index */
    u32    len;            /* STRING*: length (slice at offset+1), or arena
                              index if it had escapes */
  } v;
} Token;                   /* 16 bytes */

/* (B) alternative to measure: 8 bytes, Carbon-style
     u32 kind_aux;  kind = x & 0xff, aux = x >> 8   (manual shifts, not
                    bitfields: MSVC and clang lay those out differently)
     u32 offset;
   aux: NAME/TYPENAME intern index; TK_INT_SMALL value (< 2^24, nearly all
   literals); TK_INT_BIG / FLOAT index into a per-context {u64|double,
   suffix} table; STRING length (< 16 MB) or, for TK_STRING_ESC, an index
   to the decoded copy. Offset can't share a word with kind: 24 bits caps
   files at 16 MB, and dumbbench is 207 MB. */
typedef struct LexState {
  const u8* p;             /* cursor; buffer is NUL-terminated + padded */
  const u8* buf;
  u32  line;
  u16  indent;             /* current level (in units of 4 spaces) */
  u16  pending_dedents;
  u16  bracket_depth;      /* > 0 → newlines are whitespace */
  u8   interp_sp;          /* D13 \( ) nesting: depth of interp_stack */
  u16  interp_stack[16];   /* bracket_depth at each open \( */
  Token t;                 /* current token */
  /* no lookahead slot unless D9/D10 force it */
  struct FuncState* fs;
  jmp_buf* err;
} LexState;
```

Token size: only one or two tokens are ever alive ([D1](D01-single-pass.md) comparison), so size
doesn't matter as storage, only as the cost of handing a token to the
parser. ABIs: SysV x86-64 and AAPCS64 return structs up to 16 bytes in two
registers. **Windows x64 returns only 1/2/4/8-byte structs in `RAX`**, and
16 bytes go through a hidden pointer. If the lexer writes in place into
`ls->t` (Lua's way), every ABI does the same thing, one L1 store, and size
barely matters. **Plan:** start with (A) in place (no side tables, no 2²⁴
limits). The step 1 bench also tries (B) returned by value
(`Token lex(LexState*)`) on macOS and Windows. Switch if it wins, or if a
token buffer ever appears ([D10](D10-comprehensions.md) replay, a future AST). Keep the token behind
a few accessors (`tok_kind`, `tok_name`, `tok_int`, ...) so switching is a
local change.

Measured 2026-11-07 (macOS arm64, `dumbbench.luv`): a variant with a single
global `LexState` instead of passing `ls` was **no faster** (412.6 ± 9.8 ms
pointer vs. 415.8 ± 5.1 ms global, 15 runs). `ls` lives in a register
inside `lex_next`, and the hot cursor is already cached in a local and
written back once per token. So keep the pointer: it's library-safe.

**Where the lexer's time goes** (2026-11-07, macOS arm64, `dumbbench.luv`
207.5 MB / 41.25M tokens, buffer already in memory, `bench/lexfloor.c`, best
of 5):

| Pass | ms | ns/byte |
|---|---|---|
| vectorized byte sum (memory-read floor) | 9.6 | 0.05 |
| `memchr` for every `\n` | 59 | 0.29 |
| scalar copy, 1 load + 1 store per byte | 52 | 0.25 |
| minimal branchy tokenizer (byte control flow only) | 116 | 0.56 |
| lexer, identifiers hashed but not interned, `lex_next` inlined (LTO) | 185 | 0.89 |
| lexer, identifiers hashed but not interned | 215 | 1.04 |
| full lexer | 375 | 1.81 |
| (`fread` of the file, outside the lexer) | 63 | |

So of 375 ms: ~116 ms is the floor of branchy byte-at-a-time scanning, ~70 ms
is real token production (layout, number values, strings, dispatch, token
writes), ~30 ms is the per-token call into `lex_next` (removable by
inlining, LTO or a unity build), and **~160 ms (≈43%) is interning**.
Pre-sizing the table didn't help (it was slower), so the cost isn't
rehashing. It's roughly one cache/TLB miss per new name in a table that
grows to 32 MB. dumbbench is close to the worst case: a fresh random
16-char name every 8 lines. That's the number to attack in the [D15](D15-interning.md)
benchmarks (ideas: prefetch the slot as soon as the hash is known and finish
the probe later, smaller slots, or a per-file first-level table).

How Lua 4 handles the same thing, for comparison: its `Token` is
`{int token; union {double r; TString* ts;}}`, 16 bytes on 64-bit, with no
position (line lives in `LexState`). Every number is copied into `Mbuffer`
and converted to `double` by `luaO_str2d`. The parser's `luaK_number`
(`lcode.c:118`) puts integral values ≤ 2²⁵−1 inline in the instruction
(`OP_PUSHINT`). Anything else goes into a per-function `knum[]` table,
deduplicated over only the last 20 entries (`LOOKBACKNUMS`), and is pushed by
index (`OP_PUSHNUM`). Strings are interned `TString*`, with the per-function
constant index cached on the string. That's Carbon's inline-small /
side-table-large split, done at the bytecode level instead of in the token.

`lex(LS)`, in Lua's `for(;;) switch (*p)` style:

- `' '`, `'\r'`: skip. `'\t'`: error.
- `'\n'`: if `bracket_depth`, skip. Otherwise, the layout routine: count
  spaces; skip blank and comment-only lines; compare against `indent`;
  return `NEWLINE`/`INDENT`/`DEDENT` and set `pending_dedents`.
- `'#'`: `p = memchr(p, '\n')`.
- Punctuation: one `switch` case per first char, with a peek at `p[1]` for
  `== != <= >= << >> += -= *= /= %= |= &= ^= <<= >>=`. Padding means no
  bounds check.
- `[0-9]`: number ([D13](D13-literals.md)).
- `"`: string ([D13](D13-literals.md)). On `\(`, return `STRING_BEGIN` and push
  `bracket_depth`. On a `)` that brings `bracket_depth` back to
  `interp_stack[interp_sp-1]`, pop it and resume the string reader, returning
  `STRING_MID`/`STRING_END` instead of `)`.
- Identifier start (class table): scan while `class[*p] & IDENT`, folding the
  hash and the `has_upper`/`has_lower` bits as it goes. Intern. If the interned
  `Name` is reserved, return its keyword kind. Otherwise return `TK_TYPENAME`
  or `TK_NAME` from the case bits ([D4](D04-types-vs-values.md) rule: `_*[A-Z]` or `_*[A-Z]…[a-z]…` →
  TYPENAME; `ALL_CAPS` and lowercase → NAME).
- `'\0'` at `p < end`: error (embedded NUL). At end: dedents, then `TK_EOS`.
