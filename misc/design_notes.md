# luvr: a fast, nearly-LL(1) front end for luv, built like Lua 4.0.1

This plan covers the decisions to make before writing a lexer and parser for
the luv language (`~/luv60`, examples in `~/luv60/test`). The structure follows
`llex.c` / `lparser.c` from Lua 4.0.1 (`~/luv60/scratch/lua-4.0.1/src`). The
goal is parse speed first and a grammar that needs one token of lookahead
almost everywhere. Where we can't get LL(1) cheaply, we say so and say how
we'll handle it.

Implementation constraint: **plain, portable C. No SIMD, no re2c, no
generated tables beyond what's written by hand.** (luv60's SIMD indexer in
`lex.c` is Windows/x86-64 only. On arm64 it's compiled out, so luv60's macOS
timings below come from its scalar path. That makes them a fair target.)

Product shape: the final deliverable is a unity-built, **stb-style
single-header library**. Cross-file inlining (`lex_next` into the parser,
for example) therefore comes for free, as with LTO.

Section 8 checks the plan against the 10 LL(1) design rules from the
earlier chat.

---

## 0. Baseline and targets

| Measurement | Input | Time |
|---|---|---|
| Lua 4.0.1 `luac -p` (lex + parse + **bytecode gen**, no write) | `dumbbench[0-39].lua`, 10M lines / 210 MB | **1.59 s** real (this machine) |
| Lua 4.0.1 `lua dumbbench.lua` (above + run) | same | 2.07 s real |
| `ref/lua-4.0.1-nocode` `luac-nocode -p`: `lcode.c` stubbed via `-DLUA_NOCODEGEN`; lexer, parser, locals and constant tables unchanged | same | 1.19 s (hyperfine) |
| `ref/lua-4.0.1-nocode` `LUAC_LEXONLY=1 luac-full -p`: `luaX_lex` loop only, including interning | same | 0.82 s (hyperfine) |

So Lua 4's ~1.6 s splits roughly into **lex ≈ 0.82 s, parse ≈ 0.38 s,
codegen ≈ 0.42 s**. The lexer is half the total. That's where the
section-1 changes (ZIO → pointer, no `Mbuffer` copy, class table, inline
ints) should pay off most.
| luv60 `--syntax-only` (from `misc/notes.txt`, M3 Air, scalar lexer since SIMD is x86-only). **Not reproduced here**: release `luvc` at `bfc6ffc` segfaults with `--syntax-only`. Re-measure on `out/mr/dumbbench.luv` (luv60's own dialect: `i64`, `x = x+i`) once fixed | `dumbbench.luv`, 10M lines / 207 MB | ~0.80 s |

Lua 4 does a lot with very little: a pull lexer, one token of lookahead, a
recursive-descent parser that emits code directly, and no AST. Targets for
luvr:

1. **Lexer + parser with a null code generator** should beat `luac -p` by a
   clear margin on `dumbbench.luv`. Stretch goal: match or beat luv60's
   `--syntax-only`, using plain C only.
2. Keep the Lua 4 shape: `LexState` with `t` and at most one `lookahead`
   token, `next/check/check_match/optional`, a `stat()` switch, a
   `subexpr()` priority table, and an `expdesc` whose emission is deferred.
3. The language should still read like current luv: indentation, `def int
   main():`, `[]str res`, `Stuff s`, `x = 1`, `on T def ...`.

---

## 1. Shape of the implementation (what we keep from Lua 4, what we change)

| Lua 4.0.1 | Keep / change | Why |
|---|---|---|
| `luaX_lex` pull lexer, called by the parser's `next()` (`lparser.c:56`) | **Keep** | No token array, nothing to allocate, good cache behaviour. |
| `ZIO` + `LS->current` + `next(LS)` = `zgetc` (`llex.c:26`) | **Change**: whole file in memory, NUL-padded, scanned with a `const u8* p` | Removes the per-character buffer-refill check. The terminating `\0` is the EOF sentinel. |
| `readname` copies into `Mbuffer`, then `luaS_new` hashes and interns (`llex.c:135`, `:370`) | **Change**: hash while scanning, intern straight from the source slice | Removes a copy and a second pass over each identifier. |
| Reserved words found by interning, flagged with `ts->marked` (`llex.c:41`) | **Keep the idea** | One hash lookup answers "keyword?" and also yields the interned name the parser needs anyway. |
| `isalpha`/`isdigit` from ctype | **Change**: 256-entry class table | Locale-free, branch-light. |
| Numbers via `luaO_str2d` (strtod) | **Change**: integers accumulated inline; strtod only for floats | Most literals are small ints. |
| `Token t; Token lookahead;` plus a branch in `next()` | **Keep, aim for zero uses** | Lua 4 itself only uses the slot for `{name = ...}` in constructors (`lparser.c:554`). If we never need it, delete the slot and the branch. |
| `subexpr` with `{left,right}` priority table (`lparser.c:695`, `:710`) | **Keep** (replaces luv60's Pratt rules table) | `misc/notes.txt:131` already suggests dropping Pratt. Same algorithm, smaller dispatch. |
| `expdesc` with `VLOCAL/VGLOBAL/VINDEXED/VEXP`, discharged late | **Keep and extend** (add `VTYPE`, `VNAME_UNRESOLVED`, ...) | This is what lets us "parse a prefix, then decide" without backtracking (see D4, D8). |
| `namestat`: parse `var_or_func`, then branch on `v.k` (`lparser.c:980`) | **Keep the pattern** | Same trick as declaration vs. expression statement. |
| `check_match(ls, what, who, line)` error with opener line | **Keep** | Cheap and gives good errors. |
| `luaK_*` code generator interface (`lcode.c`) | **Keep as an interface**, start with a null implementation | Lets us measure parse-only speed honestly, like luv60's `parse_syntax_check.c`. |
| Errors via `lua_error` → `longjmp` | **Keep** | First error aborts. No recovery machinery on the fast path. |
| Free-form layout with `end`-terminated blocks | **Change** to indentation (D2) | That's the language we want. Section 3 shows it stays LL(1). |

---

## 2. Decision points

Each decision lists the options, what they cost in **lookahead** and **speed**,
and a recommendation. **★** marks the decisions that determine whether the
grammar is LL(1). The rest are local.

### Decision log

| # | Status | Decision |
|---|---|---|
| D1 | **decided** 2026-11-07 | Single pass as much as possible: (c) hybrid. The only exceptions are D10's lexer rewind and D11's skim-on-miss. No general AST. |
| D2 | **decided** 2026-11-07 | Significant indentation, fixed 4-space unit. Tabs and non-multiples of 4 are errors. |
| D3 | **decided** 2026-11-07 | One layout token per line boundary (`NEWLINE` / `INDENT` / `DEDENT`×k), and newlines are whitespace inside brackets. D3a: **yes**, one-line suites for simple statements. D3b: **no** `\` continuation. The `newline_before` flag variant is only a later measurement idea. |
| D4 | **decided** 2026-11-07 | Lexical naming convention as a hard rule (CamelCase / single capital = `TK_TYPENAME`, builtin types are keywords). Drop `IDENT_CONST` and `IDENT_IMPORT` lexing. A single capital is always a generic type parameter (D21, 2026-11-12). |
| D5 | **decided** 2026-11-07 | Statement-start dispatch as in the D5 table: no prefix deref for now (revisit, postfix form, if `p[0]` gets tedious), only call/assignment expression statements. D5a: **no** multiple declarators. D5b: **deferred**. The multiple-values-not-tuples proposal is kept below for reference. Revisit when real code needs it, alongside a Zig-like `!T`/`try` error type, which may be the better answer. The parenthesized-RHS formatter escape is not agreed. |
| D6 | **decided** 2026-11-07 | (b) declare at first assignment, visible from that point on (current luv60 behaviour), function-level scope, `global` to opt out. An intentional difference from Python's whole-function rule. |
| D7 | **decided** 2026-11-07 | As in the D7 table. **`print` stays a keyword statement** (`print expr`). **`cast T(x)`** with required parens (matching `sizeof(...)` etc.): after `cast` the parser reads a type, and `(` must follow. No cast precedence rule needed. Intended for pointer↔int and similar. `sizeof`/`alignof` use the first-token rule, `typeof(e)` is a type form, `offsetof` is type-only. `len`/`range` are builtin symbols (`fmt` is dropped, D13), not keywords. |
| D8 | **decided** 2026-11-07 | Lua 4 `subexpr` precedence climbing with the D8 table. `not` at Python precedence (below comparisons). Comparisons non-associative (`a < b < c` is an error). **No ternary.** `not in` via `not` in binop position, slices as in D8, augmented assignment as statements only, assignment is not an expression. |
| D9 | **decided** 2026-11-07 | No `T(x)` conversion syntax; `Name(...)` is only a compound literal (see D7). Named arguments use **C-style designators**: `Stuff(.a=44, true)`. A leading `.` can't start an expression (D13), so this is LL(1) from one token, with no lookahead slot and no parse-then-reinterpret. Name lookup is never deferred, and `f(x=1)` typos are errors. Named parameters are **not** planned for ordinary functions, so `.name=` only ever means a struct field (may be revisited). |
| D10 | **decided** 2026-11-07 | Python order via **lexer rewind** (b). Defaults recorded: a comprehension opens its own scope, so its loop variables don't leak (`compr_shadow.luv`), the one exception to D6's function-level scope. Multiple `for`/`if` clauses are allowed, nesting left to right per PEP 202. Multiple loop targets (`for k, v in`) allowed. Dict comprehensions deferred (the skim must pass the first depth-0 `:`). |
| D11 | **decided** 2026-11-07 | Assume define-before-use, **skim on first miss** (c), with the limits listed in D11. Defaults recorded: no forward-declaration syntax (skim covers mutual recursion). Nested functions are strictly define-before-use (only indent-0 lines are skimmed, and D12's capture-at-definition implies it anyway). |
| D12 | **decided** 2026-11-07 | Implicit capture **by value at definition time**, multi-level, via the `FuncState->prev` chain. Pointers, escaping and name-resolution details are for later. |
| D13 | **decided** 2026-11-07 | Floats as in the D13 table (digit after `.` or exponent; `f32`/`f64` suffixes; no `1.f`). Strings `"` only. **Swift-style `\(expr)` interpolation** via a lexer mode stack (`STRING_BEGIN`/`MID`/`END`, details in D13). Raw newlines are errors anywhere in a string, including inside `\( )`. **`fmt(...)` is dropped**: it was compile-time only, so interpolation covers it. Manual argument reordering (`fmt("{1} {0}")`) is accepted as lost. Format specs deferred. Escapes: `\n \t \r \\ \" \0 \xHH \u{...}` and `\(`, none of Lua 4's `\a \b \f \v \ddd`. **No raw or multiline strings** for now (`r"..."`/`"""..."""` can be added later without conflict). |
| D14 | **decided** 2026-11-07 | `#` to end of line, skipped with `memchr`. **Trailing comments allowed** (luv60 rejected them only because of its SIMD indexer). No block comments. |
| D15 | **deferred to measurement** | Hash table implementation chosen in step 1 by benchmarking on real lexer traffic: luv60 swisstable (`dict.h`), open addressing with stored hash, Lua-style chaining. Kept as design regardless: one table for keywords and identifiers (Lua `marked`-style), and a `binding` pointer on each `Name` for one-load scope lookup (to be benchmarked in step 3 against a Lua-style fixed per-function locals table with a linear scan). |
| D16 | **decided** 2026-11-07 | Line counter **in the lexer only** (free, since layout already visits every `\n`). Used for errors and debug line info. Tokens carry only a `u32` offset, and columns are computed lazily on error. Errors: first error → `longjmp` to the handler. **Library-safe**: all state lives in a context struct (no mutable globals, unlike luv60's `glob`/`tu`), all allocation comes from context-owned arenas reset by the handler, no `exit`/`abort`/stdio in the library (message + offset stored in the context, internal asserts take the same path), keywords interned per context (Lua 4's `luaX_init` per `lua_State`). |
| D17 | **decided** 2026-11-07 | Separate `toplevel_stat()` and `stat()` switches instead of luv60's repeated `if (toplevel) error(...)`. `module -> { import } { toplevel_stat } EOS`. Nested `def` allowed; `struct`/`on`/`foreign`/`import` top-level only. |
| D18 | n/a | Guidance for future syntax, nothing to decide now. |
| D19 | **decided** 2026-11-07 | D19a: trailing commas **only in list/dict literals** (not calls, params, or compound literals). D19b: **no** adjacent string concatenation. Section 9 rules 1–5 remain constraints on everything else. |
| D20 | open | **Looping constructs.** No `while` keyword (removed from the lexer). Investigate a general `for` that covers `while`-style loops (e.g. `for cond:` vs. `for x in xs:`). Interesting for LL(1): after `for`, telling `NAME [, NAME] in` from a condition expression needs care, since a condition can also start with `NAME`. Options include parse-then-decide on `in`, as with D5's expdesc dispatch. |
| D21 | **decided** 2026-11-08 (provisional), **revised** 2026-11-12 | Generics use `{}`: `List{int}`, `struct Pair{A, B}`, `def T max{T}(...)`, explicit `max{u64}(...)`, inferred by default. Monomorphized by re-parsing the body per instantiation. Revised: type parameters are exactly one capital letter (`[A-Z]`), and a single capital letter is always a type parameter, never a struct name. `{...}` lists are still required and give the order of explicit type arguments. A return type that uses parameters before `{T}` is checked afterwards with a 26-bit mask, so binding stays single-pass. Options explored (`<>`, `[]`, `'`, `!`, parameter placement, `$T`, alphabetical order) are recorded in D21, since the user may revisit this after writing generic code. **Revised 2026-11-13:** no free generic functions. Type parameters only come from a struct's list or a method's receiver, so type arguments are never inferred from function arguments. Free generics can be added later without breaking anything. |
| D22 | **decided** 2026-11-08 | Methods (`on`): keyed on (generic type, name), no specialization, no methods on pointers, methods only in the receiver type's own module (builtin types: the prelude, D22a open), no free-function overloading. Static methods (`List{int}.new()`) and dunders as operator spellings. A method is a free function with nicer syntax and a namespace: `xyz.f(5)` is `Xyz.f(&xyz, 5)`. |
| D23 | **open** 2026-11-12, option B preferred | Arrays, lists and slices, and how their memory is managed. What `a = [1, 2, 3]` is, what `a[1:]` is, what a comprehension produces, and what the user has to do to free each of them. This affects literal typing, comprehensions (D10), `[]T`/`{K}V` (D21, D22a) and likely more syntax, so it's settled before further type work. Option A: lists in locals own a region, a current pool, copying results out of scratch blocks. **Option B (preferred):** one dynamically scoped current arena that every allocation goes into, `with Arena():` and `into a:` blocks, and opt-in storage annotations. Under B: literals are lists, slicing copies, comprehensions produce lists, views are deferred to a later `View{T}`/`Slice{T}` type. Strings are immutable `{ptr, len}` with a builder, structs are values, `[]T` is `*List{T}`, and `&Stuff(...)` allocates in the current arena. |

### D1 ★ Single-pass emit vs. building an AST

Lua 4 parses and emits bytecode in one pass. luv60 also works in one pass
(parse → type check → IR). Some luv features don't fit one pass well:
comprehensions (D10), forward references (D11), and capture-by-value
analysis (D12).

- **(a) Strict single pass, Lua style.** Fastest, with no AST allocation.
  But every construct has to be ordered so names and types are known when
  first seen, and that shapes the syntax.
- **(b) Flat AST** (an arena array of fixed-size nodes, with indices instead of
  pointers) followed by a separate check/emit pass. It costs one write and one
  read per node. It frees the syntax from emission order and makes
  comprehensions, hoisting and incremental work (luv goal #2) easier.
- **(c) Hybrid.** Single pass, with escape hatches: a top-level skim pass
  (D11) and token-range replay for the few constructs that bind late (D10).

**Recommendation: (c).** Keep the Lua shape, and add two narrow escape
hatches with clear boundaries rather than a general AST. Revisit if the
incremental-compilation goals in luv's README (`~/luv60/README.md`) start to
need a persistent tree.

**Comparison: pre-tokenized buffer (Carbon, luv60).** Carbon lexes the whole
file into a token array that stays alive (8-byte tokens: 8-bit kind, 24-bit
index into side arrays, 32-bit source offset), then parses it in a second
pass. luv60 does the same with `token_offsets`. The packing matters because
the array is large. `dumbbench.luv` has roughly 35 tokens per function ×
1.25M functions ≈ 44M tokens: about 350 MB at 8 bytes per token, which is
more than the 207 MB source, or about 1 GB at a naive 24 bytes. A pull
lexer never creates that array. Only `ls->t` is alive, and nothing refers
back to a token after it's consumed except its offset. So for (a) and (c),
bulk token storage isn't a cost at all. A token buffer only becomes
worthwhile if:
- D1 moves to (b) or incremental work needs a persistent structure. Then a
  token array is the natural thing for nodes to refer to, and Carbon-style
  8-byte packing is worth copying exactly.
- D10 uses skim and rewind. Instead of lexing the bracket twice, the skim
  can record tokens into a small scratch buffer of 8-byte packed tokens and
  replay them. That's local and bounded, so it's an implementation detail
  rather than an architecture change.

### D2 ★ How blocks are delimited

- **(a) Significant indentation, fixed 4-space unit.** This is what luv60 does
  (`categorizer.in.c` matches `\n` followed by 4·k spaces). The indent level
  is `spaces/4`. Anything that isn't a multiple of 4, and any tab, is an
  error. No indent stack is needed.
- **(b) Python indentation with any width** and an indent stack. More
  flexible, a little more work per line, and the error messages are worse.
- **(c) Braces or `end`** (Lua). Free-form layout, so the lexer never looks at
  column positions. `misc/notes.txt:491` already asks whether braces would be
  faster. They don't change the asymptotics. Indentation work only happens
  once per line, and newlines get scanned anyway.

**Recommendation: (a).** It's cheap and it's the luv look. The token protocol
is in D3.

### D3 ★ Layout tokens (how newlines and indentation reach the parser)

Python sends `NEWLINE`, `INDENT` and `DEDENT` as separate tokens, plus extra
`NEWLINE`s. Lua sends nothing, because its statements delimit themselves.
Proposal: send **exactly one layout token per logical line boundary**:

| Next non-blank line, relative to current indent | Token(s) |
|---|---|
| same level | `TK_NEWLINE` |
| one level deeper | `TK_INDENT` (it implies the newline) |
| deeper by more than one level | error: "unexpected indent" |
| k levels shallower | `TK_DEDENT` × k (lexer keeps `pending_dedents`, emits them without moving `p`) |
| end of file | `TK_DEDENT` × level, then `TK_EOS` |

Rules:

- A line that is blank or contains only a comment produces no tokens. The
  indentation of comment lines is ignored, which already matches
  `test/comment_indent.luv` and `test/lex_indented_comments.luv`.
- When the bracket depth (`(`, `[`, `{`) is above 0, a newline is whitespace.
  luv60 already does this with `token_continuation_paren_level`.
- A simple statement ends at `TK_NEWLINE`, which it consumes, or at
  `TK_DEDENT` or `TK_EOS`, which it leaves for the enclosing block. That makes
  `block_follow()` = `{TK_DEDENT, TK_EOS}`, the same pattern as Lua's
  `block_follow` (`lparser.c:749`).
- A compound statement (`if`/`for`/`def`/...) ends with its block's
  `TK_DEDENT`, so it doesn't need a trailing NEWLINE.
- `elif`/`else` come after the `if` block's `DEDENT`. One token of lookahead
  decides, the same as Lua's `while (ls->t.token == TK_ELSEIF)`.

**Sub-decision D3a:** allow one-line suites (`if x: return 1`)? It's LL(1):
after `:`, if `t == TK_INDENT` parse a block, otherwise parse one simple
statement. It costs almost nothing. **Lean yes**, limited to simple
statements.

**Sub-decision D3b:** allow `\` line continuation? **Lean no.** Brackets cover
it.

**Alternative to measure later:** drop `TK_NEWLINE` and put a
`newline_before` bit on the following token, like Go's and JS's ASI-style
lexers. That saves one `next()` per statement, but separators become
implicit. Try it only if profiling shows per-token overhead matters.

### D4 ★ Telling types from values (the C-declaration problem)

The user requirement is C-style `Type name` declarations. In C this is the
classic ambiguity: `T * x;`, `T(x);`, and the lexer hack. luv60 avoids most of
it by **classifying identifiers lexically** (`categorizer.in.c:8-10`):

- `varname` = starts lowercase → `IDENT_VAR`
- `typename` = CamelCase / single capital → `IDENT_TYPE`. A single capital
  is always a generic type parameter, never a struct name (D21).
- `constname` = `ALL_CAPS` → `IDENT_CONST`
- builtin types (`int`, `i32`, `str`, `bool`, ...) are keywords.

Options:

- **(a) Keep the naming convention.** `TK_TYPENAME` is decided in the lexer
  for free: track "saw uppercase" and "saw lowercase" bits while scanning the
  name. With that, every type-starting token is recognizable from one token:
  `TK_TYPENAME`, builtin-type keywords, `*`, `[`, `{` (and `def` if function
  types arrive).
- **(b) Symbol-table feedback (the C lexer hack).** Needs declare-before-use
  and couples the lexer to scopes. It breaks out-of-order top-level
  declarations and imports.
- **(c) Keyword-introduced declarations** (`var x int`, `let`). Trivially
  LL(1), but drops the C-style requirement.
- **(d) Postfix annotations** (`x: int`). LL(1) via `NAME ':'`, but again not
  C-style.

**Recommendation: (a).** Two refinements:

1. **Drop `IDENT_CONST` as a token kind.** Constness is semantic and no
   parse decision depends on it. `[BLAH]int` takes an expression in brackets
   anyway. A token kind with no parse use is one more case for every switch.
2. **Drop `IDENT_IMPORT` lexing.** luv60 looks up every lowercase identifier
   in the import dict in the lexer (`token.c:21`). That's a hash probe per
   identifier for a decision the parser can make when it reaches `NAME '.'`
   (see D5). Removing it makes the lexer context-free.

The convention is the price of LL(1) C-style declarations. It's worth stating
in the language docs as a rule, not a style suggestion.

### D5 ★ Declaration vs. expression at statement start

Statement-start FIRST sets, assuming D4(a):

| First token | Can be | Decision |
|---|---|---|
| keyword (`if`, `for`, `def`, `return`, ...) | that statement | LL(1) switch, like Lua's `stat()` (`lparser.c:1022`) |
| builtin type, `*`, `[`, `{` | **declaration only** | Requires the rules below: no expression statement may start with these |
| `TK_TYPENAME` | `Stuff s` (decl) or `Stuff(1).f()` (expr) | **Left-factor**: parse the type. If the next token is `NAME` it's a declaration. If it's `(`, it's a compound literal; continue as a postfix expression |
| `TK_NAME` | `x = ...`, `x += ...`, `f()`, `a.b = ...`, `mod.Type v` | Parse the postfix chain into an `expdesc` (Lua `var_or_func`). Then branch on `v.k` and `t`: `VTYPE` followed by `NAME` → declaration. Lvalue followed by `=` or augmented op → assignment. A call → call statement. Otherwise "syntax error" |
| anything else | — | error "statement expected" |

Constraints this puts on the expression language:

- **No prefix dereference `*p`.** luv already uses `p[0]` and auto-deref on
  `.`, so a statement starting with `*` is always a pointer type. If deref
  syntax is ever needed, make it postfix (`p.*` or `p^`) so it doesn't take
  `*` away from types. (`*` followed by `NAME` would be LL(1) for one level,
  but `**p` vs `**int` makes it LL(k).)
- **Only certain expression statements are allowed**: calls, assignments,
  augmented assignments. Lua enforces the same rule (`namestat` +
  `check_condition(... "syntax error")`). That makes `[f(x) for x in xs]` or
  `{...}` as a bare statement illegal, which is no loss.
- **Qualified types (`mod.Type x`) need no lexer help.** The parser resolves
  `NAME '.' TYPENAME` as a postfix chain. Because `Type` is lexically a
  type, the `expdesc` becomes `VTYPE`, and the following `NAME` makes it a
  declaration.

Declaration form sub-decisions:

- **D5a:** multiple declarators (`int a, b = 1, 2`)? **Lean no.** One
  declarator per statement keeps the code simple, and the tests never use it.
- **D5b (deferred; proposal kept for reference): multiple return values, not tuples.** A function may
  return several values (`def int, bool parse(str s):`,
  `return n, true`), but a value list is **not a type**. It can't be stored
  in one variable, passed as one argument, indexed, or nested. It can only
  be consumed by multi-assignment (`n, ok = parse(s)`, `a, b = b, a` with
  the whole right side evaluated first) or forwarded with `return f()` from a
  function with the same return types. `x = f()`, `g(f())`, `print f()` on
  a multi-value `f` are errors (Go's rule, not Lua's truncation). The ABI
  returns an anonymous struct, a backend detail.
  - LL(1): signature `def T {',' T} NAME '('`: after a type, `,` means more
    types, `NAME` is the function name. Multi-assignment is Lua's recursive
    `assignment` (`,` after the first target). Declaring follows D6: new
    names on the left are declared with types from the signature. No
    typed multi-declaration (consistent with D5a).
  - **Not** `def (int, bool) f(`: at statement start, `def (` would clash
    with function-type declarations `def(int) int cb = ...` (D18). Unless
    function types use a different keyword (`fn(int) int`), keep the
    unparenthesized list.
  - `_` becomes a reserved blank name for discarding (`n, _ = parse(s)`).
  - Formatter (section 9): a parenthesized expression list is allowed
    *only* as the entire right side of a multi-assignment or `return`
    (`return (a,\n b)`). That's grouping, not a tuple. After `(` and an
    expression, `,` means a list. One token.
  - Alternatives: return a struct (`return ParseResult(n, true)`, works
    today) or out-params (`*int out`). An error/optional type (`!T`/`?T`
    with `try`) for no-exceptions error handling is a separate, later
    decision that this doesn't block.

### D6 ★ How plain `x = 1` introduces a variable (and scope granularity)

This is semantic, but it determines whether a single-pass parser can work
at all.

What the tests show:

- `create_local_over_global.luv`: assigning to a global's name inside a
  function creates a local.
- `for_is_not_scope.luv`: `for` doesn't open a scope. Scope is
  function-level, Python-style.
- `scopes.luv`: `global x` opts out.

Options:

- **(a) Python's rule: "assigned anywhere in the function → local
  everywhere in it".** This needs to see the whole body before resolving
  the first use, so it's **not single-pass**.
- **(b) Declare at first assignment, visible from that point on** (Lua's
  `local` without the keyword). Single-pass friendly. It is what luv60
  currently does: `assign_global.luv` prints the global, then assigns,
  which wouldn't work under rule (a).
- **(c) Explicit declaration operator** (`x := 1` declares, `x = 1` only
  assigns). LL(1) via the token, catches typos (`cuont = 1`), and is
  single-pass. Less Python-ish.

**Recommendation: (b)**, documented as an intentional difference from Python.
Keep (c) in mind if typos start biting. With function-level scope, the Lua
`block()` → `removelocalvars` step (`lparser.c`, `block`) becomes a no-op
inside a function. Lua's `actloc` stack only gets pushed per function and per
comprehension (D10).

### D7 Keywords vs. builtin names

luv60 reserves many words (`fmt` is now dropped, D13): `print len range fmt sizeof offsetof alignof
typeof cast check alloc del relocate typeid`, plus about 18 type names. Each
keyword is one less identifier and one more interned "reserved" entry, but it
costs nothing at runtime. The question is which ones need **special syntax**:

| Word | Special syntax? | Recommendation |
|---|---|---|
| builtin types (`int`, `u8`, `str`, ...) | yes: they start type FIRST sets (D5) | keyword |
| `print` | `print expr` without parens | keyword statement (or make it a function and drop the rule; **user's call**) |
| `cast` | `cast T(x)`: a type, then a parenthesized operand | keyword. **Decided: required parens**, like `sizeof(...)`. Plain `T(x)` can't work for all types in expression position (`[4]int(x)` vs. a list literal), so the `cast` keyword is what enables types there. With parens, there's no precedence rule, and `cast i64(x) * -1` vs. `cast i64(x * -1)` is explicit. A long operand wraps inside the parens (these parens are required syntax, not grouping, so section 9 rule 2 is unaffected) |
| `sizeof`, `alignof` | take a type **or** an expression | keyword. First token decides: builtin type or `*` → type. Literal, `(`, `-`, `not`, `&` → expression. `NAME`/`TYPENAME` → D5 postfix chain, then `VTYPE` → type, otherwise expression (`Stuff(1)` has the same size as `Stuff` anyway). `[`/`{` → **always type** (fixed rule, still LL(1)). `sizeof(([1,2]))` forces an expression. No `sizeof_expr` keyword needed |
| `typeof` | takes an expression, **produces a type** | keyword, and a type-starting token: `typeof(x) y = ...`, `sizeof(typeof(e))` |
| `offsetof` | `(type, field)` | keyword, type only |
| `check`, `pass`, `global`, `with`, `on`, `foreign`, `struct`, `import`, `elif` | statement introducers | keyword |
| `len`, `range` | no, ordinary call syntax | **not keywords**. Resolve as builtin symbols in the global scope (compile-time intrinsics). (`fmt` is dropped in favour of `\(expr)` interpolation, D13) |
| `true false null` | literals | keyword |
| `in`, `not`, `and`, `or`, `as` | operators | keyword. (Lua 4 avoided making `in` reserved with a "dirty" check, `lparser.c:873`. We don't need that.) |

### D8 ★ Expressions: engine, operator set, precedence

Engine: **Lua 4 `subexpr`** with a `{left, right}` priority table, unary
operators at `UNARY_PRIORITY`, and the postfix chain (`.`, `[...]`, `(...)`)
handled in the primary-expression loop like `var_or_func_tail`
(`lparser.c:422`).

Proposed priority table (low → high), mostly Python's:

| Level | Operators | Notes |
|---|---|---|
| 1 | `or` | |
| 2 | `and` | |
| 3 | `not` (unary) | **Python precedence**: `not a == b` = `not (a == b)`. Lua's `not` binds tightly. Decide, lean Python. Implement as a prefix handled in `subexpr` with priority 3 instead of `UNARY_PRIORITY` |
| 4 | `== != < <= > >= in not in` | **Non-associative**: `a < b < c` is an error. Python chains them, C does the wrong thing. An error is the simple, safe choice |
| 5 | `\|` | |
| 6 | `^` | |
| 7 | `&` | |
| 8 | `<< >>` | |
| 9 | `+ -` | |
| 10 | `* / %` | |
| 11 | unary `- ~ &` (address-of) | `cast T(x)` is a primary, not an operator (D7) |
| 12 | postfix `. [] ()` | |

LL(1) details:

- **`not in`**: in *binary-operator position*, `not` can only mean `not in`.
  So `getbinopr(TK_NOT)` returns `OPR_NOTIN` and then requires `TK_IN`. That
  is LL(1), with no lexer fusion needed.
- **Slices `a[i:j]`, `a[:j]`, `a[i:]`**: after `[`, `:` means no start.
  Otherwise parse an expression, then take `:` or `]`. LL(1).
- **Ternary.** Options: none (current), Python `a if c else b`, or
  `c ? a : b`. The Python form conflicts with comprehension filters
  (`[x for x in xs if c]`). Python handles that by parsing comprehension
  conditions one level below the ternary. `? :` is also messy because `:`
  is the block and slice token. **Lean: none for now.** If added, use the
  Python form with a "no-ternary" flag passed into comprehension `if`
  clauses.
- **Augmented assignment** (`+= -= *= /= %= |= &= ^= <<= >>=`): needed,
  since `dumbbench.luv` uses `x += i`. Lexer tokens, statement level only,
  so they aren't expression operators.
- **Assignment is not an expression.** That keeps `=` free for D9's keyword
  arguments.
- **Integer literal suffixes** (`123i32`, `0u64`) are lexical (D13).

### D9 Calls, keyword arguments, compound literals

`Stuff(a=44, true)` and `Stuff(x=99, false)` exist (`compound_literal.luv`,
`struct_with_initializer.luv`). `NAME '='` looks like it needs two tokens. In
Lua 4, this is exactly the one place that uses `lookahead()`
(`constructor_part`, `lparser.c:554`).

- **(a) Use the lookahead slot**, like Lua. Simple, and costs one branch in
  every `next()` (see section 1).
- **(b) Parse first, decide after.** Parse an argument as an expression. If
  the resulting `expdesc` is a bare, undischarged `VNAME` *and* `t == '='`,
  turn it into a keyword argument. This only works if a bare name isn't
  resolved or emitted until it's discharged. Lua's late discharge already
  works this way.

**Recommendation: (b)**, which helps remove the lookahead slot entirely.

**Decided instead: (c) C-style designators**, `Stuff(.a=44, true)`. An
expression can't start with `.` (D13 bans `.5`), so `'.' NAME '='` in
argument position is LL(1) from its first token. Compared with (b): no
deferred name resolution inside argument lists (with (b), `Stuff(a=44)`
can't look up `a` until it sees `=`), and `f(x=1)`-for-`f(x==1)` typos
become errors. Update `compound_literal.luv`, `struct_with_initializer.luv`
and the README examples.

Other decisions here:

- **Compound literal = call on a type.** `TYPENAME '(' args ')'`.
  **Decided (via D7):** no `T(x)` conversion syntax. Conversions are always
  `cast T(x)`, and bare `Name(...)` is only a compound literal.
- **Argument evaluation order** and promotion (`int_promotion_in_call.luv`)
  are semantic and outside the parser's scope.

### D10 ★ Comprehensions in a single pass

`[magic(x) for x in nums]`: the element expression uses `x` before `for x`
binds it. This is the one construct in the current language that is
**inherently not LL(1) with single-pass emit**. `misc/notes.txt:273` covers
the same ground. Options:

- **(a) Reorder the syntax**: `[for x in nums: magic(x)]`,
  `[for x in nums if x % 2: x*3]`. Pure LL(1) and single-pass. It reads
  "randomly different" from Python (notes' words).
- **(b) Lexer rewind.** The lexer works on an in-memory buffer, so its state
  is `{p, line, bracket_depth, ...}`. Inside brackets the indentation state
  is frozen, so saving or restoring it is a struct copy. In `[`:
  1. Save checkpoint `E` (start of the element).
  2. **Skim**: run `lex()` in a tight loop, tracking bracket depth, until the
     first depth-0 `for`, `,` or `]` (or `:` for dicts). No parsing, no
     symbol lookup, no emission. It stops at the *first element*, so long
     literals pay for one element, not the whole list.
  3. `,`/`]`: list literal. Rewind to `E` and parse normally.
  4. `for`: parse the `for x in xs [if c]` clauses with the normal parser,
     which evaluates `xs`, binds `x`, and emits the loop header and filter.
     Save checkpoint `R` at the closing `]`. Rewind to `E` and parse the
     element with the **same** expression parser. It stops by itself at
     `for`, because `for` can't continue an expression. Emit the append,
     close the loop(s), and jump to `R`.

  Cost: the first element gets lexed twice, only inside `[`/`{` literals.
  `next()` stays a bare `t = lex()`. Interning is idempotent. Nested
  comprehensions keep their checkpoints on the C stack. There is no second
  parser or rules table (the "stinky" part in `misc/notes.txt:273`).
- **(b′) Token recording/replay.** The same idea, but the skim stores tokens
  in a scratch buffer, and `next()` gets one branch
  (`if (replay < replay_end) t = *replay++; else t = lex();`), the same
  shape and cost as Lua 4's lookahead check. Also needs only one parser.
  Prefer it only if a token buffer exists anyway (D1 comparison).
- **(c) Element expression into a mini-AST.** Only needed if D1 → (b).
- **(d) Python order, but type the comprehension variable in the syntax**
  (`[x*3 for int x in nums]`). Doesn't solve it: the element expression is
  still parsed before the binding is seen.

**Recommendation: (b), lexer rewind, keeping Python order** (user leaning
toward this). (a) remains the fallback if (b) turns out fiddly in practice.
Section 4 shows (b) is the only non-LL(1) mechanism in the design, and it's
local to `[`/`{`.

**Implementation (2026-11-12).**

- A checkpoint is a copy of `LexPos`, the part of `LexState` that changes
  as the lexer advances: the cursor, the current token, indent, pending
  dedents and bracket depth. The interpolation stack (`interp_sp`,
  `interp_stack`) isn't saved, so a checkpoint has to be restored at the
  same depth of interpolation that it was made at. That's true for
  comprehensions, because the mark and both rewinds are inside the same
  `[`. The exception is a file that ends inside an open `\(`, where the
  skim stops at EOF one level deeper, but that can never parse, and it
  gives the same error. `lex_mark()` and `lex_rewind()` in lex.c are the
  only way to make and use one.
- The lexer also writes to tables in `Ctx`. Interning is idempotent, so
  re-lexing a name gives the same `NameId`. An escaped string is decoded
  into `Ctx.strs`/`Ctx.strbytes` each time it's lexed, so re-lexing appends
  a duplicate with a new index. These aren't truncated on rewind: the
  `for` clauses are lexed after checkpoint E and may hold string indices
  past it, so reusing those slots for the element would overwrite them.
  The duplicates are bounded by the size of the first element.
- The skim calls `lex_next()` until a `for` or `,` at the literal's own
  `bracket_depth`, until the depth drops below it (the `]`), or until end of
  file. The lexer already tracks the depth, including through `"\(...)"`.
  The skim never reports parse errors. If it stops anywhere unexpected, the
  parser rewinds and parses normally, so the error is the usual one. Lexer
  errors are reported during the skim, but they would be reported at the
  same place by the normal parse.
- A comprehension opens a scope (a saved `num_locals`). Each loop variable
  is declared as a new local even if the name is already bound, so it
  shadows an outer `x` (`compr_shadow.luv`). `pop_locals()` closes the
  scope after the jump to R.
- Between E and R, no parser state is saved or restored. When the element is
  parsed, the loop variables are bound and nothing else has changed, which
  is the state it needs. Nothing inside an expression declares a name.
  Nested comprehensions keep their checkpoints on the C stack.
- Errors in the `for` clauses are reported before errors in the element,
  so `[undefined_a for x in undefined_b]` reports `undefined_b`. A test
  records this order.
- In debug builds, the skim saves the lexer state at the `for` it stopped
  at, and it's compared with the state when the element parse reaches the
  same `for`. Both lexed the same text from E, so they should be equal,
  and lexer state added outside `LexPos` is caught when it's first used.
  `lexdump -r` (run by `./m test`) does the same check at every `(`, `[`
  and `{` in the tests: it lexes past the close, rewinds, lexes again and
  compares.

Dict comprehensions (`{k: v for ...}`) would use the same mechanism with
`{`/`}`.

Background: Python's order is **syntactically** LL(1). The old pgen grammar
was `listmaker: test ( list_for | (',' test)* [','] )`, so after the first
element, `for` vs. `,` decides between a comprehension and a literal. That is
why PEP 202 rules that `[x, y for ...]` must be written `[(x, y) for ...]`.
Python never had the binding problem because it builds a tree and resolves
names in the compiler. The expression-first order comes from set-builder
notation (via SETL/Haskell, `{f(x) | x ∈ S}`). The 1999–2000 debates were
about the separator (`|`, `:`, `for`), not the order. So for us the problem
is purely single-pass emission, not grammar.

### D11 ★ Forward references (functions and types used before they are defined)

A typed compiler needs the callee's return type to type-check
`x = helper()`, and a struct's layout to use it by value. Lua 4 doesn't care
because globals are dynamic.

**What the luv60 tests actually need (scanned all of `test/`, `test/errors/`,
`stdlib/`):**
- **Zero** uses before definition. No top-level `def`/`on`/`struct`/
  `foreign` appears after `main` in any file. `memfn_self_references.luv`
  even defines `memfn_2` *before* `memfn_1`, which calls it. The suite is
  written define-before-use.
- **Zero** recursive or mutually recursive functions. (Two hits from the scan
  aren't recursion: `manual_list_int.luv` calls `int`'s `__repr__` from
  `ListData_int.__repr__`, and `memfn_shadow_foreign_name.luv` calls the
  foreign `free` from `Stuff.free`.)
- Imports aren't affected, because an imported module is compiled whole
  before its importer.

So requiring definition before use breaks no existing test. It's a question
of what the language should allow, not of compatibility.

**Recursion under define-before-use:**
- *Self*-recursion is free in a single pass. The return type comes before
  the name (`def int fact(int n):`), so the full signature is known at `)`
  and the symbol can be bound before the body is parsed.
- *Mutual* recursion and mutually referencing structs need a forward
  declaration. No new keyword is required: a header that ends at NEWLINE
  instead of `:` is a declaration (`def bool is_odd(int n)`, `struct Node`).
  After `)` (or after the struct name), `:` vs. NEWLINE is one token, so it's
  LL(1). luv already has the same shape in `foreign int f()`. Pointers to a
  declared-only struct work; using it by value needs the full definition.

Options:

- **(a) Define before use**, plus the forward-declaration headers above.
  Zero cost. C/Pascal-like discipline, and somewhat un-Pythonic for files
  that put `main` first.
- **(b) Eager top-level skim pass.** Before the real parse, lex only
  indent-0 lines (`def [type] name(params)`, `on T def ...`, `struct` with
  its field lines, `foreign`, `import`, top-level variables) and skip bodies
  by fast-forwarding while lines start with a space (a byte compare after
  each `\n`). Every file pays for it. On dumbbench that's re-lexing 1 in 7
  non-comment lines, plus a line-start scan of the whole 207 MB (which
  exceeds cache, so it's a second trip to DRAM). Rough estimate: 5–15% of
  parse time.
- **(c) Lazy skim on first miss.** Parse in order as in (a). The *first*
  time a global name or TYPENAME fails to resolve, run the (b) skim once
  over the rest of the file to build a `name → offset` index of top-level
  headers, then parse the needed signature (and, for a struct, its fields)
  on demand. Files written define-before-use, which is the whole test suite
  and dumbbench, pay **nothing**. Files that use forward references pay one
  skim. A truly undefined name pays the skim and then errors. Recursion and
  mutual recursion work without forward declarations, though the
  header-only declaration can still be allowed.
- **(d) Late patching**: emit unresolved calls and patch them afterward.
  Can't type-check `helper().field` before `helper` is seen. Rejected.

**Limits on the skim (so lazy lookup can't diverge from or hide errors in
the normal path):**
- *The skim never reports errors.* An indent-0 header that doesn't parse is
  recorded as "malformed at offset X" and skipped. Before the main parse
  reports "undefined name", it checks for a recorded malformed header. If one
  exists, it parses that header for real and reports *its* error instead,
  which is the root cause and matches what the in-order file would report.
  Duplicate definitions are indexed silently. The main parse reports them
  when it reaches the second one, as usual.
- *One header parser.* The skim calls the same `funcsig`/`struct`-field code
  as the main parse, at a different offset, so a symbol is identical no
  matter which path created it. When the main parse reaches a header the
  skim already turned into a symbol, it reuses that symbol and parses only
  the body. It never defines it twice.
- *Declarations only, never values.* The skim finds only items whose type is
  fully written in the header: `def`, `on T def` (keyed `(T, name)`),
  `struct`, `foreign`, typed globals (`int x = 55`: address and type only).
  Untyped globals and constants (`x = 1`, `BLAH = 3`) get their type or
  value from evaluating an expression, so using them before their
  definition stays an error.

**Recommendation: (c).** It has the speed of (a) for code that's already in
order and the convenience of (b) for code that isn't. It keeps the main
parse single-pass. The cost is a resumable "parse just this header at
offset X" entry point, which (b) needs anyway. A struct's field types may
trigger further on-demand lookups, so the parser needs a cycle check for
by-value self-containment. If (c)'s bookkeeping turns out to be fiddly, (a)
is the fallback: no tests need changing. The incremental "top-level
chunker" from `misc/notes.txt` (sha per top-level item) is the same
header index as (b)/(c), so this work could feed it later.

### D12 Nested functions and captures

luv captures **by value at definition time** (`nested_simple_upval.luv`,
`nested_capturing_func_with_upval.luv`). Lua 4.0.1 had the same semantics,
spelled `%name` and limited to the immediately enclosing function
(`pushupvalue`, `lparser.c:229`).

- Implicit capture (current luv): the parser walks the `FuncState->prev`
  chain like `search_local` (`lparser.c:187`) and records upvalues per
  function. LL(1) either way, since this is name resolution.
- Explicit `%x` (Lua 4): cheaper analysis, uglier.

**Recommendation: implicit**, with multi-level capture as luv already
supports (`nested_two_levels.luv`). Use Lua's `FuncState` chain directly.

### D13 Literals

| Item | Options | Recommendation |
|---|---|---|
| Integers | dec / `0x` / `0o` / `0b`, `_` separators, suffix `i8..u64` | keep luv60's set. Accumulate inline. Check overflow per suffix in the lexer |
| Floats | `1.0`, `1e9`, `1.5e3`; C-style `1.f`? | a float needs a digit after `.` or an exponent. `1.` and `.5` are errors. Suffixes mirror ints: `1.0f32`, `1f32`, `2.5f64` (optional alias `f` = `f32`), decimal only (`0x1f` is hex 31). **No C-style `1.f` / `1.e5`**: with methods on scalars (`on int def f`), `1.f` is ambiguous with a method call, so `1.foo()` stays int + method. The `a.b` vs `1.0` trouble in luv60 (`lex.c:384`) comes from its parallel indexer. A sequential number reader owns the `.` after a digit, with byte lookahead made safe by padding. Lexer calls strtod only for floats |
| Strings | `"..."` only, or `'...'` too | `"` only. Then `'` stays free (for codepoint literals, maybe) |
| Escapes | `\n \t \\ \" \0 \xHH \u{...}` | scan to the closing quote. If no `\` was seen, the string token is a **slice of the source** (zero copy). Otherwise decode into the arena (Lua 4 always copies through `Mbuffer`) |
| Raw / multiline strings | Lua `[[...]]` | **not `[[`**: it collides with nested arrays/lists. If needed, use `r"..."` and `"""..."""` |
| Interpolation | `fmt("...{}", a)` call vs. `f"...{a}"` vs. Swift `"...\(a)"` | **Decided: Swift `\(expr)`; `fmt(...)` dropped.** luv60's trouble was its SIMD quote-pairing indexer, which treats string contents as opaque. A sequential lexer uses a mode stack, as Swift, Kotlin and JS templates do: the string reader returns `STRING_BEGIN` at `\(` and pushes the bracket depth onto `interp_stack`. Normal tokens follow (nested strings are fine). The `)` that returns to that depth pops it and resumes the string, returning `STRING_MID`/`STRING_END`. A string with no `\(` is still one zero-copy `TK_STRING`. Grammar: `STRING \| STRING_BEGIN expr {STRING_MID expr} STRING_END`, LL(1), and the token kind says up front whether a string is interpolated. Cost: one compare per `)` while the stack is non-empty. Rules: a raw newline inside a string, including inside `\( )`, is an error, so strings stay atomic for the formatter. A literal `\(` is written `\\(`. Deferred: format specs (a lexer-only `\(x:08x)` is possible because nothing else uses a depth-0 `:` there). Test impact: 17 tests use `fmt(` and get rewritten. The `errors/fmt_*` tests (6) go away. Cost elsewhere: editor highlighting needs nested regions |

### D14 Comments

- `#` to end of line, skipped with `memchr('\n')`.
- **Trailing comments** (`x = 1  # why`): luv60 rejects them
  (`TOK_INVALID_TRAILING_COMMENT`) because of the SIMD indexer. A scalar
  lexer handles them for free. **Allow them.**
- Block comments: none.

### D15 Interning and symbol lookup

- One global intern table (open addressing, power-of-two size, keyed on
  `(hash, len, bytes)`) holds both keywords and identifiers, the same as
  Lua 4's `marked`. Each `Name` record has: `kind` (keyword id or `NAME` /
  `TYPENAME`), `len`, `hash`, a pointer to the bytes in the source, and a
  **`binding` pointer**.
- Name resolution via the `binding` pointer: each declaration pushes the
  previous `binding` onto a scope undo-stack and points `binding` at the new
  symbol. Leaving a function pops it. Lookup is one load, versus Lua 4's
  linear `actloc` scan or luv60's per-scope dicts (`notes.txt` profiling
  shows scope lookup and `str_intern` as hot).
- **Alternative to benchmark in step 3: Lua 4's fixed per-function locals
  table.** Lua's `FuncState` has fixed arrays (`int actloc[MAXLOCALS]`
  with 200 entries, `expdesc upvalues[MAXUPVALUES]` with 32), so starting
  a function allocates nothing and nothing grows. `search_local`
  (`lparser.c:187`) scans `actloc` backwards comparing interned pointers,
  then repeats in each enclosing function. Of Lua's limits, these two are
  the ones that help the front end. The others (131k functions per chunk
  from `OP_CLOSURE`'s 17-bit A field, `MAXSTACK 250` from the 9-bit B
  field, the constant-table limit from the 26-bit U field,
  `LFIELDS_PER_FLUSH`) exist for the fixed 32-bit instruction format and
  do nothing for parse speed, so we shouldn't copy them unless our output
  format forces it. `MAXVARSLH`/`MAXPARAMS` cap C-stack recursion in
  multiple assignment, which we don't have.
  - Our scoping makes the scan unusually good: scope is the whole function
    (D6) and captures are by value (D12), so a lookup searches only "this
    function's locals, then globals", never walking enclosing functions.
    The scan stays in a small hot array, while `Name.binding` costs a
    load from the `Name` entry (often still in cache, since the lexer just
    interned it) plus an undo-stack push per declaration and a pop pass at
    function end.
  - Cost of the scan: a hard per-function cap (e.g. 256 locals) with an
    error to match, and lookup that's linear in the local count.
  - Measure both. Dumbbench has three locals per function, which flatters
    the scan, so also bench something with many locals per function.
  - **Result (2026-11-08): chose `Name.binding`.** Both were implemented
    for locals only (binding step 1, with no globals, captures or errors
    yet), and they resolved the same names on every test file (3.75M
    local lookups on dumbbench). `scratch/gen_locals.py` generates the
    many-locals inputs (N locals per function, names drawn from a shared
    pool). Hyperfine means, measured on the laptop:

    | input      | HEAD (no lookup) | `Name.binding` | scan     |
    |------------|------------------|----------------|----------|
    | dumbbench  | 476 ms           | 484 ms         | 494 ms   |
    | locals4    | 82.3 ms          | 83.0 ms        | 90.8 ms  |
    | locals16   | 69.4 ms          | 69.5 ms        | 85.4 ms  |
    | locals64   | 66.1 ms          | 66.2 ms        | 97.4 ms  |
    | locals256  | 66.1 ms          | 66.1 ms        | 170.2 ms |

    The scan loses even at 4 locals, and its cost grows linearly as
    expected. `Name.binding` costs nothing measurable on the many-locals
    files, and about 2–3% on dumbbench (+8–15 ms over several runs).
    That's about the same size as the laptop's run-to-run noise (±10–17
    ms, possibly battery or thermal throttling), so it should be
    re-measured on the desktop. For reference, Lua 4.0.1 on dumbbench:
    1.584 s full `luac`, 1.182 s with code generation stubbed out, 0.821 s
    lex only (builds in `scratch/lua-4.0.1-nocode`).

    Re-measured on the desktop (Ryzen 9 5900XT, Linux, clang 18 `-O2`,
    pinned with `taskset -c 6`), comparing 8b968df with e61a77e. The scan
    wasn't rebuilt there.

    | input      | HEAD (no lookup) | `Name.binding` |
    |------------|------------------|----------------|
    | dumbbench  | 640–651 ms       | 649–653 ms     |
    | locals4    | 104.2 ms         | 106.1 ms       |
    | locals16   | 85.7 ms          | 87.9 ms        |
    | locals64   | 81.3 ms          | 82.9 ms        |
    | locals256  | 80.5 ms          | 82.4 ms        |

    σ is 4–14 ms on dumbbench and under 1 ms on most of the locals files.
    On dumbbench, binding costs 1–8 ms (0.2–1.3%) across four runs. On the
    locals files it costs a steady 1.5–2.4 ms (about 2%), which the
    laptop's noise was hiding. System time is about 30% of the total on
    Linux (about 195 ms on dumbbench, versus 37 ms on the Mac), so the user
    time comparison is the closer one.
  - As implemented: a single `Local locals[1024]` array in the `Parser`,
    shared by enclosing functions, with `func_base` marking the current
    function's first entry. Each `Local` saves the shadowed binding, so
    the array doubles as the undo stack. Name indices are a `NameId`
    struct so they can't be mixed up with offsets. Bindings are undone at
    the end of `parse_file()`, including after an error, because `Name`s
    are shared by every file parsed with the same `Ctx`.
- Hash: something cheap over the bytes already being scanned (FNV-1a,
  or wyhash on ≤16-byte loads given the padding).

### D16 Errors and positions

- Track `line` in the lexer. It's free because indentation handling visits
  every `\n`. Each token carries a `u32` byte offset. Compute columns lazily
  on error, like luv60's `get_location_and_line_slow`.
- First error → `longjmp` to the driver (Lua's model). No recovery. If editor
  integration later wants multiple errors, the top-level skim (D11) already
  gives natural resync points: indent-0 lines.
- Keep `check_match`'s "`)` expected (to close `(` at line N)".

### D17 Top-level grammar vs. nested grammar

luv60 uses one `parse_statement(bool toplevel)` with `if (toplevel) error(...)`
checks (`parse.c:5637`). Proposal: **two functions**, `toplevel_stat()` and
`stat()`, each with its own `switch`. Each stays smaller and gives better
error messages, and "imports must come first" becomes part of the grammar:

```
module -> { import } { toplevel_stat } EOS
```

Nested `def` stays legal inside functions. `struct`, `on`, `foreign` and
`import` are top-level only.

### D18 Things that are easy to make non-LL(1) by accident (future syntax)

| Future feature | Trap | LL(1)-safe form |
|---|---|---|
| Methods without `on` | `def Stuff.method()` vs. `def mod.Type func()` needs 2–3 tokens | keep `on T def name(self...)` |
| Generics | `List<int>` vs. less-than, `List[int]` vs. indexing | `{}` (D21): `{` after a name or type always opens type arguments. This works only because of D4 |
| Function types | `def(int) int` in type position | `def` is a type FIRST token only in type position, never at statement start, where `def` is a definition |
| Prefix deref | `*p = 1` vs. `*int p` | postfix deref (D5) |
| Tuples / parenthesized targets | `(a, b) = ...` | don't add. Use D5b's `a, b = ...` |
| Decorators | `@name` before `def` | `TK_DECORATOR` token, LL(1) prefix |
| Enums | `Color.RED` | `TYPENAME '.' NAME` in expression position, which `VTYPE` handles |


### D19 Formatter-friendly syntax choices

The constraints themselves are in section 9. Two choices there affect the
grammar:

- **D19a: trailing commas.** Decided: allowed in list and dict literals
  only, not in calls, parameter lists or compound literals.
- **D19b: adjacent string literal concatenation** (`"abc" "def"`).
  Decided: **no**, because of the accidental missing-comma concatenation.
  Revisit with something explicit if long strings become a problem.

### D21 Generics

Scope: template-style generics only. Structs take type parameters, and
methods use their receiver's (see the 2026-11-13 revision). No constraints, no value parameters (`Array{int, 4}`), no
specialization. Each distinct instantiation is monomorphized once, on first
use. The body is parsed once at the definition without emitting anything, so
syntax errors are reported at the definition. Each instantiation then rewinds
the lexer to the body and re-parses it with the type parameters bound, using
the same rewind as D10.

**When instantiation happens (2026-11-12, not implemented).** Re-parsing a
body where it's first used, in the middle of another function, would see
that function's locals through `Name.binding`, and its `func_depth`,
`type_params` and return type state. Saving and restoring all of that
means a full parser snapshot, and any field later added to `Parser` could
break it. Instead, instantiations are queued (the body's offset and the
type arguments) and done after the current top-level item ends. The parser
state is then nearly empty (no locals, `func_depth` 0, `type_params` 0), so
an instantiation needs a lexer checkpoint, the type parameters bound to the
arguments, and a flag so the function isn't declared a second time. The
call site already knows the return type from the signature, so the call
can be emitted before the body. Instantiations found while instantiating
are added to the queue. A `static_assert` on `sizeof(Parser)` next to this
code would force a decision about each new parser field. The parser doesn't
need any of this until there's type checking or codegen.

Open: this isn't enough for a generic defined in another module. The
imported module is finished before the importer starts, so its globals
are no longer bound (`Name.global` is per file and cleared in
`parse_end()`). Re-parsing its body in the importer would resolve names
against the importer's globals: a private helper of the defining module
would be undefined, and a name both modules define would bind to the wrong
one. The body needs the defining module's global scope. Either the
definition parse records what each non-local name resolved to and the
instantiation uses that, or globals become per-module tables that can be
switched to, which ties in with replacing `Name.global` with real global
records. Within one module, deferring is enough: globals can't be
redefined, and a name defined after the generic but before the
instantiation was already reported as undefined by the definition parse.

**Decided (2026-11-08, provisional): `{}`.** The user expects to revisit it
after writing code that uses generics.

```
struct Pair{A, B}:
    A first
    B second

List{[]int} xs = ...
p = Pair{int, str}(.first=1, .second="x")
on Pair{A, B} def A first(self): ...
```

Generic functions (`def T max{T}(...)`, `max{u64}(a, b)`, `f = max{int}`)
were in the original decision, and were removed on 2026-11-13.

Why it's LL(1): a `{` can't otherwise follow a name, a field or a type,
because dict literals and the `{K}V` dict type only appear at the start of an
expression or type. So `{` after `TYPENAME`, `name.TypeName`, a NAME or an
lvalue always opens type arguments. That holds in type position (`type()`)
and in expression position (`postfix()`, where `SH_TYPE`, `SH_NAME` and
`SH_LVALUE` can be followed by type arguments). Explicit call-site type
arguments are decided in `postfix()`, so there's no precedence problem. `{`
already counts as a bracket in the lexer, so long argument lists can wrap
across lines. `}}` is two tokens, so nesting needs no special case. Type
arguments are types only, never expressions.

**Revised (2026-11-12): type parameters are single capital letters.** A
type parameter is exactly one capital letter, `[A-Z]`, and a single capital
letter is always a type parameter. Struct names need at least two letters
(`Elem`, not `T`). The syntax above is unchanged.

- Every type parameter is still declared in a `{...}` list: on the struct
  (`struct Map{K, V}:`), on the function (`def T max{T}`), or on a method's
  receiver (`on List{T} def ...`). The list gives the order of explicit
  type arguments (`Map{str, int}`, `max{u64}(...)`). A single capital that
  isn't declared by an enclosing list is an error ("Undefined type parameter
  'U'."), including in a body. A nested function sees its enclosing
  function's parameters.
- Binding is single-pass. Struct fields and function parameters and bodies
  all come after the list that declares their type parameters. Only a
  function's return type comes first. Because a type parameter is known from
  its spelling, the return type doesn't need resolving where it's parsed.
  The parser records each one used in a 26-bit mask (bit `c - 'A'`), and
  checks the mask against the declared parameters once the list (and the
  receiver, for a method) has been parsed.
- Type parameters and real types can't be confused, so "Undefined type"
  never needs to know about generics. Generic code is also easy to spot
  when reading. The cost is losing descriptive parameter names (`Elem`,
  `Key`), which would be checked as real types instead.
- The lexer classifies a name once per distinct identifier (D4), so telling
  `T` from `Tree` costs nothing at each use.
- Single capitals are currently `TK_TYPENAME`, not constants, so no constant
  spellings are lost. No test here or in luv60 has a single-letter struct.

**Revised (2026-11-13): no free generic functions.** A method is a free
function with nicer syntax and a namespace, nothing more: `xyz.f(5)` is
`Xyz.f(&xyz, 5)`, and `Xyz{int}.f` is a nicer spelling of a mangled
`Xyz$int$_f`. So a method on a generic struct already is a generic
function, and instantiating its body is the same work as instantiating a
free generic function. What this revision restricts is where type
arguments come from.

- Type parameters are only declared by a struct's list
  (`struct Pair{A, B}`) or a method's receiver (`on List{T}`). Functions
  and methods have no list of their own. A method's type parameters are
  exactly its receiver's: `on List{T} def append{T}(...)` would be a
  redundant second list, and `on List{T} def map{U}(...)` adds a parameter
  the receiver can't supply, which makes it a free generic function again.
- So type arguments are never inferred from function arguments. In
  `xs.append(1)`, `T` is read off the declared type of `xs` (`List{int}`,
  or `*List{int}` for `[]int`), which is a one-sided structural match. In
  `List{int}.new(5)` they're written on the type. This removes inference
  across arguments, literals taking their type from the other arguments
  (`max(x, 1)`), and the error for mismatched concrete types.
- Type arguments only follow a type: `{` after a name or a value in
  `postfix()` is an error, so `max{u64}(...)` and `f = max{int}` are gone.
- A method's return type comes after its receiver, so type parameters are
  always declared before they're used, and the 26-bit return type mask
  described above isn't needed.
- Instantiation is unchanged: method bodies are still re-parsed per
  instantiation, queued to the end of the current top-level item, and the
  cross-module problem is still open. It matters early, because the most
  used generic methods are in the prelude (`[]T` is `*List{T}`).
- Lost: functions with no natural receiver. `max`/`min` can be builtins,
  or methods on the builtin types in the prelude (`a.min(b)`, D22a).
  `sort(xs)` becomes `xs.sort()`, `make{T}(n)` becomes `List{T}.new(n)`,
  and comprehensions cover most of `map`/`filter`. `swap{T}(*T, *T)` and
  helpers that span two generic types (`zip` over `List{A}` and `List{B}`)
  have no good form, and need concrete versions.
- Adding free generic functions back later doesn't break any code. The
  next step would be free generic functions that always take explicit type
  arguments (`swap{int}(&a, &b)`), which still needs no inference, but
  brings back `{` after a name in `postfix()`. Inference would come after
  that, if it's needed.

Rules in the prototype (the parts about function lists, the return type
mask and call-site inference are superseded by the 2026-11-13 revision):

- Type parameters (`struct Pair{A, B}`, `def T max{T}`) must be single
  capital letters (see the revision above). Builtins, longer type names and
  `mod.Type` are rejected.
- Generic function parameters come after the function name, matching
  structs. The return type can use `T` before `{T}` declares it, and is
  checked after the list with the mask described above.
- `foreign` functions can't be generic.
- Type arguments can't be given twice (`List{int}{str}`), and can't follow
  a call or other value (`f(){int}`).
- No trailing comma (D19a) and no empty `{}`.
- Default at call sites: type arguments are inferred from the arguments.
  Literals take their type from the other arguments (`max(x, 1)`).
  Different concrete types (`u32` and `u64`) are an error, fixed by `cast`,
  a literal suffix, or explicit `max{u64}(...)`.
- Open semantic questions: what `on Pair{A, B}` binds, whether `on
  Pair{int, int}` (specialization) is allowed, and whether a type argument
  can be inferred from the expected type (`List{int} xs = make()`), which a
  single pass does know first.
- Open (2026-11-12, mostly moot since 2026-11-13, when functions lost
  their own lists): an error for unused type parameters. In a function's
  own list (`def f{T}()` with no `T` in the return type, parameters or
  body), every instantiation is the same code and `T` can't be inferred, so
  it's probably a leftover. It would be checked at `func_close()` with a
  mask of the parameters used, next to the declared mask. A use in a nested
  function counts for the enclosing one, and a use only in the return type
  (`def List{T} make{T}(int n)`) counts. A method's receiver list is exempt,
  because it mirrors the struct's and most methods don't need every
  parameter. A struct's list is exempt too, because an unused parameter
  makes a tag type: `struct Id{T}: int value` gives `Id{User}` and
  `Id{Order}` as distinct types with the same layout.
- Not covered by tests yet (2026-11-12), since type parameters became their
  own token (`TK_TYPEPARAM`): `mod.T` is rejected ("Expected type name after
  module name, found type parameter 'T'."). `T{int}` as a type is an error,
  but `T{int}(...)` in an expression is still accepted by postfix.
  `on List{Elem}` reports "Methods can't be defined for specific type
  arguments."

Known wart: braces now mean both generics and dicts. `{K}V` inside type
arguments reads badly: `List{{str}int}`. The user's inclination is to later
replace both `[]T` and `{K}V` with more verbose generic synonyms (e.g.
`Slice{T}`, `Dict{K, V}`), which would remove the overlap. Superseded
(2026-11-12): `[]T` and `{K}V` are kept as synonyms for `*List{T}` and
`*Dict{K, V}`, pointers to ordinary prelude structs, so user code rarely
needs to nest them inside `{}`. See D23.

Options explored:

- **`<>`** (`List<int>`, `struct Pair<A, B>`, `def T max<T>`). Usually
  rejected because `<` is ambiguous with less-than, but D4 fixes most of
  it. In type position there's no ambiguity. In expression position, `<`
  after `SH_TYPE` is currently always an error (types can't be compared), so
  it can open type arguments. `Color.RED < x` and `MAX < x` stay
  comparisons because their shape isn't `SH_TYPE`. The `,` ambiguity in
  `g(a < b, c > (d))` doesn't arise either, because the type-argument list
  is already open before the `,`. Costs:
  - `List<List<int>>` lexes `>>` as SHR. Fixable in the parser: where a
    `>` is expected and the token is SHR, rewrite it in place to GT with
    offset+1 instead of calling `lex_next()`. `>=` and `>>=` can't validly
    follow a type-argument list.
  - `<` isn't a lexer bracket, so newlines inside `<...>` are layout tokens
    and long argument lists can't wrap.
  - **Explicit call-site type arguments don't work.** In `max<int>(a, b)`,
    `max` is a NAME, so `<` can't be decided from one token. Deciding after
    parsing the operand (as `sizeof_operand` does) works for acceptance, but
    the `<` is reached inside the high-priority operand: in `a + f < b`, `b`
    would be parsed and emitted before the `+`, which breaks single-pass
    emit. Knowing which names are generic functions (C++) needs symbol
    information from imports, which breaks section 9's one-file formatter.
    So `<>` means inference only, with non-inferable cases as static methods
    (`List<int>.new()`), or Rust-like `max.<int>(a, b)` (LL(1) since `.` `<`
    is otherwise unused, but ugly).
- **`[]`** (`List[int]`, `max[int](a, b)`), the earlier D18 plan. LL(1)
  everywhere: after `SH_TYPE`, `[` is currently an error, and for function
  calls the decision is made inside the brackets with the `sizeof_operand`
  first-token rule, with no precedence issue. Rejected for looks:
  `List[[]int]`, `Map[str, [4]u8]`.
- **`'`** (`List'int`, `Pair'(int, str)`, `max'u64(a, b)`) and **`!`**
  (`List!int`, `Pair!(A, B)`, as in D). Both characters are otherwise
  unused (strings are `"` only, and a lone `!` is an error). LL(1). Short for
  one argument, but several arguments need a grouping form, and then struct
  literals read badly (`Pair'(int, str)(1, "x")`). `'` also confuses most
  editors' highlighting until luv has its own syntax definition.
- Placement of function type parameters: `def T max{T}(...)` (chosen,
  matches structs) vs. `def{T} T max(...)`, which declares `T` before the
  return type uses it. Structs could likewise be `struct{A, B} Pair:`.
  Both are LL(1) (`def {` and `struct {` are otherwise errors).
- Inference only (no explicit call-site syntax at all) was considered
  acceptable and doesn't block adding explicit syntax later. `{}` makes the
  question moot since explicit arguments come for free.
- **Implicit `$T` parameters** (2026-11-12): `def $T max($T a, $T b)`, with
  the sigil marking a type parameter and the first use declaring it, and no
  `{T}` list on functions. Single-pass and LL(1) (`$T` is one token). It was
  rejected because explicit type arguments would then need an order taken
  from first appearance, which silently changes if parameters are reordered,
  and because structs would still need a list. It's also noisier. `'T` was
  considered for the same idea and is worse, because an unpaired `'`
  confuses editors and other tooling.
- **Alphabetical order for explicit type arguments** (2026-11-12): with
  single-letter parameters, `Pair{int, str}` could bind `A`, `B` in
  alphabetical order, so no lists are needed at all. Rejected as too subtle.
  `{K, V}` only works by accident of the alphabet, and something like
  `{V, K}` couldn't be written.

### D22 Methods (`on`) and overloading

`on T def name(...)` defines a function looked up by (receiver type, method
name). The receiver's static type is known when `.name` is reached, so
lookup is one exact probe and nothing is ranked. This is overloading on the
first argument only, and it's kept to that.

Decided (2026-11-08):

- **Key.** A method on a generic type is keyed on the generic type itself:
  `on List{T} def len` is stored under (`List`, `len`). `xs.len()` with `xs`
  a `List{int}` probes (`List`, `len`) and instantiates it with `T = int`.
  Builtin compound types key the same way (`[]T` under slice, `{K}V` under
  dict).
- **No specialization.** `on List{int}`, `on []u8` etc. are errors. The type
  arguments of a receiver are always new type parameters, so it doesn't
  matter whether their names match the struct definition, and the question
  of whether `T` refers to an existing type doesn't come up. Specialization
  is rarely needed for restricting methods to some types: a method body is
  only type-checked when it's instantiated, so `on List{T} def T sum(self)`
  works for numeric `T` and is an error only when called on `List{str}`.
  Type-specific operations like `decode_utf8` are free functions.
- **No methods on pointers** (`on *T`), because `.` auto-dereferences, so
  `p.f()` would be ambiguous. No methods on `[N]T` (would need a value
  parameter, D21) or `typeof(...)`.
- **Methods can only be defined in the receiver type's own module** (option
  (c) below). `on mod.Thing` is a syntax error. Dunder methods are
  therefore also only defined with the type, so a module can't change how
  another module's type prints. `memfn_on_basic_type.luv` (a method on
  `int` in user code) was removed.
- **No free-function overloading.** Methods on a generic struct cover
  "same code, many types", and methods on different types cover "different
  code per type". Method names don't enter
  module scope, so a free `free` and a method `Stuff.free` coexist
  (`memfn_shadow_foreign_name.luv`).
- **Static methods**: a method whose first parameter isn't a bare name has no
  `self`. `on List{T} def List{T} new(int cap)` is called as
  `List{int}.new(4)`. This is how a function that would have been a free
  generic function is written (D21, 2026-11-13).
- **Operators** on structs map to dunder methods (`<` to `__lt__`) through
  the same lookup. That's a spelling for `<` on that type, not overloading.
- **Errors inside instantiations** must stay readable. Report the error in
  the generic body with the instantiating call site and type arguments, and
  avoid deep "instantiated from" chains.

Receiver grammar enforced by the parser:

```
receiver -> TypeName [ '{' TypeName { ',' TypeName } '}' ] | builtin
          | '[' ']' TypeName | '{' TypeName '}' TypeName
```

Options considered for where methods can be defined:

- (a) Methods on a struct only in its module. Methods on builtin types
  anywhere, visible only where the defining module is in scope.
- (b) Anywhere, one global key, an error when two conflicting definitions
  are visible.
- **(c) Only in the type's own module. Builtin types get methods only from
  the module that defines them.** Chosen.

D22a (open): what (c) means for builtin types. The "module that defines
builtin types" is a prelude module that's implicitly imported everywhere.
Builtin receivers (`on int`, `on str`, `on []T`, `on {K}V`) are accepted by
the parser but are a semantic error outside the prelude. The prelude is
ordinary luv code. What's special is only that it's the home module of the
builtin types, plus whatever intrinsics or `foreign` declarations it needs
for primitive operations. Other library modules can't add methods to `str`
either, and provide free functions instead. If `[]T` and `{K}V` are later
replaced by generic structs (`List{T}`, `Dict{K, V}`, see D21), those become
ordinary structs defined in the prelude, and only true primitives (`int`,
`str`, ...) are left needing the special case.

Methods on primitives are one definition per type, because `on T` isn't a
receiver: `on int def int min(self, int b)`, `on u64 def u64 min(...)` and
so on, in the prelude. `a.min(b)` and `(3).min(b)` parse already. The
static form `int.min(a, b)` doesn't, because a builtin type can't start an
expression.

### D23 Arrays, lists and slices (open)

Paused on 2026-11-12 to think through, because how this memory is managed,
and what the user has to do with it, will change later decisions and syntax.

The questions:

- What `a = [1, 2, 3]` is. In luv60 it's a fixed array `[3]i32`, so
  `a.append(4)` is an error ("Undefined member function append on type
  [3]i32.", `array_no_append.luv`). An owning list needs a declared type
  (`[]int nums = [1, 2, 3]`). The user wrestled with this in luv60 without a
  satisfying answer.
- What a slice `a[i:j]` (D8) is: a copy, or a view into `a`. In luv60 it
  was a view of some kind. `list_slice_append.luv` expects `nums[2:]` of a
  `[]int` to fail a runtime check on `append`.
- What a comprehension produces, and from what. `[x*2 for x in xs]` over an
  array, a list, a slice or `range`. The existing errors
  (`compr_array_from_non_array`, `compr_filter_array`,
  `compr_array_from_diff_sized_array`) show where luv60 drew the boundary
  between arrays and lists.
- Who owns each of them and how they're freed. There's no decision on
  memory management at all yet. Go can hide the difference between an
  owning list and a view partly because it's garbage collected.
- The spelling. `[]T` was chosen to look like Python, so it owns its storage
  like `std::vector`. In Go, Zig and Odin `[]T` is a view (a slice), which
  may confuse readers, and a view type is needed regardless.

What's agreed so far:

- The compiler is closely tied to the implementations of `List` and `Dict`,
  because of literals and comprehensions. It knows `List{T}`'s layout and
  builds literals and comprehensions directly, and preallocates when the
  length is known. The methods (`append`, growth, dunders) are luv code in
  the prelude (D22a). `Dict` literals probably call a prelude insert
  function rather than the compiler knowing the hash table.
- `[]T` and `{K}V` are transparent synonyms for `*List{T}` and
  `*Dict{K, V}`, where `List` and `Dict` are ordinary prelude structs (see
  "Values and references" below). The compiler knows about them only to
  make code less noisy. Diagnostics print the short form. This replaces the
  inclination in D21 to replace them with verbose synonyms.
- Fixed arrays `[N]T` stay primitive value types.

#### Option A (explored 2026-11-12): regions, a current pool and scratch space

This is one coherent option, written down so that the details can be worked
through. Nothing in it is decided. It started from Goose
(aardappel.github.io/goose), which keeps each growable array alone at the top
of its own bump-allocated stack, so it grows in place and never moves, and
frees by resetting the stack when a scope exits. Goose assigns stacks and
checks lifetimes with whole-program analysis. That doesn't fit a single-pass,
per-file compiler (D1), so this option replaces both analyses with things that
can be done at runtime or checked inside one function.

**Regions.** A region is a large reservation of address space (Goose uses
2 GB) with a guard area at the end and a bump pointer. Each thread keeps a
free list of regions. Taking or returning one is a pointer pop or push and a
reset of the bump pointer, and a reused region already has committed pages.
Regions are always released in reverse order of acquisition, so the free
list plays the part of Goose's compiler-assigned stacks without any analysis.

**Lists in locals own a region.** A list whose lifetime is a local variable
(`[]int xs = []`, `ys = [x*2 for x in xs]`) takes a region when it's created
and gives it back when the function returns.

- It's the only thing in its region, so `append` is a store and a bump, with
  no capacity check and no reallocation. A list never moves, so growing it
  never invalidates a slice or reference into it.
- It doesn't use the current pool (below), because that may live much longer
  than the function. A function that's called a million times would
  otherwise leak a list per call until the pool ends.
- Returning a list hands its region to the caller, which takes no copy, no
  hidden destination argument and no return-value optimization. This matters
  for a single-pass compiler, which can't know at `[]str out = []` that `out`
  is the value that will be returned.
- Each region has one owner, so `ys = xs` is a move (`xs` can't be used
  afterwards, checked locally), with an explicit `copy(xs)` for two lists.
  Copy-on-assignment is the alternative.

**Annotations, when the user knows more.** Regions remove the usual reason to
annotate (reserving capacity to avoid reallocation), so annotations only
change where storage lives:

| what the user knows | syntax | where it lives |
|---|---|---|
| nothing (the default) | `[]int` | its own region, grows in place |
| never more than N | `[:64]int` | the frame, or inline in a struct, aborts past N |
| the size when it's made | `[n]int` (runtime `n`) | the frame, or a region if large |
| a compile-time size | `[4]int` | a value, as now |

`[:N]T` reuses slice syntax, where `a[:N]` means "up to N", and is LL(1)
because `:` can't start an array size expression. `[n]T` needs no grammar
change, only relaxing the rule that the size is a constant. All of these
should convert implicitly to the view type, so that functions that only read
take a view and don't care which kind they were given. That makes the
spelling of the view type more pressing, because it's the type that would
appear most often in function signatures.

**The current pool.** Allocations whose lifetime isn't a local's go into the
current pool. That includes objects behind pointers, strings and lists
stored inside structs or other lists, and anything a callee builds that the
caller keeps inside another structure. The current pool is a per-thread,
dynamically scoped variable, like a Common Lisp special variable, and callees
see the binding of their caller. It could be a thread-local pointer that's
saved and restored around a block, or a hidden argument. At program start it's
a pool that lives until exit, so simple programs work without thinking about
memory, and long-running code opts into narrower scopes.

```
with Pool() as p:          # makes a pool, makes it current, frees it at the end
    tree = parse(text)     # parse()'s allocations go into p

into p:                    # makes an existing pool current, doesn't free it
    names.append(make_name(i))
```

`with Pool()` owns its pool and `into p` borrows one. A plain `with p:` can't
tell its `__exit__` whether to free, so borrowing needs its own statement.
`in` is already membership and part of `for`, hence `into`. D3a's one-line
suites cover single statements (`into p: xs = load(f)`), and function-level
scope (D6) keeps `xs` visible after the block. Explicit placement methods
(`p.new(...)`, `p.list{T}()`) were considered and aren't wanted.

**Checking lifetimes inside one function.** Every view and pointer
expression has a root, tracked in the expression descriptor. A root is a local
owner, a parameter, static data, or a pool. Inside a function, "the current
pool" is either the innermost enclosing `with`/`into` in that function, which
is visible in the source, or otherwise the caller's current pool, which acts
like an implicit parameter.

- Values allocated into the current pool, including results of calls made in
  that block, get its root.
- A function may return values rooted in its parameters or in the caller's
  current pool. It may not return values rooted in a local or in a `with
  Pool()` inside itself.
- A value can be stored into a structure only if its root lives at least as
  long as the structure's. Two different unknown roots are conservatively an
  error. That's why `make_name(i)` above is made inside `into p:`, so its
  result is rooted in `p`.
- A call that returns a view is treated as returning something rooted in all
  of its view arguments, like Rust's lifetime elision but with no syntax for
  exceptions. No function body needs to be seen, only signatures, which the
  D11 skim provides for forward references.
- Slices keep their own length, and lists never move, so a slice of a list
  that later shrinks reads stale values of the right type, not freed memory.

**Scratch space.** Two cases use the same mechanism, a pool that's reset when
something ends, with only the result surviving.

1. *Statement temporaries.* A call whose result isn't assigned to a variable,
   returned or stored is a temporary, for example `print "x is \(x)"`, or
   `n = count(split(s))` where `split`'s result is only read by `count`.
   Such calls run with the current pool set to a per-statement scratch
   region, which is reset at the end of the statement. So all of the callee's
   allocations, including its internal garbage and its result, are discarded
   without bookkeeping. Calls whose result is kept run with the normal
   current pool.
2. *Declared scratch blocks.* The user marks a region of code whose
   allocations are temporary. Only the results are kept.

   ```
   []str top = []
   with Pool():
       words = split(text)
       counts = count_words(words)
       top = most_common(counts, 10)
   # words and counts are gone. top was copied out.
   ```

   The rule is that nothing allocated in a `with Pool()` block survives it,
   except by being copied out through a variable declared before the block.
   An assignment to such a variable inside the block copies the value's owned
   data (lists, strings, pointed-to objects) into the outer current pool.
   Variables first assigned inside the block die with it, and a later use is
   an error ("'words' was in scratch space released at line N."). This is
   checkable in one pass: at the end of the block, its locals are marked dead,
   as a scope is popped. A function can wrap its own work this way and return
   only its result, so the caller's pool gets the result and none of the
   garbage. Whether a separate spelling (`scratch:`, `with Scratch():`) is
   worth having is open.

**What's still open in this option:**

- Nested ownership: `[][]int`, structs that contain lists, lists of owned
  strings. Inner parts either go in the current pool or each own a region,
  and the latter doesn't scale to millions of small lists. `[:N]T` helps for
  bounded inner data.
- Whether a list declared inside `into p:` comes from `p` or still owns a
  region.
- How a single object is allocated without placement methods: `new(Node(...))`,
  or `&Node(...)` allocating in the current pool (it's an error today).
- What exactly copying out a value copies, especially views inside it whose
  roots are outside the scratch block.
- How strings are represented (owned bytes, views, or both), since built
  strings are the most common allocation.
- Address space: 2 GB regions allow about 64K live regions in a 47-bit
  address space, so deep recursion that keeps a list in every frame could run
  out. Smaller reservations, or Goose's rule against keeping growable data
  across a recursive call, would help.
- The program-lifetime default pool grows without limit in long loops that
  don't use `with Pool()`. Allocation statistics in debug builds would make
  that easy to spot.
- One thread-local read per allocation, which a function can load once.
- The current pool could grow into a context (temporary pool, logger), or
  user-defined special variables. Starting with only the pool keeps that
  open.

Option A became complicated, both to implement and as a model for the
programmer. In particular, copying a result out of a scratch block isn't
well defined once the result contains pointers. That led to option B.

#### Option B (preferred, 2026-11-12): one current arena

Every allocation goes into the current arena. There are no per-local regions,
no moves and no copying out. This is close to where luv60's thinking had got
to, stated more clearly, and it suits how early the language is.

The model for the programmer is one rule: everything allocated lives until
the arena that was current when it was allocated is reset, and arenas are
blocks.

```
with Arena():          # a new arena from the free list, current for this block and its callees
    ...                # everything allocated in here dies at the end of the block
into a:                # makes an existing arena current
    ...
```

- The current arena is a per-thread, dynamically scoped variable, as in
  option A, and starts as an arena that lives until exit. `with Arena()`
  takes an arena from the free list and releases it at the end of the block.
  `into a:` borrows one. D3a's one-line suites give a per-statement form
  (`into a: xs = load(f)`).
- Returning a value needs nothing. A callee allocates into its caller's
  arena unless it opens its own, so what it returns already lives there.
- Pointers between allocations are unrestricted within an arena, and from a
  shorter-lived arena to a longer-lived one. Nothing is ever copied out, so
  the question of what copying a pointer means doesn't come up.
- Lifetime checking is one local rule: a value allocated inside a `with
  Arena()` block can't be stored in, or returned through, anything that
  outlives the block. Roots are "the caller's arena" or a block's arena, and
  both are visible in the function. Leaving it unchecked, with debug builds
  poisoning a reset arena, is the fallback.
- The compiler uses the same machinery for its own allocations (the arena
  task for the compiler itself). Odin and Jai have essentially this model,
  with an allocator in an implicit context.

The caller's arena is named with `Arena.current()`, a prelude function that
reads the special variable. That's enough for the usual pattern of a scratch
arena plus an output arena:

```
def []str top_words(str text, int n):
    out = Arena.current()
    with Arena():                  # scratch
        counts = count_words(split(text))
        into out:
            return most_common(counts, n)
```

`with Arena()` always takes a fresh arena from the free list, so the scratch
arena can never be the same arena as `out`. That's a known pitfall when this
pattern is written by hand.

Costs, which are the next thing to work on:

1. Garbage accumulates until a reset. A function called in a loop leaves its
   temporaries in the caller's arena unless it wraps its work in `with
   Arena()`.
2. A list in a shared arena grows in place only while it's the most recent
   allocation. Otherwise it doubles and copies to the top, like
   `std::vector`. That's amortized O(1), but the old copies are garbage until
   the reset (typically about the final size again). A list can also move, so
   a pointer to an element taken before an `append` refers to the old copy.
   That memory is still mapped, so reads are stale rather than crashing, but
   writes through it are silently lost.
3. Peak memory depends on the programmer scoping temporary work. A long loop
   without a `with Arena()` just grows.

What it doesn't cost: allocation is still a bump, locality is often better,
and freeing is still free.

Storage annotations go on top of option B as opt-ins, rather than being the
default as in option A. `[:N]T` (bounded, inline) and `[n]T` (sized once at
runtime) carry over unchanged. A declaration could also opt a list into
option A's own region, so that a big, growing list never moves and leaves no
copies behind. The spelling is open. The statement-temporaries rule from
option A (calls whose result isn't kept run in a per-statement scratch arena)
also carries over unchanged, because it's mechanical and needs no copying
out. Whether to keep it is a separate choice.

Follow-ups agreed for option B (2026-11-12):

- Garbage until a reset is accepted. Automatically wrapping small pieces of
  list and comprehension work in a `with Arena()` was considered, but it
  needs escape analysis, and realistic code calls helper functions, which
  would have to be assumed to let everything escape.
- A list grows in place when it's still at the top of its arena. The check
  is only on the growth path (`len == cap`): whether the end of the list's
  storage equals the arena's bump pointer, which is a load and a compare.
- A local rule against appending to a list while a pointer or slice into it
  is still used was rejected, because a callee that does the append isn't
  covered. Stale pointers after a move are left to debug tooling instead:
  filling the old copy with a pattern, guard pages, or integrating with
  AddressSanitizer by poisoning the old copy and reset arenas with its
  manual poisoning interface.
- Per-arena high-water marks with the allocation sites that used the most,
  and an optional arena limit (`with Arena(.limit=...)`) that aborts past
  it, belong with the developer tools. That's around the same time as the
  formatter, alongside profiling tools for memory and performance, whose
  main job would be noticing a bad peak.
- Storage annotations: the user's real interest is assigning large blocks of
  memory to a subsystem. That isn't clear enough yet and is too early to
  design. `[:N]T` and `[n]T` remain as recorded above.

**Decided under option B (2026-11-12): everything is a list, and slices
copy.**

- `a = [1, 2, 3]` is a list (`[]int`), not a fixed array. A list costs a
  bump allocation and a header, so there's no longer a reason for literals to
  default to fixed arrays, and `a.append(4)` works. A fixed array is chosen
  by declaring one: `[3]int a = [1, 2, 3]`. `array_no_append.luv` no longer
  describes an error and changes when type checking arrives.
- `a[i:j]` makes a new list in the current arena, as in Python. Slicing
  never creates a view into a list's storage, so growing a list can't
  invalidate a slice.
- A comprehension produces a new list in the current arena. Into a declared
  array type (`[4]int all = [...]`) it fills the array and checks the size,
  which is what the `compr_array_*` error tests describe.
- Passing a list to a function passes the list itself, as in Python, so a
  callee's `append` is visible to the caller, because `[]T` is a pointer
  (see "Values and references" below).
- The remaining hazard is an element pointer (`&xs[i]`) taken before the
  list grows. That's the same rule as `std::vector`, and it's left to the
  debug tooling, or to a list with its own region.
- Views are deferred. If they're wanted later, they'll be a separate type
  (`View{T}` or `Slice{T}`). Whether that needs syntax, and how views
  interact with arrays, lists and strings, is open. Views are safe for
  storage that can't move (fixed and bounded arrays, and strings if they're
  immutable once built), which is a likely place to start.

**Decided under option B (2026-11-12): strings.**

- `str` is immutable UTF-8 bytes, `{ptr, len}`, with the length in bytes.
  The bytes are in static data, the source buffer or an arena, and never
  change or move. So a `str` is always safe to copy, and slicing a string
  can give a view, because a view of immutable bytes can't be told apart
  from a copy. Literals without escapes stay zero-copy slices of the source
  (D13).
- A string builder (`StrBuilder`, or `[]u8`) is the growable form, and
  finishing it gives a `str` without copying, because its bytes are already
  in the arena.
- For the programmer, the whole model is that strings are immutable values,
  and making a new one allocates in the current arena. A function that builds
  a string and returns it needs nothing special, because a callee allocates
  into its caller's arena. Returning a string built inside the function's own
  `with Arena()` is caught by the usual lifetime rule.
- These are optimizations that can't be observed:
  - Interpolation evaluates its embedded expressions left to right first,
    then formats all the pieces straight onto the top of the current arena,
    so the result grows in place and is never copied. User types take part
    through a formatting method that appends to a builder (something like
    `on T def __format__(self, *StrBuilder b)`) rather than returning a
    `str`, so nested formatting also appends in place.
  - `print` of an interpolated string writes the pieces directly and builds
    no string.
  - `s = s + x` extends `s` in place when its bytes are still at the top of
    the arena. Existing strings keep their own length and can't see the
    extra bytes. This is the same top-of-arena check as list growth, and it
    makes the usual string-building loop amortized linear.

**Decided under option B (2026-11-12): values and references.**

| kind | semantics | representation |
|---|---|---|
| numbers, `bool`, structs, `[N]T` | values, copied on assignment | inline |
| `str` | a value (immutable, so sharing is invisible) | `{ptr, len}` |
| `[]T`, `{K}V` | references, like Python's list and dict | `*List{T}`, `*Dict{K, V}` |
| `*T` | an explicit pointer | a pointer |

- `s = Stuff(...)` is a value in the frame, or inline in whatever contains
  it, like C's `Stuff s = {...}`. This follows from compound literals (D7,
  D9) and from luv having `*T` and `&`.
- `List{T}` is an ordinary value struct in the prelude (`{data, len, cap}`),
  written the way a user would write `MyList{T}`. The reference behavior
  comes from the synonym, so `*MyList{int}` behaves exactly like `[]int`,
  and a plain `List{int}` value behaves exactly like a plain `MyList{int}`.
  Copying a plain `List{int}` value aliases its storage, as Go slices do,
  but only for someone who chose the value form.
- `.` auto-dereferences (D22), so `xs.append(4)` works on a `[]int`, and
  methods are defined `on List{T}`.
- A single object is allocated with `&Stuff(.a=1)`: the value goes into the
  current arena, and the result is a pointer to it. A list literal
  `[1, 2, 3]` is the same thing for `&List{int}(...)`, with its elements in
  the current arena, so the literal isn't special. The parser accepts `&` of
  a struct literal (`address_of_struct_literal.luv`) but not of other
  values such as `&5` or `&f()`. `new(...)` is the alternative if
  `&Stuff(...)` turns out to be too subtle.
- Open: zero values in general, and in particular what `[]int xs` with no
  initializer holds. Allocating an empty list is cheap (a bump) and matches
  Python, which has no null lists. Treating null as the same as an empty
  list would avoid that, but a method can't replace a null `self` with a new
  list, so `xs.append()` on a null `xs` would still need the declaration to
  allocate. Making it work would mean a method on `*[]int` that can replace
  its `self`, which D22's rule against methods on pointers rules out, and
  that's probably not worth reopening. It probably belongs with a general
  decision about zero values.

**To decide during codegen:** whether calls whose result isn't kept run in a
per-statement scratch arena (statement temporaries, from option A). Keep it
if the compiler can elide temporaries cheaply.

---

## 3. Grammar sketch (with the recommendations applied)

FIRST sets are shown where they decide anything. `TYPESTART` = `{TYPENAME, TYPEOF,
builtin-type keywords, '*', '[', '{'}`.

```
module        -> { import } { toplevel_stat } EOS
import        -> IMPORT STRING [AS NAME] NEWLINE

toplevel_stat -> funcdef
               | ON receiver ( DEF funcsig suite | FOREIGN funcsig NEWLINE )   # D22
               | STRUCT TYPENAME [ type_params ] ':' INDENT field { field } DEDENT
               | FOREIGN funcsig NEWLINE
               | simple_stat end                 # global decls / `X = const`

funcdef       -> DEF funcsig ':' suite
funcsig       -> [ type ] NAME '(' [ param { ',' param } ] ')'
                 # [type] is decided by TYPESTART vs NAME: LL(1) via D4
param         -> type NAME | NAME                # bare NAME: `self` only
field         -> type NAME [ '=' expr ] end

suite         -> INDENT { stat } DEDENT
               | simple_stat end                 # D3a one-liner

stat          -> IF expr ':' suite { ELIF expr ':' suite } [ ELSE ':' suite ]
               | WHILE expr ':' suite
               | FOR NAME [ ',' NAME ] IN expr ':' suite
               | WITH expr [ AS NAME ] ':' suite
               | funcdef
               | simple_stat end

simple_stat   -> RETURN [ expr ]                 # expr absent iff t in {NEWLINE,DEDENT,EOS}
               | BREAK | CONTINUE | PASS
               | PRINT expr | CHECK expr
               | GLOBAL NAME { ',' NAME }
               | decl_or_expr_stat

decl_or_expr_stat
               -> TYPESTART-led:  type NAME [ '=' expr ]           # D5
                | NAME/TYPENAME-led: primary_chain
                     ( NAME [ '=' expr ]        # chain was VTYPE → decl
                     | '=' expr | AUGOP expr    # chain was lvalue
                     | <nothing> )              # chain was a call

end           -> NEWLINE | <peek DEDENT> | <peek EOS>

type          -> '*' type
               | '[' [ expr ] ']' type          # []T list, [N]T array
               | '{' type '}' type              # {K}V dict
               | BUILTIN_TYPE
               | TYPENAME [ type_args ] | NAME '.' TYPENAME [ type_args ]
               | TYPEOF '(' expr ')'
type_args     -> '{' type { ',' type } '}'       # D21
type_params   -> '{' TYPEPARAM { ',' TYPEPARAM } '}'

expr          -> subexpr(-1)                    # Lua 4 precedence climbing, D8
primary       -> NUMBER | string | TRUE | FALSE | NULL
               | NAME | TYPENAME                # TYPENAME '(' → compound literal
               | '(' expr ')'
               | '[' list_or_compr ']'          # D10; list form allows trailing ',' (D19a)
               | '{' [ expr ':' expr { ',' expr ':' expr } [','] ] '}'   # trailing ',' ok (D19a)
               | (SIZEOF|ALIGNOF) '(' type_or_expr ')'   # D7 first-token rule
               | OFFSETOF '(' type ',' NAME ')'
               | CAST type '(' expr ')'           # D7
string        -> STRING
               | STRING_BEGIN expr { STRING_MID expr } STRING_END   # D13 \( )
postfix       -> { '.' NAME | '.' TYPENAME | '[' slice_or_index ']' | '(' args ')'
                 | type_args }                  # only after a type (D21)
args          -> [ arg { ',' arg } ]
arg           -> '.' NAME '=' expr | expr       # designator, D9
```

---

## 4. Lookahead audit

Every place in the language that looks like it needs more than one token,
and how this design resolves it:

| Construct | Naive k | Resolution | Final k |
|---|---|---|---|
| `Stuff s` vs. `Stuff(1)` | 2 | TYPENAME lexing + left-factor (D4, D5) | 1 |
| `mod.Type v` vs. `mod.func()` | 3 | postfix chain → `expdesc.k == VTYPE` (D5) | 1 |
| `[10]int a` vs. `[1,2]` | 2+ | `[` at statement start is always a type, in an expression always a literal (D5) | 1 |
| `*int p` vs. `*p = 1` | 2+ | no prefix deref (D5) | 1 |
| `def int f(` vs. `def f(` | 2 | NAME is never a type (D4) | 1 |
| `foreign [T] f(` | 2 | same | 1 |
| `int a` vs. `self` param | 1 | — | 1 |
| `f(.a=1, 2)` named arg | 1 | leading `.` can't start an expression (D9) | 1 |
| `a not in b` | 2 | `not` in binop position ⇒ `not in` (D8) | 1 |
| `a[i:j]` vs. `a[i]` | 1 | — | 1 |
| `if ... elif/else` | 1 | after DEDENT (D3) | 1 |
| `return` vs. `return x` | 1 | follow set `{NEWLINE, DEDENT, EOS}` | 1 |
| `x = 1` declares vs. assigns | — | semantic, at first assignment (D6) | 1 |
| `List{int}`, `max{int}(x)` vs. dict literal | 1 | `{` can't otherwise follow a name or type (D21) | 1 |
| `sizeof(T)` vs. `sizeof(e)` | 2+ | first-token rule, D5 left-factor for names, `[`/`{` = type (D7) | 1 |
| `[e for x in xs]` vs. `[e, f]` | unbounded | **D10**: reorder (k=1) or bracket-bounded skim + rewind | 1 or bounded rescan |
| use before definition | whole file | **D11** lazy top-level skim on first miss (free for in-order files) | 1 per pass |

If D10 picks (a), the whole language is LL(1) with *zero* uses of the
lookahead slot, and `next()` loses its branch.

---

## 5. Lexer spec sketch (`llex.c` analogue)

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

Token size: only one or two tokens are ever alive (D1 comparison), so size
doesn't matter as storage, only as the cost of handing a token to the
parser. ABIs: SysV x86-64 and AAPCS64 return structs up to 16 bytes in two
registers. **Windows x64 returns only 1/2/4/8-byte structs in `RAX`**, and
16 bytes go through a hidden pointer. If the lexer writes in place into
`ls->t` (Lua's way), every ABI does the same thing, one L1 store, and size
barely matters. **Plan:** start with (A) in place (no side tables, no 2²⁴
limits). The step 1 bench also tries (B) returned by value
(`Token lex(LexState*)`) on macOS and Windows. Switch if it wins, or if a
token buffer ever appears (D10 replay, a future AST). Keep the token behind
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
16-char name every 8 lines. That's the number to attack in the D15
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
- `[0-9]`: number (D13).
- `"`: string (D13). On `\(`, return `STRING_BEGIN` and push
  `bracket_depth`. On a `)` that brings `bracket_depth` back to
  `interp_stack[interp_sp-1]`, pop it and resume the string reader, returning
  `STRING_MID`/`STRING_END` instead of `)`.
- Identifier start (class table): scan while `class[*p] & IDENT`, folding the
  hash and the `has_upper`/`has_lower` bits as it goes. Intern. If the interned
  `Name` is reserved, return its keyword kind. Otherwise return `TK_TYPENAME`
  or `TK_NAME` from the case bits (D4 rule: `_*[A-Z]` or `_*[A-Z]…[a-z]…` →
  TYPENAME; `ALL_CAPS` and lowercase → NAME).
- `'\0'` at `p < end`: error (embedded NUL). At end: dedents, then `TK_EOS`.

---

## 6. Implementation steps

Each step is measurable on its own.

1. **Lexer + token dump.** `llex.c/h` per section 5, and a driver that lexes a
   file and prints tokens or just counts them.
   - Test: every `~/luv60/test/*.luv` lexes cleanly. Golden token dumps for
     the layout edge cases (`comment_indent`, `lex_indented_*`,
     `blank_line_before_indent`).
   - Bench: lex-only time on `dumbbench.luv`. This is the floor for
     everything else. No SIMD. Speed comes from the char-class table,
     padding (no bounds checks), zero-copy names/strings, and `memchr`
     (libc's, which is vectorized for us).
2. **Parser skeleton + null code generator.** `lparser.c` with `stat`,
   `toplevel_stat`, `block`, `subexpr`, `primary`, `type`, against a
   `lcode.h`-style interface whose implementation does nothing.
   - Test: all of `test/*.luv` parse. Syntax-error cases in `test/errors/`
     fail, with expected messages where applicable.
   - Bench: parse-only vs. `luac -p` (1.59 s) and luv60 `--syntax-only`.
   - **Status 2026-11-07: written** as `src/parser.c/h` (acceptance only,
     no code generator stub) plus the `src/parsecheck.c` driver. All 164
     luv60 `test/*.luv` files parse once converted to the decided syntax
     (`cast T(x)`, `Stuff(.a=1)`). Unconverted, the 14 failures are exactly
     those. In `test/errors/`, the real syntax errors are reported at the
     same line:col as luv60. Parse time on `dumbbench.luv`: **0.45 s** vs.
     lexing alone 0.43 s and Lua 4 lex+parse with codegen stubbed 1.20 s
     (hyperfine). The parser itself adds about 1 ns per token, but it does
     no name binding yet, while Lua's stubbed build still registers locals
     and builds constant tables.
   - **Status 2026-11-08:** D21 `{}` generics added to the parser
     (type arguments, `struct`/`def` type parameters, explicit call-site
     arguments). The luv60 results are unchanged (150/164, the same 14
     unconverted files).
3. **`expdesc` + name binding (D5, D6, D9, D15).** Real symbol resolution
   through `Name.binding`, declaration vs. expression dispatch, keyword args.
   Re-bench to see the cost of resolution, and compare against a Lua-style
   fixed locals table with a linear scan (see D15).
4. **Forward references (D11, lazy skim).** Add tests that call functions
   defined later, use mutual recursion, and have a malformed later header
   referenced earlier (must report the header's error, not "undefined").
   Coverage of the skim path:
   - a debug **force-skim** flag runs the skim up front on every file. The
     whole suite must give identical results in both modes.
   - a script that **reverses top-level definition order** (`main` first)
     in each test and expects the same output. Files with untyped
     globals/constants are skipped or expected to fail.
   Confirm dumbbench time is unchanged (it never misses), and time the
   forced-skim path separately.
5. **Comprehensions (D10)**, using whichever option is picked.
6. **Profile and tune:** `perf`/Instruments on the step 2–4 drivers. Try the
   `newline_before` flag variant (D3), hash choice (D15), and removing the
   lookahead slot.
   - **Measured 2026-11-13** (desktop, `parsecheck`): where the hot lexer
     state lands in a cache line changes the whole parse by 5–8%. `LexPos`
     as `p, t, u32 ×3` was 9% faster on dumbbench than `p, u16 ×3, t`, but
     because of where it put `pos`, not because of the field widths. u16,
     u32 and u64 made no difference by themselves. `Parser` comes from
     `malloc` (32 mod 64 with glibc), and `pos` at 8 mod 64 was fast, while
     at 32 or 40 it was slow, so it seems to matter whether all 40 bytes of
     `LexPos` are in one line. Forcing `Parser` to be 64-byte aligned gave
     mixed results. This depends on allocations made before `parse_file()`,
     so it's fragile, and not worth tuning until there's more code.
   - Inlining into the recursive `primary()`/`subexpr()` shows up too.
     Adding the D10 skim made dumbbench, which has no list literals, 1–2%
     slower, and making `list_literal()` noinline undid that.
   - The D10 skim and rewind cost about 7% on a generated file that's
     nothing but small list literals, compared to before they existed.
   - To check later: `Parser.locals` is a large array (`MAX_LOCALS`
     entries) between `ls` and the rest of the parser's fields, which may
     put warm data in more lines than it needs. Try moving it out of line.
     Get hardware counters (L1 misses, split loads) on the desktop, which
     needs `perf_event_paranoid` lowered. Try LTO and PGO, and noinline on
     cold parser paths.
7. **Real code generator** behind the interface: luv60's IR, sqbe, or a
   bytecode interpreter in the spirit of `rewrite.txt`'s reference
   implementation.

Proposed layout in `~/luvr`:

```
plan.md
src/lex.c src/lex.h        # lexer, Name interning (written)
src/lparser.c src/lparser.h # grammar, FuncState, expdesc
src/lcode.h  src/lcode_null.c
src/lexdump.c              # lexer driver: -d dump, else count+time (written)
src/main.c                 # --syntax-only driver
bench/                      # scripts comparing against luac -p and luv60
```

---

## 7. Decisions that are the user's call

Tracked in the **Decision log** at the top of section 2. Open: D20 (general `for`). Provisional: D21 (generics syntax). Open: D22a (methods on builtin types and the prelude), D23 (arrays, lists, slices and their memory). Deferred: D15 (to measurement), D5b (until
real code needs it).

---

## 8. Check against the 10 LL(1) rules from the earlier chat

| # | Rule | This plan | Notes |
|---|---|---|---|
| 1 | Every construct starts with a distinguishing token | **Mostly follows** | Every statement except declaration/expression statements starts with a keyword (D5 table). Declaration/expression statements start with `TYPESTART` or `NAME`, and D5 resolves those. |
| 2 | Avoid C-style declarations | **Deliberately breaks** | Requirement. Restored to LL(1) by the case-based TYPENAME token (D4) and left-factoring (D5). What it costs: the naming convention becomes a hard language rule, and prefix `*` deref is unavailable. |
| 3 | No `<>` generics; type vs. expression decided syntactically | **Follows** | No `<>`. Types are `*T`, `[N]T`, `[]T`, `{K}V`, TYPENAME, `Name{T, ...}`. Generics use `{}` (D21), unambiguous only because of D4. `<>` would mostly work too because of D4, except explicit call-site type arguments. |
| 4 | Required block delimiters, no dangling else | **Follows, via layout** | INDENT/DEDENT *are* the delimiters (D2, D3). Dangling else can't happen because `else` must line up with its `if`. A D3a one-line suite allows simple statements only, so no nested `if` there. |
| 5 | Lambdas distinguished up front | **N/A for now** | No lambdas. If added, start them with a keyword (`def(int x) int: ...` or similar) so they're never `(a, b) => ...`. |
| 6 | Precedence climbing / Pratt | **Follows** | Lua 4 `subexpr` precedence climbing (D8), replacing luv60's Pratt table. |
| 7 | Self-terminating or explicitly terminated statements | **Follows, via layout** | NEWLINE/DEDENT/EOS terminate simple statements (D3). Optional-semicolon problems go away because newlines inside brackets are whitespace and nowhere else. |
| 8 | Context-free lexer, no context-dependent keywords | **Follows (better than luv60)** | Drops luv60's import-dict lookup in the lexer (D4). TYPENAME vs. NAME depends only on the identifier's characters. One caveat: the lexer keeps state for layout (indent level, bracket depth), but that depends only on characters, never on parser state. |
| 9 | Assignment: keyword, or parse an expression then check for `=` | **Follows the Lua approach** | D5: parse the postfix chain, then branch on `expdesc.k` and the next token (`=`, augmented op, NAME for declarations). Named arguments need no trick: `.name=` starts with a token no expression can (D9). |
| 10 | No left recursion | **Follows** | Postfix chains and binary operators are loops (`var_or_func_tail`, `subexpr`). |

Two items go beyond those rules and aren't covered by them: comprehensions
(D10) and forward references (D11). The rules assume an expression's names
can always be resolved left to right in one pass. Python-order comprehensions
and use-before-definition break that assumption whatever the grammar is.

---

## 9. Keep the grammar formatter-friendly (constraints for a later formatter)

The formatter is a separate, much later project: aggressive, re-wrapping,
column-limited, opinionated, in the spirit of clang-format, ruff and yapf.
Nothing here builds it. These are rules that keep decisions in this plan
from making it harder:

1. **Parsing never depends on name resolution.** The formatter has to parse
   one file alone, without imports or types. D4's lexical TYPENAME rule
   makes that true: `Stuff s` vs. `Stuff(1)` and `mod.Type x` vs.
   `mod.f()` are decided from tokens alone, unlike C's lexer hack. Rule:
   *no future parse decision may consult the symbol table*. (D11's
   skim-on-miss is resolution, not parsing, so it's fine.)
2. **Grouping parens are transparent wherever an expression appears.** A
   formatter wraps an over-long unbracketed expression by adding parens:
   `x = (a +\n b)`, `return (...)`, `print (...)`, `if (...):`,
   `for x in (...):`, `with (...):`. That only works if
   `(e)` always means `e`. Hazards:
   - **Tuples.** If `(a, b)` ever becomes a tuple, `print (a, b)` changes
     meaning. This is black's biggest wart. If tuples arrive, make them
     syntactically distinct (D18 already says no parenthesized tuples).
   - **D7's `sizeof(([1,2]))`** uses parens to force an expression. That's
     a deliberate exception inside a keyword's own parens. The formatter
     must never add or remove parens there. Keep it the only such case.
3. **Only brackets continue lines** (D3b: no `\`). One way to continue a
   line means one thing for the formatter to emit and nothing to normalize.
   Inside brackets, newlines are whitespace (D3), so continuation indent and
   alignment are entirely the formatter's choice. The block indent is fixed
   at 4 (D2).
4. **Comments are `#` to end of line only, never inside a line** (D14: no
   block comments). Every comment is either on its own line or at the end of
   one, which is the easy case for attaching comments. Comment-only lines
   don't affect indentation (D3), so the formatter can re-indent them to the
   following statement without changing meaning (as
   `test/comment_indent.luv` would get normalized).
5. **One statement per line, no `;`.** Keep it that way.
6. **Trailing commas: list and dict literals only** (D19a, decided).
   A trailing comma is allowed in `[...]` and `{...}` data literals, where it
   lets the formatter keep one element per line ("magic trailing comma").
   It is **not** allowed in call arguments, parameter lists, or compound
   literals (`Stuff(...)` is call syntax). LL(1): after `,`, `]`/`}` ends the
   list (Lua 4's `listfields`/`recfields`).
7. **No adjacent string literal concatenation** (D19b, decided) because of
   the accidental `["a" "b"]` missing-comma bug. Long strings stay unsplit
   by the formatter. Interpolation (D13) reduces the need. Something
   explicit can be added later if necessary.
8. **Lossless lexing is the formatter's job, but share the lexer.** The
   compiler lexer drops comments, blank lines and literal spellings. The
   formatter needs them (`0x1F`, `1_000`, raw string text, number of blank
   lines). Write `llex.c` so a compile-time variant (`#ifdef LEX_TRIVIA`)
   emits `COMMENT`/`BLANKLINES` tokens and token end offsets, so the
   compiler build pays nothing. Token kinds and layout rules then live in
   one place.
9. **A shared spec and corpus instead of a shared parser.** The compiler
   parser emits code as it goes and builds no tree, but the formatter needs
   a CST, so it will have its own parser. To keep the two from drifting:
   section 3's grammar becomes the normative spec, and the formatter's tests
   include `format(f)` re-lexing to the same token stream as `f` (ignoring
   layout, trivia and added grouping parens) for every file in the test
   corpus.
