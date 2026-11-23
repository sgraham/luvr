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
| D15 | **decided** 2026-11-17 | One table for keywords and identifiers (Lua `marked`-style), and a `binding` pointer on each `Name` for one-load scope lookup (chosen over a fixed per-function locals table with a linear scan, 2026-11-08). The intern table is one shared open-addressing table of `{tag, val}` slots, positioned by the tag, which is also used for `NameMap` and IL constants. Benchmarked layouts, load factors and Verstable all came out the same or slower. |
| D16 | **decided** 2026-11-07 | Line counter **in the lexer only** (free, since layout already visits every `\n`). Used for errors and debug line info. Tokens carry only a `u32` offset, and columns are computed lazily on error. Errors: first error → `longjmp` to the handler. **Library-safe**: all state lives in a context struct (no mutable globals, unlike luv60's `glob`/`tu`), all allocation comes from context-owned arenas reset by the handler, no `exit`/`abort`/stdio in the library (message + offset stored in the context, internal asserts take the same path), keywords interned per context (Lua 4's `luaX_init` per `lua_State`). |
| D17 | **decided** 2026-11-07 | Separate `toplevel_stat()` and `stat()` switches instead of luv60's repeated `if (toplevel) error(...)`. `module -> { import } { toplevel_stat } EOS`. Nested `def` allowed; `struct`/`on`/`foreign`/`import` top-level only. |
| D18 | n/a | Guidance for future syntax, nothing to decide now. |
| D19 | **decided** 2026-11-07 | D19a: trailing commas **only in list/dict literals** (not calls, params, or compound literals). D19b: **no** adjacent string concatenation. Section 9 rules 1–5 remain constraints on everything else. |
| D20 | open | **Looping constructs.** No `while` keyword (removed from the lexer). Investigate a general `for` that covers `while`-style loops (e.g. `for cond:` vs. `for x in xs:`). Interesting for LL(1): after `for`, telling `NAME [, NAME] in` from a condition expression needs care, since a condition can also start with `NAME`. Options include parse-then-decide on `in`, as with D5's expdesc dispatch. |
| D21 | **decided** 2026-11-08 (provisional), **revised** 2026-11-12 | Generics use `{}`: `List{int}`, `struct Pair{A, B}`, `def T max{T}(...)`, explicit `max{u64}(...)`, inferred by default. Monomorphized by re-parsing the body per instantiation. Revised: type parameters are exactly one capital letter (`[A-Z]`), and a single capital letter is always a type parameter, never a struct name. `{...}` lists are still required and give the order of explicit type arguments. A return type that uses parameters before `{T}` is checked afterwards with a 26-bit mask, so binding stays single-pass. Options explored (`<>`, `[]`, `'`, `!`, parameter placement, `$T`, alphabetical order) are recorded in D21, since the user may revisit this after writing generic code. **Revised 2026-11-13:** no free generic functions. Type parameters only come from a struct's list or a method's receiver, so type arguments are never inferred from function arguments. Free generics can be added later without breaking anything. |
| D22 | **decided** 2026-11-08 | Methods (`on`): keyed on (generic type, name), no specialization, no methods on pointers, methods only in the receiver type's own module (builtin types: the prelude, D22a open), no free-function overloading. Static methods (`List{int}.new()`) and dunders as operator spellings. A method is a free function with nicer syntax and a namespace: `xyz.f(5)` is `Xyz.f(&xyz, 5)`. |
| D23 | **open** 2026-11-12, option B preferred | Arrays, lists and slices, and how their memory is managed. What `a = [1, 2, 3]` is, what `a[1:]` is, what a comprehension produces, and what the user has to do to free each of them. This affects literal typing, comprehensions (D10), `[]T`/`{K}V` (D21, D22a) and likely more syntax, so it's settled before further type work. Option A: lists in locals own a region, a current pool, copying results out of scratch blocks. **Option B (preferred):** one dynamically scoped current arena that every allocation goes into, `with Arena():` and `into a:` blocks, and opt-in storage annotations. Under B: literals are lists, slicing copies, comprehensions produce lists, views are deferred to a later `View{T}`/`Slice{T}` type. Strings are immutable `{ptr, len}` with a builder, structs are values, `[]T` is `*List{T}`, and `&Stuff(...)` allocates in the current arena. |
| D24 | **decided** 2026-11-13, **implemented** 2026-11-14 | Modules and imports. A module's canonical name is its path relative to the root, normalized lexically, and keys a dict of `Module`s on Ctx, so each file is loaded and parsed once however it's imported. Files are loaded through a callback on Ctx. A `Module` (name, buffer, table of globals) lasts as long as the Ctx, and a `Parser` is per-parse state that points at one. Globals move from `Name.global` to the module's table, with a `Name.binding` cache only if the table probe costs too much. Resolves D21's cross-module instantiation question: the one active Parser is re-pointed at the defining module. |
| D25 | **open** 2026-11-14, direction agreed | Backend and execution model. Survey of existing small backends and execution models. Direction: run in process first, on x64 Windows, x64 Linux and aarch64 macOS. One backend of our own: a linear SSA IL per function built in the single pass, ABI lowering with structs by value as QBE does it, and a backward pass that allocates registers, selects and encodes, into sections with relocations. A JIT is the first consumer, and C objects are linked into a host program by the user. The optimized tier is more passes on the same IL. No interpreter, no `.s`, no third-party backend. sqbe is the fallback and the comparison. Debug info, executable writers and a C emitter are deferred. |
| D26 | **provisional** 2026-11-14 | The IL. Per-function flat array of 8-byte instructions (opcode, type, two 24-bit references, constants below a bias), value types `i32`/`i64`/`f32`/`f64`, aggregates as memory. Two stages: the parser appends raw instructions with every local as a frame slot, then one forward pass at the end of the function promotes slots whose address wasn't taken to SSA and does folding and CSE. Blocks in source order with structured control flow. C calling convention for luv functions, QBE-style aggregate arguments, and the luv context pointer in a pinned callee-saved register. |
| D27 | **decided** 2026-11-15 | Numeric types and conversions, as in Go. `int` is an alias of `i64`, and error messages say `int`. `len` and indexes are `int`, and `sizeof` is an untyped constant (D28). No other aliases: no `uint`, `byte`, `float` or `double`, and no `f16` for now. `int` is an error in `foreign` signatures, since C's `int` is 32 bits. There are no foreign-only types, and `foreign` signatures match C by ABI, not by C type. No `void` in luv code. `codept` is left for D23. Untyped constants adapt to context, defaulting to `int` and `f64`, with range checks. No implicit conversions between numeric types (widths, signedness, `f32`/`f64`, int/float). Any integer type is accepted as an index or shift count. Lossless widening was rejected because it hides where arithmetic wraps, and is revisited only if the casts prove too painful. `cast` converts between any numeric types, between pointers, and between pointers and `u64` (revised 2026-11-21: not `i64`, as Go only converts pointers to `uintptr`; a pointer difference is a `u64` subtraction, cast to `i64` if needed). Float to integer out of range is unspecified, and constants must be in range for the target. |
| D28 | **decided** 2026-11-15 | Constants and folding, as in Go. Constant expressions are folded in `ExpDesc` as they're parsed. Untyped integer constants are `__int128` (a two-`u64` implementation later for MSVC), untyped floats are `double`, and they're range checked only when converted to a type. Typed constants fold at their width, and overflow is an error. A constant zero divisor is an error. ALL_CAPS constants keep their initializer's untypedness, and a type or suffix makes them typed. `~` on an untyped constant is `-x - 1`. |
| D29 | **deferred** 2026-11-16 | Tests as ninja targets. `m` would pass all of its arguments to ninja, with `test` and each test's name as phony targets. Waits until there's one compiler binary to run on every input, so that one generic rule covers every test. `m` reruns the generator each time to pick up new tests, and test edges have no stamp files, so they always run. |
| D30 | **proposal** 2026-11-16 | Emission for the vertical slice. `ExpDesc` gets a kind (constant, local slot, ref, string literal, void) replacing `is_const`, `exp_*` helpers in the parser and `il_*` buffer functions in `il.c`. `parser.c` is built twice (`EMIT` 0 or 1) behind one `luv` driver (`luv parse`, `luv il`), so checking costs nothing extra. Unsupported constructs are errors in the emitting build. The slice runs `luv il` output in a throwaway Python interpreter. SSA concerns are ignored until the SSA pass. |
| D31 | **proposal** 2026-11-20, partly decided | Aggregates. C layout for every struct (no `foreign struct` mark), natural alignment, nothing above 8, empty structs are errors, `str` a builtin with a fixed layout. A per-struct summary (size, alignment, two SysV SSE bits, an HFA kind) is all the Win64, SysV and Apple arm64 classifiers need, with the per-ABI rules checked against clang and the ABI documents. Aggregates are addresses in the IL (`COPY`, `ZERO`, `ARGAGG`, `PARAMAGG`, aggregate results). The ABI step is never in the parser. Aggregates by value in `foreign` signatures are errors for now. Narrow integers are extended on both sides. Leaning: the ABI step before promotion, and splitting unescaped aggregate slots into scalars. |

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
- **Result (2026-11-17): the table's layout doesn't matter, so it's shared.**
  The IL needs to intern constants, and `NameMap` was already a second
  open-addressing table, so the intern table was measured to choose one
  layout for all three. Desktop, `lexdump` (the lexer alone, which spends
  about 35% of its time in `intern` on dumbbench), `-falign-functions=64`,
  user cycles averaged over interleaved `perf stat` runs:

  | | dumbbench | betterbench |
  |---|---|---|
  | A: HEAD. `u64` slots of hash tag and index, positioned by the hash's low bits, rehashed from `Name.hash` | 1720M | 2824M |
  | B: as A, but positioned by the tag, rehashed from the slots alone, no `Name.hash` | 1724–1756M | 2782–2841M |
  | D: `u32` slots of only the index, compared through `Name.hash` | 1694–1765M | 2810–2824M |
  | E: B with a ¾ load factor instead of ½ | 1619M | 2863–2903M |
  | F: Verstable, a set of name indices, at ½, ¾ and ⅞ load | 2229–2343M | 3073–3127M |

  - A, B and D are within noise of each other. The spread on dumbbench
    follows code layout: with `-falign-loops=64` too, B is within 0.3% of
    A. All three do the same 19.7M probes on dumbbench. The cost of
    interning is the cache miss on each new name, which none of these
    layouts avoids.
  - The tag in the slot isn't measurable. At half load, there are too few
    collisions on the probe path for skipping the `Name` load to matter.
  - B has 62% fewer dTLB misses than A on dumbbench, from the smaller
    `Name` (24 bytes without `hash`). It also has 1.8M more branch misses,
    from the rehash's test for empty slots, which is a coin flip at half
    load. They roughly cancel.
  - E is faster or slower depending on where a file's name count falls
    relative to a power of 2. dumbbench's 1.25M names fit in 2M slots
    instead of 4M, while betterbench's 206k end at the same size as B and
    probe for longer. Not a general win.
  - Verstable runs 15–23% more instructions. Part of that is its rehash
    calling the hash function, which reads a random `Name`, for every key.
    It also has deletion, high load factors and short chains, which an
    insert-only table at half load doesn't need.
  - Not tried: prefetching the slot when the hash is known (there's
    little lexer work between hashing and probing to overlap it with) and
    Swiss-style control bytes (probes are already about 1.05 slots).

  Decided: B's layout, as one table used for interning, `NameMap` and IL
  constants. A slot is `{u32 tag, u32 val}`, positioned by `tag & mask`, and
  tag 0 is empty. For `NameMap` the tag is the `NameId` + 1, so a matching
  tag is a matching key, and consecutive ids are still in consecutive slots.
  For interning, the tag is 32 bits of the hash and a match is confirmed
  against the key. Growing only needs the slots. Scratch builds are in
  `~/luvr/scratch/d15` on the desktop.

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

Resolved by D24 (2026-11-13): an instantiation is parsed by the same Parser,
re-pointed at the defining module, so it sees that module's globals.

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
- At run time `str` is an ordinary struct, as if it were declared
  `struct RuntimeStr: *u8 data; int len`, and the IL has nothing specific
  to strings. A literal's bytes go in the output's read-only data, copied
  from the source slice or from its decoded copy in `Ctx.strs`. The data
  isn't guaranteed to be NUL terminated.
- Eventually, the runtime's `print` will only print a `str`, and other
  values are converted by luv code, such as `on int def str __str__(self)`,
  which interpolation uses too.

**For the vertical slice (2026-11-16):**

- There are no struct values in the slice, so a `str` is emitted as its two
  words (data and len). This goes away when structs are implemented.
- `str + str` and interpolation outside `print` need an arena. They're
  typed by the front end but are rejected by emission as not supported yet.
- `print` remains a single expression and adds a newline. Multiple values
  are printed using interpolation. The runtime has one printer per kind,
  none of which write the newline: `luv_print_i64` (narrower signed ints are
  sign extended), `luv_print_u64` (zero extended), `luv_print_f64` (`f32`
  is widened, printed with `%f`), `luv_print_bool` (`true`/`false`),
  `luv_print_str(data, len)` and `luv_print_end` for the newline. An
  interpolated string in `print` emits one call per piece then
  `luv_print_end`, so no string is built.
- Output is buffered by the runtime and flushed at exit. The interpreter
  calls the same C functions that native code links against.

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


### D24 Modules and imports

Decided 2026-11-13, implemented 2026-11-14 (steps 1–5 below).

**Canonical names.** Each source file is a module. Its canonical name is
its path relative to the root (the main file's directory), normalized
lexically, without `.luv`. An import path is relative to the importing
file's directory, so joining the two and collapsing `.` and `..` gives the
canonical name. `import "x"` in a root file and `import "../../x"` in
`sub/dir/y.luv` are both the module `x`. A path that goes above the root
normalizes to `../x`, which is still a consistent key. Normalization doesn't
touch the filesystem, so a file reached through two symlinked paths is two
modules.

**Loading.** Ctx has a dict from canonical name to `Module`. Importing a
module that's already in the dict only binds the name (or `as` alias) to it,
and nothing is parsed again. Otherwise the module is loaded and parsed
completely before the importer continues past its import list. Imports come
first in a file (D17), so at that point the importer has no locals, and no
globals other than the module names it has already imported. Importing a
module that's still being loaded is an import cycle, and an error.

Files are read through a callback on Ctx that returns a buffer followed by
`LEX_PADDING` zero bytes, or fails ("Couldn't open import, looking for
'test/errors/nopes.luv'."). The library itself has no stdio (D16).
`parsecheck` supplies a callback built from its `load_file()`.

**Module and Parser.** A `Module` lasts as long as the Ctx. It has the
canonical name, the source, a loading/done state, and the table of
globals. The source is kept so that generic bodies can be re-lexed. A
`Parser` is only the state of parsing, and points at the Module it's
currently in: the lexer, locals, `func_depth` and `type_params`.

The source is a `FileBuf`, which Module owns. `LexState` has a copy of the
Module's `FileBuf`, which is only changed when the Parser switches modules.
The lexer reads `buf` and `end` on every token, so it uses its copy rather
than going through a Module pointer, and `lex.c` doesn't need to know about
Modules.

```c
struct FileBuf {
  const char* filename;  // for errors
  const u8* buf;         // followed by LEX_PADDING zero bytes
  const u8* end;         // buf + len
};

struct Module {
  const char* path;  // canonical name, also the key in Ctx's dict
  FileBuf src;       // kept so generic bodies can be re-lexed
  u8 state;          // loading or done, to detect import cycles
  // table: NameId -> Global
};
```

**One active Parser.** `parse_file()` allocates a single Parser, and it's
used for the main file, every imported module, and every instantiation.
Each of those starts at a point where the Parser's state is empty apart
from the lexer and its Module: imports come first in a file (D17), and
instantiations are queued to the end of a top-level item (D21). So there's
no locals, `func_depth` and `type_params` are 0, and the lexer isn't inside
an interpolation. Switching to another module (or another place in the same
one) saves the Module pointer, the `FileBuf` and `LexPos`, points them at
the new source and offset, and restores them afterwards. That's less than
64 bytes, so an import nests by saving it on the C stack. Instantiations
found during an instantiation are queued rather than nested.

The switch asserts that the state really is empty (`num_locals`,
`func_depth`, `type_params` and `interp_sp` are 0). A new Parser field that
isn't empty at a top-level boundary fails that assert at the switch,
rather than needing a full snapshot of the Parser.

**Globals.** The module's table, keyed by NameId, is where globals live. It's
needed for `mod.name` anyway, which looks a name up in another module's
globals. An entry is a Global record. For now it's only a `GlobalKind`, and
it's where the type, the declaration offset and the codegen symbol will go.
`Name.global`, `Parser.globals` (the per-file list of NameIds) and
`pop_globals()` are removed.

Unqualified names are looked up in the table too. That's a hash probe for
every non-local name, in a table that can be large (dumbbench has 1.25M
top-level functions) and so often not in cache, where `Name.global` is a
load from a Name the lexer has just touched. If the probe is measurably
slower, a cache can be added afterwards: `Name.binding` points at the active
module's Global record. Locals already save and restore `binding`, so
shadowing works without changes. The costs are rebinding names when the
active module changes, and a tag to tell a Local from a Global. Adding the
cache doesn't change anything else, so it's fine to start without it. The
table's own performance (swisstable or other layouts) probably needs a
separate investigation once it exists.

**Generic instantiation across modules (D21).** A body is instantiated by
switching the Parser to the module that defines it, with the lexer at the
body's offset. Names in the body resolve against that module's globals, so
its private helpers work, and a name that both modules define binds to the
right one. Within one module it's the same switch without changing the
Module. Instantiations are queued to the end of the current top-level item,
because locals are bound through `Name.binding`, and in the middle of a
function the body would see that function's locals. If the `Name.binding`
cache for globals is added, switching modules also has to rebind them.

**Open: can a local reuse an import's name?** (2026-11-14) Inside a
function, a name that's bound by an import has to be followed by `.`, so
`someimp = 5` is an error ("Expecting '.' after module name.") rather than
declaring a local. It's nice that module names become semi-reserved, but
adding an import to old code could make an unrelated local invalid. To be
revisited: either keep it, or let an assignment declare a local that shadows
the import, as a local already shadows any other global.

**Steps.**

1. Split `Module` out of `Parser`, with `parse_file()` creating it, and
   `FileBuf` out of `LexState`. Move globals into the module's table, with
   all lookups through it, and remove `Name.global`. Bench against HEAD on
   the desktop.
   - **Measured 2026-11-14** (desktop, `parsecheck`, dumbbench): with the
     NameId multiplied by 2^64 / phi as the hash, 778 ms vs. 680 ms at HEAD
     (14% slower), with 2.3 more cache misses and 25 more page faults per
     function. Every global in dumbbench is a declaration, so it's the
     duplicate check and insert into a table with 1.25M entries spread out at
     random. Using the NameId itself as the hash puts consecutive
     declarations in consecutive slots: 702 ms (3% slower), and almost all
     of the difference is system time from growing the table. locals64 is
     1% slower (83.8 vs. 83.1 ms).
2. Probably: improve the table's performance (swisstable or other layouts),
   depending on what step 1 measures.
   - **Measured 2026-11-14** (desktop, ms). `bench/gen_globals.py` makes
     files that reference globals: gref_rand (300k functions, 4 lines each,
     references to uniformly random earlier globals), gref_near (the same,
     but 90% to the last 64 definitions) and gref_small (2000 globals, 800
     lines each, so the table stays in cache).

     | | dumbbench | gref_rand | gref_near | gref_small |
     |---|---|---|---|---|
     | HEAD (`Name.global`) | 695.7 | 388.3 | 286.5 | 216.8 |
     | NameId as hash, 64 slots | 709.3 | 407.0 | 296.7 | 221.8 |
     | NameId × 2^64 / phi | 787.4 | 441.8 | 321.3 | 228.4 |
     | NameId as hash, presized | 695.0 | 404.9 | 290.1 | 220.3 |
     | Verstable, default hash | 779.4 | 434.9 | 326.1 | 234.3 |
     | Verstable, NameId as bucket | 710.0 | 411.6 | 305.2 | 226.5 |

     The memory access pattern decides it, much more than the table's
     code. Any hash that spreads NameIds out costs 8–13% on the large
     files, and Verstable is 1–3% slower than the simple table even with
     NameId buckets. Its strengths (deletion without tombstones, high load
     factors, chains that stay short) don't matter for a table that's
     insert-only, at most half full, and keyed by dense integers.
     Presizing to the next power of 2 that's at least file length / 32
     slots avoids regrowing, which was mostly page faults, and makes
     dumbbench level with HEAD. What's left is about 1.5 ns per reference
     (gref_small), where `Name.global` was a load from a Name the lexer had
     just touched. That isn't worth the `Name.binding` cache for now.
     Decided: keep the simple table, presized.
3. The loader callback, the dict of modules by canonical name, and loading
   imports. Driven by `test/errors/todo/import_does_not_exist.luv`. The test
   runner skips files marked `# IMPORTED`.
4. Look up `mod.name` in the imported module's table. Driven by
   `import_wrong_func`, `import_wrong_type` and
   `import_treating_module_as_variable` in `test/errors/todo/`.
5. Report import cycles as an error.

### D25 Backend and execution model (open)

Investigated 2026-11-14, then revised the same day after the user's answers.
The survey is unchanged. The direction, the IL discussion and the
recommendation near the end reflect those answers. Not yet marked decided.

#### What's being asked of a backend

- Native code for x64 and aarch64 on Windows, Linux and macOS. That's six
  OS/CPU pairs and four C ABIs (SysV x64, Win64, AAPCS64, and Apple's
  arm64 variant), plus Windows arm64 as a fifth if it's wanted.
- Compile speed is a feature. luv60's goal was to stay above 1M lines/s
  through to code. The front end here does lex and parse at about 22M
  lines/s (10M lines in 0.45 s), so the backend will be nearly all of the
  compile time whatever is chosen.
- luvr is plain C and is meant to end up as one header. A backend that
  needs C++, Rust, a generator step, or a copyleft licence works against
  that.
- The front end is single pass with no AST (D1). Code is produced in
  source order through an `expdesc` that's discharged late, behind a
  `luaK_*`-style interface. Functions are complete units, and generic
  instantiations are queued until the current top-level item ends (D21).
  So a function at a time is the natural unit to hand to a backend.

Language features that the backend has to deal with, and that most small
backends don't do for us:

- `foreign` functions and structs passed by value mean the C ABI rules for
  aggregates are needed for every target, including C calling back into luv.
- The current arena (D23) is a per-thread, dynamically scoped variable. Real
  TLS is different in every object format and is fiddly in a JIT. A pinned
  register or a hidden context argument avoids all of that.
- Constants that are evaluated at compile time already exist
  (`const_for_array_size.luv`). Anything beyond literal folding needs an
  evaluator inside the compiler.
- Nested functions capture by value (D12), so there are no closures to
  allocate. That's only a hidden argument.

A budget, to make "fast" concrete: 1M lines/s on `dumbbench.luv` is about
8 µs per function (8 lines, about 35 tokens), or a few hundred cycles per
token. 5M lines/s is under 2 µs per function. Process creation is on a
different scale. Linking a 2 MB object with the system linker took 32 ms
here, and one `clang` invocation is tens of milliseconds before it reads any
input. Anything that starts another process can't be the fast path.

#### Measurements

Laptop (noisy), best of 5, on the first 96k lines (12k functions) of
`dumbbench.c`, the C equivalent of `dumbbench.luv`:

| What | Time | Lines/s |
|---|---|---|
| `clang -fsyntax-only` | 82 ms | 1.2M |
| `clang -O0 -c` | 630 ms | 150k |
| `clang -O0 -S` | 807 ms | |
| `clang -c` on that `.s` (10.9 MB) | 909 ms | 105k |
| `clang c.o -o c.exe` (link only) | 32 ms | |

Two things follow. LLVM at `-O0` spends 85% of its time after the front end
and is 100 times slower than our parse. And assembling the text takes
longer than compiling the C did, so `.s` as an interchange format costs more
than the code generator that feeds it, before the linker runs.

luv60 is a prototype in an unknown state, so its numbers are weak evidence
and nothing below depends on them. For the record: its notes have dstogov/ir
at `-O1`, JIT in memory, doing 10M lines in 4.4 s on an M3 (2.2M lines/s
including the front end). Its current sqbe build did the 96k-line slice in
about 0.96 s including a `clang -g` link, and a sample showed about three
quarters of that in `strcmp` under sqbe's symbol handling, with QBE's own
passes (`filllive`, `spill`, `filluse`) a few percent. So that says nothing
about QBE's real speed. It would need the lookup fixed and a re-measure.

#### Execution models

| Model | Needs | Compile cost | Gives | Problems |
|---|---|---|---|---|
| Interpret bytecode | Nothing | Lowest. Lua 4's whole code generator was 0.42 s per 10M lines | Runs on every host on day one, a reference for the others, compile-time evaluation | Slow at run time. `foreign` calls and callbacks from C still need per-ABI machine code, or a restricted set of signatures |
| JIT in process | Executable memory, `dlsym`/`GetProcAddress` for foreign symbols | Low. No files, no linker | Compile and run in one step, the base for luv60's "listener" and code reload ideas | No binary to ship. Debuggers see nothing without extra work (GDB's JIT interface, `RtlAddFunctionTable` for Windows unwinding). macOS arm64 needs `MAP_JIT` and write-protect toggling |
| Write an executable directly | A writer per format | Low. One file write | A self-contained toolchain with no linker installed, which matters most on Windows | Can't link static `.o`/`.a` files without becoming a linker. Debug info has to be written by us |
| Emit `.o`/`.obj` | A writer per format, and the system linker | Medium. One linker process | Static linking with C code, and the platform's debug info flow | Needs MSVC or lld on Windows and the Xcode tools on macOS. Linker start-up is the floor |
| Emit `.s` | Assembler and linker | High, see above | Readable output | Slowest of the native routes. Only useful as a debugging dump, which a disassembly printer also gives |
| Emit C | A C compiler | Highest: 150k lines/s at `-O0`, far less optimized | Optimized code from clang/gcc/MSVC without writing an optimizer, every platform a C compiler reaches, debug info through `#line` | Not a fast path. C's undefined behaviour has to be fenced off (`-fwrapv`, `-fno-strict-aliasing`, explicit conversions) |

How hard the direct executable writers are, for a program that only links
against shared libraries:

- **ELF** is the easy one. Program headers, `PT_INTERP`, a dynamic section
  with `DT_NEEDED`, and a GOT filled from relocations. A few hundred lines.
- **PE** is nearly as easy. Headers, sections, an import table per DLL, and
  optionally base relocations. `.pdata` unwind entries are needed for
  anything that unwinds through our frames (debuggers, SEH).
- **Mach-O** is the most work. An executable has to be dynamically linked
  against `libSystem` (there's no stable syscall ABI), needs dyld bind
  information or chained fixups, and on arm64 has to carry an ad-hoc code
  signature, which is SHA-256 over each page plus a small blob. tcc, Go and
  Zig all do this themselves, so it's known to be doable in small code.

A JIT and a direct executable writer are nearly the same thing. Both take
code and data in section buffers with relocation records against a symbol
table. One applies the relocations in memory and the other lays the
sections out behind headers and import tables. Writing `.o` files later is
a third consumer of the same records. So the choice between them doesn't
need to be made up front if the code generator emits into sections with
relocations rather than into "the JIT buffer".

Debug info is the largest hidden cost of every route except C. DWARF line
tables are a few hundred lines, and types and locals are much more. On
macOS the debug info normally stays in the `.o` files and is collected by
`dsymutil`, so a direct writer has to produce a dSYM or embed the sections
itself. On Windows the native format is CodeView in a PDB, and writing PDBs
is a project by itself. None of the small backends below solves this either:
QBE carries line numbers only, MIR and ir have nothing beyond JIT hooks, and
tcc has DWARF on ELF.

#### Existing backends

| Backend | What it is | Lang, licence | Targets | In-process API | Output | Compile speed | Code quality |
|---|---|---|---|---|---|---|---|
| LLVM | Optimizing compiler framework | C++, Apache 2 | Everything | Yes | obj, JIT | Slow, see above | Best |
| Cranelift | Optimizing code generator from wasmtime | Rust, Apache 2 | x64, aarch64, riscv64, s390x. ELF, Mach-O, COFF through a Rust crate | Rust only, no maintained C API | obj, JIT | Faster than LLVM, by a small factor rather than an order of magnitude | Good |
| QBE 1.3 (2026-06) | SSA backend that reads a text IL | C, MIT | amd64 SysV, Apple and (new in 1.3) Windows, arm64, arm64 Apple, rv64. No Windows arm64 | No. It's a filter from IL text to assembly text | `.s` only | Limited by text in and text out | "70% of the performance of industrial optimizing compilers in 10% of the code" is its stated aim |
| sqbe | luv60's fork of QBE as a single header with a C API | C, MIT | As QBE | Yes | `.s`, Mach-O and COFF objects, JIT. No ELF object | Not known, see Measurements | As QBE |
| dstogov/ir | JIT framework, the base of PHP's JIT since 8.4 | C plus DynASM, MIT | x86, x64, aarch64. Exercised on all three OSes through PHP | Yes | Memory. Can also print C and LLVM text | Fast. Its README claims up to 40 times faster than `gcc -O2` | README claims about 5% slower than `gcc -O2` on its C benchmarks |
| MIR | JIT with its own IR, an IR interpreter, and a C11 front end | C, MIT | x64, aarch64, ppc64, s390x, riscv64 | Yes | Memory only | Fast | Around `-O1` |
| TinyCC (mob) | One-pass C compiler with its own assembler and linker | C, LGPL 2.1 | x86, x64, arm, arm64, riscv64. ELF, PE, Mach-O | `libtcc`, which takes C source | exe, obj, memory | The fastest C compiler by a wide margin | Poor, about `-O0` |
| xcc | C compiler, assembler, linker and libc | C, MIT | x64, aarch64, riscv64, wasm. README says ELF64 only | No, command-line tools | `.s` through its own `as` and `ld` | Not stated | No optimizer yet (it's on the TODO list) |
| rat | C99 compiler with a sea-of-nodes optimizer and its own linker | C, MIT | x86 only so far. ELF, with COFF and DWARF in progress | No, a `ratcc` driver | obj, exe | README: `-O1` builds `sqlite3.c` about 17 times faster than `clang -O1` | README: within 5% of `gcc -O1` |
| TPDE | Framework for single-pass back ends over an existing SSA IR | C++20, Apache 2 | x64 and aarch64, ELF only | C++ | obj, JIT | 8–24 times faster than LLVM `-O0` on SPECint | About LLVM `-O0`, within ±9% |
| sljit | Portable low-level assembler, used by PCRE2 | C, BSD | x86, x64, arm, arm64, riscv, ppc, mips, s390x, on all three OSes | Yes | Memory only | One pass, very fast | Whatever is asked for. It has no register allocator |
| Tilde (TB) | Sea-of-nodes backend library | C, MIT | Mostly x64 | Yes | obj, JIT | Fast | Aims at `-O1` |
| Copy and patch | A technique: stencils compiled ahead of time by clang, copied and patched at run time | n/a | Whatever clang targets | n/a | Memory | The paper reports about 100 times faster than LLVM `-O0` | Around `-O0`, sometimes better |

Notes on each, in terms of what this project needs.

**LLVM and Cranelift.** Ruled out as the main path for the reasons already
suspected. LLVM is two orders of magnitude larger than the rest of the
compiler would be, and even its fastest mode misses the speed goal by ten
times. Cranelift is a reasonable backend, but it brings a Rust toolchain
and a hand-written C shim, and its compile times are in LLVM's league
rather than tcc's. If optimized output is wanted, emitting C gets LLVM's or
MSVC's optimizer without linking either.

**QBE and sqbe.** This is the only candidate where every ABI except Windows
arm64 already works, and sqbe has already added an API, object output for
two formats, and a JIT. It also produces better code than anything we'd
write quickly. The doubts are speed and size. QBE runs a dozen passes over
a pointer-based SSA IR, and upstream has never had to care about throughput
because it's fed by a text parser. sqbe.h is 24k lines, which would be
several times the size of everything else in luvr. ELF object output and
debug info beyond line numbers would be ours to add. Whether its speed is
acceptable isn't known and is cheap to find out.

**dstogov/ir.** The fastest of the real optimizing backends by luv60's own
numbers, and it's in production on all three OSes. Against it: the
instruction selectors are DynASM `.dasc` files (11k lines for x86, 6.6k for
aarch64) that need a Lua preprocessing step, though the generated C could
be checked in. It has no object or executable output. Aggregates in the C
ABI are left to the caller. Its README says the API isn't stable. luv60
moved from ir to sqbe in June 2025, and the reason isn't recorded in the
notes.

**MIR.** Similar in shape to ir, with a wider set of CPUs and its own
interpreter for the IR. It's JIT only, and its author has said he has much
less time for it. luv60 tried and removed it in January 2025.

**TinyCC.** Its architecture is the closest match there is to this front
end. It's Lua 4 for C: one pass, a value stack, code emitted as the parser
goes, one `*-gen.c` of a couple of thousand lines per CPU, and its own ELF,
PE and Mach-O writers including the arm64 code signature. As a dependency
it fits badly. The supported interface is C source text, so the program
would be lexed and parsed twice. The internal code generator interface is
not an API. LGPL doesn't sit well in a single-header library. The last
release was in 2017, though the `mob` branch is active daily. It's the best
thing to read while writing our own, especially `tccelf.c`, `tccpe.c` and
`tccmacho.c`.

**xcc.** A clean, small, complete toolchain for three CPUs and wasm, but
it's a C compiler that drives its own assembler with text, has no Windows
or (per its README) Mach-O output, and has no library interface. Useful
reading for encoders and for a small linker. Not something to build on.

**rat.** The most interesting as an optimizer: sea of nodes, GVN, SCCP,
inlining, even SLP vectorization, with fast compile times claimed. It's
x86 only, one author, changing daily, and built as a C compiler rather than
a library. Worth watching as a possible optimizing tier in a few years. Not
usable now.

**TPDE.** Not usable (C++, ELF only), but it's the best evidence for a
design: one pass over each function that selects instructions and
allocates registers together, given liveness and loop information, gets
`-O0`-or-better code at 10–20 times LLVM's `-O0` speed. Baseline JITs for
wasm and JavaScript are built the same way.

**sljit.** If only a JIT were wanted, this would be the shortcut. It
supplies encoders, function entry and exit for each ABI, and executable
memory handling including Apple's rules. It can't produce objects or
executables, so it's a dead end for anything that ships a binary.

**Copy and patch.** luv60's `gen.c` "snip" experiments were in this family,
and its notes record the jump-patching trouble. It pays off when each
operation is a large piece of code, as in a dynamic language. For a typed
C-like language most operations are one or two instructions, so a stencil
saves little over a hand-written encoder, and register allocation ends up
fixed by the stencil set. It also puts clang into luvr's own build.

**Tilde.** Mostly x64, development has slowed, and Odin removed its Tilde
backend. Not a candidate.

What other small-team languages did, for calibration:

- Zig wrote its own x64 backend and linkers. It became the default for
  debug builds on Linux in 2025, isn't yet the default on Windows (COFF
  linker work), and aarch64 isn't ready. Release builds still use LLVM.
  Reported debug build times dropped several times over.
- Go has its own backends, assembler and linker for every target, and is
  the standing proof that it scales.
- Hare uses QBE and so doesn't target Windows or macOS.
- V and Nim emit C. V bundles tcc so that development builds are fast.
- Odin and C3 use LLVM, and both tried Tilde as a fast alternative.

The pattern among the ones that care about build speed is the same: an
in-house fast path for development, and someone else's optimizer for
release.

#### Writing our own

Rough sizes for the plan recommended below, in lines of C. They're
estimates from the sizes of the same jobs in tcc, QBE and LuaJIT (from
memory), and could easily be off by half in either direction.

| Piece | Lines |
|---|---|
| IL: definition, builder, SSA construction, fold and CSE at emit, dump | 1.5–2k |
| ABI lowering: Win64, Apple arm64, SysV x64, with structs by value | 0.8–1k |
| Register allocation and the selection driver shared by both CPUs | 1–1.5k |
| aarch64 selection and encoding | 1.5–2k |
| x64 selection and encoding | 2–2.5k |
| JIT glue for three OSes (memory, symbols, Win64 unwind tables) | 0.4–0.6k |
| **Fast tier on all three targets** | **7–10k** |
| Optimizing passes, 0.3–0.8k each | 3–5k for a useful set |
| Executable writers: ELF 0.4k, PE 0.6k, Mach-O with signature 1k | 2k |
| Loader for external `.o`/`.obj`/`.a` files, three formats | 2–3k |
| Debug info | depends entirely on the form chosen |

For scale: the front end is about 3.5k lines today, sqbe.h is 24k, and ir
is about 35k plus its DynASM files. So the fast tier would be two to three
times the current front end and well under half of sqbe. It is the
largest single piece of the project.

It doesn't arrive all at once. A first milestone of aarch64 macOS only,
with integers, pointers, branches and calls to luv and to simple foreign
functions, is the IL, one classifier, the allocator and half an encoder:
about 3–4k lines, and it runs most of the existing tests. Floats, structs
by value, x64 and the other two ABIs are each separate steps after that.

Where the size and risk are, and what the fallbacks are:

- The backward pass that allocates and selects together is the least
  conventional part. If it gets complicated at joins and loops, the
  fallback is a liveness pass followed by an ordinary forward linear scan.
  That's about the same amount of code and one more walk over the function.
- SysV x64 struct classification is fiddly but small and completely
  specified, and it can be tested exhaustively against the C compiler.
- x64 encoding is the larger of the two encoders because of its
  irregularities. Restricting the fast tier to a fixed set of instruction
  forms keeps it down.
- If the whole thing proves too much, the IL lowers to QBE's, and sqbe
  becomes the backend with the front half kept.

#### Direction from the user (2026-11-14)

- Running in process is fine to start with.
- Targets to begin with are x64 Windows, x64 Linux and aarch64 macOS only.
  Windows arm64 is out for now.
- Differences between backends should be avoided as much as possible. That
  means observable behaviour, and also capability: no "only this mode can
  use external `.o` files" and no "this backend only works on Windows". A
  user should be able to pick any tier and any way of running in whatever
  context suits their project. Two tiers are fine: a fast compile, and a
  slower compile that gives faster code.
- Linking with C objects is wanted on the fast path, but it can be done by
  the user building a host program ahead of time that contains the C code
  and runs the luv code. So the fast path needs no object writer or linker.
- Structs passed by value to and from C are required for `foreign`. This is
  why luv60 moved from ir to sqbe, so ir is out.
- Debug info is deferred unless it blocks something. It may be limited to
  one platform (raddbg only, or PDB with MSVC), or the effort may go into a
  luv debugger with its own simpler format. The user's `dyn_basic_pdb.h`
  writes PDBs for JIT code but only has file and line.
- A C backend is in doubt. Debugging luv through generated C with its
  temporaries and helper calls may be too strange.
- Compile-time evaluation is only basic constant folding.

These change the earlier draft of this section in three ways. The
interpreter loses most of its case: no compile-time evaluation needs it, the
three targets are all hosts that can JIT, it couldn't do struct-by-value
foreign calls without machine code anyway, and it would be one more backend
to differ. The C emitter is a second semantics (undefined behaviour,
evaluation order, float contraction) as well as awkward to debug, so it
moves to "only if a platform we can't generate code for ever matters". And
the wish for one behaviour across tiers means the tiers should be the same
backend with more or fewer passes, which makes the choice of IL the main
design question.

#### Keeping the modes equal

The user's rule is that every combination should work: either tier, on
every target, run in process or (later) written out. The plan keeps to it
by making each choice independent of the others:

- A tier is a list of passes over the IL. It doesn't know which CPU or OS
  is being targeted or where the code will go.
- A target is an ABI classifier plus an encoder. Both tiers go through the
  same ones.
- A consumer (JIT, later an executable or object writer) takes sections
  and relocations. It doesn't know which tier produced them.
- Anything to do with foreign code belongs to the layer that resolves
  symbols, below all three. To begin with there's one rule everywhere: C
  code lives in the host program or a shared library. If loading `.o`
  files is added, it's added there, as a reader that turns an object file
  into the same sections and relocations, so the JIT and any executable
  writer get it together. An executable writer that could link objects when
  the JIT couldn't, or the reverse, is what to avoid.

The practical consequence for sequencing is that a feature isn't done until
it works on all three targets and both tiers, and the test runner should
run every test in every combination that exists.

#### Where this is headed

The user's reference for where the project should end up is the Tomorrow
Corporation tech demo (https://www.youtube.com/watch?v=72y2EC5fkcE). From
memory of that talk, not re-checked: their own language, compiler and
debugger, a whole game rebuilt in a fraction of a second, code reloaded
into the running game, and the full run recorded so that execution can be
scrubbed and stepped backwards. luv60's README goals (saved inputs,
incremental code update, debugger integration) are the same picture.

What that asks of the backend, to keep in mind now though none of it is
built yet:

- Functions have to be replaceable one at a time in a running program. The
  per-function IL and per-function code with retained relocations already
  suit this. Calls between luv functions would go through something
  patchable (a table, or a jump at the old entry) in the development tier.
- Globals and types need a story for when their layout changes under a
  running program, which luv60's notes had started on. That's a language
  and runtime question more than a backend one.
- Recording and replay want all input and nondeterminism to come through
  the host, which fits the host-program model.
- A debugger of our own makes the debug info format ours to choose. That
  supports deferring PDB and DWARF, and it means the information the IL
  and the fast tier keep (source offsets, where locals live) is the real
  requirement.
- It favours the JIT and in-process model as the main one, with written
  executables as the shipping step.

#### The IL

The single-pass front end doesn't force a push/pop code generator. The
front end produces the IL in one pass, a function at a time, and what
happens to a finished function is separate. Three levels:

1. **No IL: machine code from the parser** (tcc). Locals in stack slots, a
   value stack for expressions. Nothing can be improved afterwards, and a
   better tier would be a different backend.
2. **Register bytecode, not SSA** (Lua). Virtual registers are locals and
   expression temporaries. What's affordable without SSA: constant folding
   in `expdesc` before anything is emitted (Lua 4 does this), fusing
   compares with branches and folding address arithmetic into addressing
   modes at selection time, and giving the most used locals a register for
   the whole function from use counts weighted by loop depth. That's well
   ahead of push/pop code. It stops there, though: with no def-use
   information there's no CSE, no dead store removal, and nothing for a
   second tier to build on.
3. **Linear SSA in a flat array, built during the single pass.** This is
   the shape of LuaJIT's IR, which dstogov/ir also follows: fixed-size
   instructions in one array, operands are indices, constants in the same
   array growing the other way, reset per function. SSA can be built
   directly by a single-pass front end with no dominance computation
   (Braun et al., "Simple and Efficient Construction of Static Single
   Assignment Form", 2013, which is what Cranelift's front end uses): each
   block keeps a small map from local to current value, and a read that
   misses asks the predecessors and adds a phi where they disagree. Loop
   headers get provisional phis that are resolved when the loop ends. It's
   a few hundred lines.

What level 3 gives nearly for free, in the fast tier:

- Folding and algebraic simplification as each instruction is emitted.
- CSE by hashing each instruction as it's emitted, within the dominating
  code that's already been seen.
- Dead code removal from use counts.
- Register allocation, instruction selection and encoding in **one backward
  pass** over the array, as LuaJIT's assembler does. Going backwards, a
  value's last use is seen first, so liveness needs no separate analysis,
  and each instruction can see how its result is used (as an address, as a
  branch condition) when it's selected. Control flow joins and loops need
  more care than in a trace compiler, which is the part TPDE's design
  shows how to do.

What the slower tier adds, on the same IL and through the same selection,
allocation and ABI code: inlining, moving loop-invariant code, global value
numbering, load and store forwarding, a better allocator. These are
ordinary passes that can be added one at a time, and each can be turned off
to find a miscompile. Both tiers share everything that defines behaviour,
so they differ only in which rewrites ran.

Level 3 is recommended. It costs more than level 2 up front (the SSA
construction, phi handling in the allocator) and compiles somewhat slower.
The budget has room: ir, which has this shape plus a real pass pipeline,
was the fastest backend luv60 tried. It's also the level at which "fast
tier" and "optimized tier" are one backend.

Struct passing is handled the way QBE does it, which is known to work for
this language. Aggregates are memory in the IL. A call or parameter that
takes a struct by value says so with the struct's layout, and a per-target
ABI step rewrites that into registers, stack slots and hidden pointers
before selection. Three classifiers are needed for the first targets: Win64
(simple, by size), Apple arm64 (homogeneous float aggregates, 16-byte
rule), and SysV x64 (the eightbyte classification, the hard one). A
generated test that passes many struct shapes both ways across a C boundary
and compares the bytes would be written with it. sqbe stays useful here as
something to compare against.

For debug info, whichever way it goes later, two things are worth keeping
from the start because they're cheap now and awkward to add: every IL
instruction carries a source offset, and the fast tier can report where each
named local lives.

#### Recommendation (revised 2026-11-14)

1. **A linear SSA IL per function**, with folding and CSE as it's built.
   D26 has the design, and explains why SSA is made by one pass at the end
   of each function and not while parsing. Expression typing produces this. A text
   dump of it is the first testable output.
2. **One backend of our own: ABI lowering, then a backward pass that
   allocates registers, selects and encodes**, for x64 and aarch64, emitting
   into sections with relocation records.
3. **A JIT as the only consumer to begin with.** The library compiles and
   runs in process. Foreign symbols are resolved by the host, through a
   callback on Ctx in the same style as D24's file loader, with `dlsym` and
   `GetProcAddress` as the default. C code is linked into the host program
   by the user. Order of bring-up: aarch64 macOS (the laptop), x64 Linux
   (the desktop), then x64 Windows, which adds only Win64 and unwind tables
   to the x64 work.
4. **The optimized tier is passes on the same IL**, added when there's
   real code that's too slow. Nothing needs to be designed for it now
   beyond choosing SSA.
5. **Later, as needed:** executable or object writers as further consumers
   of the same sections, debug info in whatever form is chosen, and a C
   emitter only for platforms without a native generator.

Not planned: an interpreter, `.s` output, any third-party backend as a
dependency.

sqbe is the fallback and the comparison. If the backward-pass generator
turns out harder than expected, lowering this IL to QBE's is mechanical,
since both are SSA with the same view of aggregates. It also remains the
quickest way to check our ABI lowering and code quality against something
independent.

What this gives up:

- The first native "hello world" is further away than with sqbe, since the
  IL, one ABI, one encoder and the allocator all have to exist first.
- Until the optimizing passes exist, code is somewhere between tcc and
  `-O1`.
- Two instruction selectors and three ABIs are ours to get right. With no
  interpreter, the checks are the tests' expected output, the two CPUs
  against each other, the fast tier against the optimized tier, and the
  struct-passing test against C.
- Standard debuggers see nothing in JIT code until debug info is done.

#### Still open

1. The exact IL. See D26 (provisional).
2. Whether the current arena (D23) is a pinned register or a hidden
   argument. D26 proposes a pinned register that holds the context, with
   the arena as a field of it.
3. How good the fast tier's code needs to be for interactive use, which
   decides how early the optimizing passes are wanted.
4. Debug info: platform and format, or our own debugger. Deferred.
5. Whether a shipped executable without a host program is ever needed, and
   so whether the executable writers get written.
6. Whether loading external `.o` files is wanted, beyond C code in the host
   program or a shared library. If so it's a loader below every consumer.

#### Sources

- QBE: https://c9x.me/compile/ and https://c9x.me/compile/releases.html
- TinyCC: https://bellard.org/tcc/ and https://repo.or.cz/tinycc.git
- xcc: https://github.com/tyfkda/xcc
- rat: https://github.com/hexratcc/rat
- ir: https://github.com/dstogov/ir and https://wiki.php.net/rfc/jit-ir
- MIR: https://github.com/vnmakarov/mir
- TPDE: https://arxiv.org/abs/2505.22610 and https://github.com/tpde2/tpde
- Copy and patch: https://arxiv.org/abs/2011.13127
- SSA construction: Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau, "Simple and
  Efficient Construction of Static Single Assignment Form", CC 2013
- Tilde: https://github.com/RealNeGate/Cuik and
  https://forum.odin-lang.org/t/tilde-backend-got-removed/1697
- Zig's x86 backend: https://ziggit.dev/t/self-hosted-x86-backend-is-now-default-in-debug-mode/10447
  and https://mitchellh.com/writing/zig-builds-getting-faster

Not checked against a primary source, from memory: Cranelift's lack of a C
API and its relative compile speed, sljit's and MIR's target lists, tcc's
per-file sizes and its Mach-O arm64 support, what Go, Hare, V, Nim, Odin and
C3 use, and the descriptions of LuaJIT's IR and backward assembler pass and
of Cranelift's use of Braun's algorithm, and the contents of the Tomorrow
Corporation demo.

---

### D26 The IL (provisional)

Written 2026-11-14, before expression typing exists, so that typing has
something to produce and a dump to test against. Everything here is
expected to change as it's used. The parts most likely to change are listed
at the end.

The two references are LuaJIT's IR and dstogov/ir, which follows it. Both
keep a function's instructions in one flat array, refer to operands by
index, and keep constants in the same index space. LuaJIT's instructions
are 8 bytes with 16-bit operands, which limits a trace to 64k entries.
ir's are 16 bytes with 32-bit operands and room for a third operand.

#### Summary

- One function at a time. All of it is reset when the function is finished.
- Instructions are 8 bytes: an opcode, a type, and two 24-bit operands.
- Operands are references. A reference below a bias is a constant, and at
  or above it is an instruction.
- Value types are `i32`, `i64`, `f32` and `f64`. Narrower integers exist
  only in memory and in extension operations.
- Aggregates are memory. The IL has no struct values.
- The front end never emits a phi. It reads and writes locals as frame
  slots. One forward pass at the end of the function turns slots whose
  address was never taken into SSA values, and folds and removes common
  subexpressions as it goes.
- Blocks are in source order, and control flow is structured.
- The luv context pointer is in a pinned register.

#### Two stages

The earlier plan was to build SSA while parsing. That doesn't work for one
reason: whether a local can be an SSA value depends on whether its address
is ever taken, and `&x` can come after `x` has been read and written many
times. Switching `x` to memory at the `&x` is unsound in a loop. A pointer
from an earlier iteration can write the slot before the `&x` is reached
again, and the code between the loop header and the `&x` was already
emitted reading the SSA value.

So there are two stages, which is also what ir does with its `VAR`,
`VLOAD` and `VSTORE` and a later pass:

1. **Raw.** The parser appends instructions to a buffer. Every local is a
   frame slot, read with `LOAD` and written with `STORE`. Nothing is folded
   or searched for, so emitting is an append. Constant expressions are
   already folded in `expdesc` before anything is emitted, as Lua 4 does,
   and that's also what evaluates `[BLAH]int`.
2. **SSA.** When the function ends, one forward pass copies the raw buffer
   into the final one. A slot that was only ever loaded and stored directly
   is promoted: its stores define values, its loads are replaced by the
   current value, and phis are placed where control flow joins. Each copied
   instruction goes through folding and CSE. Slots that weren't promoted
   keep their loads and stores.

The parser is still single pass. The cost is one more linear walk and a
second buffer of the same size. Both buffers are reused for every function.

The front end not emitting phis keeps it simple. A value that depends on
control flow, like the result of `a and b` or a comprehension's
accumulator, is a compiler temporary slot that's stored on each path and
loaded after the join. The SSA pass turns that into a phi like any other
local.

#### Could the SSA pass be removed?

Considered 2026-11-15. Disallowing `&x` on scalar and pointer locals would
make it possible to build SSA while parsing, since that's the only way a
local's escape is found late. Captures are by value (D12), so they don't
add another. Not done, for these reasons:

- It doesn't make the backend one pass. The backward code generation pass
  needs the whole function, so the walks per function go from three
  (parser, SSA, code generation) to two, not to one.
- The SSA pass also knows each loop's stored list before it reaches the
  loop header, so its phis are exact. Building SSA while parsing, the
  header is emitted before the body is seen. That means a provisional phi
  for every local read in the loop, as in Braun et al., with the unneeded
  ones left as copies or patched through a forwarding array. Code already
  emitted in the body saw an opaque phi, so loop-invariant locals and
  constants don't fold or CSE inside the loop.
- The parser would have to track definitions per block and emit phis for
  `and`, `or` and comprehension accumulators itself.
- The language cost is mostly C interop: `strtol(s, &end, 10)`,
  `sqlite3_open(path, &db)` and other out-parameters, of which `**T` is
  very common. D5b also currently names `*int out` as the way to return a
  second value. The workaround would be a `[1]int` or a one-field struct.
- The folding, CSE and phi lookup move into the parser. They don't go
  away. What's saved is the walk, the second buffer, and the local `LOAD`,
  `STORE` and `SLOT` instructions that are emitted and then dropped.

If one stage is wanted later, a narrower rule than a ban would do: a local
whose address can be taken is marked at its declaration, so it's memory
from its first use. The loop phi problem remains.

The worry was memory bandwidth in long functions. None of this is
measured, but the expectation is that it's minor:

- Both buffers are walked sequentially, 8 bytes per instruction. A
  streaming read of the 207 MB dumbbench source is 0.05 ns/byte, against
  1.81 ns/byte for the full lexer (section 5).
- The IL is about the size of the source that produced it, so the pass
  streams about as many bytes as the lexer did.
- Folding, the CSE probe and operand mapping are probably several ns per
  instruction, and exist in a one-stage design too. Moving 16 bytes
  sequentially is well under 1 ns.
- Both buffers are reused per function. A function of 10k instructions is
  80 KB per buffer. Only generated functions near 1M instructions (8 MB per
  buffer) reach memory at all.

The random access is what to watch in long functions, and the number of
passes doesn't change it: the CSE hash chain, the per-block slot
definitions and their walk through predecessors, and operand references
and the register and spill arrays in the backward pass. The two stages add
the raw-to-final reference map, 4 bytes per raw instruction, whose accesses
are mostly to recent entries.

The raw stage is internal to the compiler, so rewriting in place (item 3
below) or skipping the stage for functions that never take an address can
be done later without changing the language. A bench input that's one
function of a few hundred thousand statements should be added once the
pass exists.

#### Instruction format

```
typedef u64 Ins;   // op:8 | ty:8 | a:24 | b:24
typedef u32 Ref;   // 24 bits used
```

One `u64` with shifts, not bit-fields, so the layout doesn't depend on the
compiler.

Bit 23 of a `Ref` (`REF_INS`) says that it's an instruction, and the other
23 bits are its index in `ins`. Otherwise, it's the constant at
`consts[ref - 1]`, and reference 0 means no operand. Instructions and
constants are in separate arrays. That gives 8M instructions and 8M constants per function. Running
out is an error, "function too large". Every constant's reference is below
every instruction's, which the CSE search relies on (see below).

Smaller was considered. LuaJIT's 16-bit operands would cap a function at
32k instructions, which generated code would hit. 16 bytes as in ir would
double the memory that every pass walks, for the sake of a third operand
that few operations need.

Operations with more than two operands (calls, phis, block copies) are
followed by extension slots. Each is an `Ins` with opcode `EXT` holding
two more references. The count comes from the instruction itself (the call
descriptor, the block's predecessor count). Passes that walk the array skip
`EXT` by its opcode.

Nothing else is stored in the instruction. Per-instruction data that only
some stages need is in parallel arrays indexed by reference: the CSE chain
during the SSA pass, and the register and spill slot during code
generation. LuaJIT keeps these in the instruction's fourth 16-bit field.
We've spent those bits on operand range instead.

To revisit (2026-11-15): whether a third field in the instruction is worth
a smaller range. With the CSE link or the register and spill slot inline,
a chain step or an operand lookup touches one cache line, not two.

- LuaJIT's layout (8/8/16/16/16) allows 32k instructions and 32k
  constants. A function that fits is at most 256 KB of IL and 128 KB of
  side array, which stays in L2, so the second line is cheap. The raw
  stage reaches the limit first, since every local access is a `SLOT` and
  a `LOAD` or `STORE`. Probably a function of a couple of thousand lines.
- 6/4/18/18/18 allows about 131k of each. It leaves no spare bits, caps
  the opcode count at 64 (the provisional list is already around 60) and
  the types at 16, and generated code can still reach the limit.
- Whichever layout is used, bound the chain walk as LuaJIT does: chains are
  per opcode, and the search stops below the larger operand reference,
  since a match can't be defined before its operands. A 16-bit backwards
  delta with an escape value would also halve the chain side array.

Not measured. Decide with the long-function bench input once the SSA pass
and a code generator exist.

To revisit (2026-11-17): one array for both, with constants as
instructions. LuaJIT's constants are IR instructions (`KINT` with the value
inline, and an extra slot for 64-bit values), allocated downwards from the
bias in the same buffer as the instructions, which grow upwards. It keeps a
base pointer biased so that `ir[ref]` works for both. Growing at the bottom
moves everything up by a quarter if the top is mostly free, and otherwise
reallocates at double the size with new room below. The biased pointer is
usually outside the allocation, which is undefined in C but works.
Reserving the whole range of address space (2^24 × 8 bytes = 128 MB per
buffer) and committing pages as they're used would give the same `ins[ref]`
without moving or copying anything, and without the out-of-range pointer.

- The benefit is that a constant would be an `Ins` with opcode `KINT` and
  its type in the type field, with values that fit in the 48 operand bits
  inline and others in a second slot. `ins_op()` and `ins_type()` would
  then work on any operand without checking which kind it is, and the
  separate type array goes away. That matters in passes that look at every
  operand (folding, the code generator), not while emitting.
- `Ref` is opaque outside `il.c`, so this only changes `il.c`: constants
  would be allocated downwards from `REF_INS`, as LuaJIT's are from its
  bias.
- Decide when the SSA pass exists and can be measured.

#### Types

| Type | Used for |
|---|---|
| `i32` | `bool`, `i8`–`i32`, `u8`–`u32` once loaded |
| `i64` | `i64`, `u64`, `int`, pointers |
| `f32`, `f64` | floats |
| `i8 u8 i16 u16` | only as the type of `LOAD`, `STORE`, and the extensions |
| `void` | instructions with no result |

This follows QBE. Signedness belongs to the operation (`DIV` and `UDIV`,
`LT` and `ULT`, `SHR` and `SAR`), not the type. Arithmetic on a narrow type
is done in `i32`, and the front end emits a `SEXT8`/`ZEXT8` or similar
where the narrow result is observed. Folding removes the redundant ones.
The encoders then only need 32-bit and 64-bit forms.

A pointer isn't a separate type from `i64`. All three targets are 64-bit.

#### Constants

A constant occupies one slot below the bias, holding 64 bits of payload,
with its kind and type in a parallel byte array. Kinds:

- `KINT`: an integer of either width.
- `KFLT`: a float of either width, as its bits.
- `KSYM`: the address of a symbol (a function, a global, string data). The
  payload is a symbol index and a 32-bit addend. These become relocations.

Constants are interned per function through a small hash, so equal
constants have equal references and comparing references is enough for CSE.

#### Slots

A slot is a piece of the function's frame. The slot table holds size,
alignment, a scalar type if it has one, and an `escaped` flag.

- `SLOT k` is an instruction whose value is the slot's address.
- A named scalar local is a slot. Reading it is `LOAD ty, SLOT k`, and
  writing is `STORE ty, value, SLOT k`.
- Parameters and captured values are not slots. They're immutable and their
  address can't be taken, so that they can always be values: `PARAM n` for
  a parameter, used directly wherever it's read.
- When the front end uses a slot's address for anything other than the
  address operand of such a `LOAD` or `STORE` (`&x`, passing it, indexing
  into it), it sets `escaped`. Structs, arrays and anything else that's an
  aggregate are escaped from the start.
- A slot that isn't escaped when the function ends is promoted.

The front end also keeps, for each loop, the list of slots stored inside
it. A nested loop's list is added to its parent's when it closes. The SSA
pass uses these to place phis at loop headers without guessing.

A read of a promoted slot before any write produces zero, which matches the
language's zero initialization.

#### Blocks and control flow

`BLOCK` starts a block and is the only way one starts. Its operand is an
index into the block table, which holds the block's first instruction, its
predecessor list, its loop depth, and a loop header flag. A block ends at
the next `BLOCK`.

Terminators:

- `JMP b`: go to block `b`.
- `BR c, b`: if `c` is zero go to block `b`, otherwise continue into the
  next block. `BR` is always the last instruction of its block.
- `RET` and `RET v`.
- A block with no terminator continues into the next one.

`BR` with one target and a fall-through matches both how the parser emits
(jump over the body if the condition is false) and what the encoder
produces (a compare and one conditional jump).

Two invariants, which hold because luv only has structured control flow
(`if`, `for`, `break`, `continue`, `return`, `and`, `or`, `with`):

1. Blocks are in source order.
2. Every predecessor of a block comes before it, except for the back edges
   of a loop, which go to a block flagged as a loop header.

A lot of the design leans on these. The SSA pass can be one forward walk.
The immediate dominator of each block can be found in the same walk, since
for a graph like this one visit in this order is exact. The code generator
can use loop depth for spill decisions without finding loops. If the
language ever gets `goto`, these need revisiting.

#### The SSA pass

For each block in order, and each instruction in it:

- `STORE` to a promoted slot records the value as the slot's current
  definition in this block and emits nothing.
- `LOAD` from a promoted slot is replaced by the slot's current definition.
  If this block has none, it's looked up through the predecessors, as in
  Braun et al.'s algorithm: one predecessor means asking it, and several
  mean asking each and creating a `PHI` in this block if they differ. The
  answer is recorded in the block so it's only found once.
- At a loop header, a `PHI` is created up front for each promoted slot in
  the loop's stored list. Its back-edge operands are filled in when the
  loop's last block has been processed. Because the stored list is known,
  there are no provisional phis to remove afterwards.
- Anything else is copied with its operands mapped to their new references,
  through the folder and then the CSE lookup.

Folding covers constant operands, identities (`x + 0`, `x * 1`, `x & x`),
and extensions of values that are already in range. CSE applies to pure
operations only. A match found through the hash chain is used if the block
that defines it dominates the current block. Loads are not matched in the
fast tier, since that needs to know what a store might alias.

`PHI`'s operands are in the order of its block's predecessor list, in
extension slots when there are more than two.

Dead code isn't removed in this pass. The code generator walks backwards,
so it reaches a value's uses before its definition, and a pure instruction
that nothing has asked for by then is skipped. Its operands are then never
asked for either.

#### Operations

Provisional. The exact set of conversions and comparisons will follow from
expression typing.

| Group | Operations |
|---|---|
| Integer arithmetic | `ADD SUB MUL DIV UDIV REM UREM NEG` |
| Bitwise | `AND OR XOR NOT SHL SHR SAR` |
| Float arithmetic | `FADD FSUB FMUL FDIV FNEG` |
| Integer compare, result `i32` 0 or 1 | `EQ NE LT LE GT GE ULT ULE UGT UGE` |
| Float compare | `FEQ FNE FLT FLE FGT FGE` |
| Width | `SEXT8 SEXT16 SEXT32 ZEXT8 ZEXT16 ZEXT32 TRUNC` |
| Int and float | `ITOF UTOF FTOI FTOU FEXT FTRUNC BITCAST` |
| Memory | `LOAD STORE SLOT COPY ZERO` |
| Control | `BLOCK JMP BR RET PHI` |
| Calls | `PARAM PARAMAGG CALL ARGAGG` |
| Other | `CTX EXT NOP` |

The operand type is the instruction's type for arithmetic. For compares
and conversions, where the result type differs from the operand type, the
opcode says which is meant, so some of these will split by width (`EQ` and
`EQ64`, or a type on the operand side). That's to be settled with the first
encoder.

Address arithmetic has no operations of its own. A field access is `ADD
ptr, KINT offset`, and an index is a `MUL` and an `ADD`. The code generator
sees these when it selects the `LOAD` or `STORE` that uses them and turns
them into an addressing mode.

`COPY dst, src` and `ZERO dst` take a constant size in an extension slot.
They're how aggregates are assigned and initialized.

A static table gives each opcode its operand count, whether it's pure,
whether it's commutative (for CSE to order operands), and whether it ends a
block.

#### Calls and the calling convention

Calls between luv functions use the platform's C calling convention. That
means one convention to implement and test per target, and it means C can
call a luv function with the signature it appears to have.

`CALL callee, desc` is followed by the arguments in extension slots. The
callee is a `KSYM` or a computed value. `desc` indexes a table of call
descriptors: argument count, where variadic arguments start (they're
passed differently on Apple arm64 and Win64), and the type record for an
aggregate result. The instruction's type is the scalar result type, or
`void`.

Aggregates, following QBE:

- An argument passed by value is `ARGAGG ptr, type`. It marks that the
  memory at `ptr`, with the layout of that type record, is passed by value.
  Its result is only used as a call argument.
- A call that returns an aggregate takes the address to write it to as its
  first argument, and the descriptor says so.
- `PARAM n` is the value of scalar parameter `n`. `PARAMAGG n` is the
  address of the function's own copy of aggregate parameter `n`.

Before code generation, an ABI step for the target rewrites these into
what the convention says: which registers, which stack offsets, which
aggregates go by hidden pointer, which are split across registers. The
type records it reads (size, alignment, fields) are the ones typing
creates. The IL only refers to them by index.

A nested function that captures (D12) takes a pointer to its captured
values as an extra first argument. That's part of its type, so a call to it
through a function value passes it too.

#### The context register

One callee-saved register is reserved in all luv code for a pointer to the
thread's luv context: `r15` on x64 and `x28` on aarch64 (provisional
choices, both callee-saved in every ABI we target). `CTX` is the
instruction whose value is that register. The current arena (D23) is a
field of the context, so finding it is one load, and `with Arena()` and
`into a:` store that field and restore it on the way out.

The register holds the context, not the arena, so that it never changes
while a thread runs luv code. That's what makes C interop work:

- **luv calling C.** The register is callee-saved, so C preserves it.
  Nothing is done at the call.
- **C calling luv**, as the host entering luv code or as a callback through
  a function pointer. C doesn't have the context in that register, and by
  the C convention the luv function has to preserve whatever was there. So
  a luv function that C can reach has a second entry point that saves the
  register, loads the context, and continues into the body, with the
  register restored on return. The context comes from a function in the
  runtime that returns the calling thread's context. The runtime is C and
  is compiled into the host, so thread-local storage is the C compiler's
  job and our code generator never emits it.
- **Which functions get the second entry.** A function whose address is
  taken, or that's exported to the host. Direct calls between luv
  functions use the normal entry and pay nothing.

A hidden context argument on every function was the alternative. It keeps
all registers available, but every luv function then has a signature that
differs from what it appears to be, so every function pointer handed to C
would need a thunk with the same problem of finding the context, and every
luv-to-luv call would pass one more argument. The pinned register costs
one of fifteen usable registers on x64 and one of about twenty-eight on
aarch64.

#### Source positions

A side table of (reference, source offset) pairs, with an entry each time
the front end starts a statement. The SSA pass carries it across. That's
enough for line tables and for reporting where a fault happened, whatever
form debug info takes later (D25). The slot table keeps each named local's
name, so the code generator can report where it lives.

#### The dump

A text form of both stages, one instruction per line, with references
printed as `%n` for instructions and the value itself for constants.
The first use is as test output for expression typing: a test's expected
IL sits beside its expected program output. D30 gives the format, which
`misc/interp.py` reads.

#### What's likely to change

1. The operation list, especially conversions and how operand width is
   expressed for compares.
2. Whether strings and lists (D23) are ordinary memory operations or get
   operations of their own, for bounds checks for example.
3. Whether the SSA pass should rewrite in place, with a forwarding array,
   instead of copying to a second buffer. To be measured.
4. How `check` and other statements with runtime support are expressed.
   Probably as calls into the runtime, as `print` is (D23, "For the
   vertical slice").
5. Overflow and division behaviour. The IL operations wrap, and anything
   stricter would be added operations or explicit checks.
6. The register choices for `CTX`, once the allocators exist.
7. Phi handling in the backward code generation pass, which isn't designed
   yet and may ask for something different from the SSA pass.

### D27 Numeric types and conversions

Decided 2026-11-15. Follows Go: one default integer type, untyped
constants that take their type from context, and no implicit conversions
between numeric types.

#### `int`

`int` is `i64`. It's an alias, not a distinct type, so `int` and `i64` mix
freely. Error messages say `int`, even when the source wrote `i64`.
That's occasionally a little confusing, but `int` is the name nearly all
code will use.

The alternatives considered:

- `int` as `i32`. Smaller arrays and struct fields, twice the SIMD lanes,
  and a match for C's `int`. Rejected because 2³¹ is reachable by byte
  counts, file sizes, sums and millisecond timestamps (24.8 days), and
  because it makes `len`, `sizeof` and indexes a different type from the
  default, which is where most mixed-width code comes from. Where storage
  size matters, `i32` is written explicitly.
- No `int` at all, only sized types. Rejected because `x = 5` still needs a
  default type, which would then be unnamed, and because `int` is easier to
  type and says "the width doesn't matter here". The friction in languages
  without an `int` comes from defaults that disagree (i32 literals and
  `usize` indexes), not from the name.
- Removing `i64` and keeping only `int`. Rejected because `foreign`
  declarations need a 64-bit signed name (below), and because code where
  the width is the point (struct layouts, binary formats) reads better with
  the whole `i8`..`i64`, `u8`..`u64` family.

`len` and indexes are `int`. They're signed, so `len(xs) - 1` on an empty
list is -1, not 2⁶⁴-1. `sizeof`, `alignof` and `offsetof` are untyped
constants (D28), so they can be used as any integer type. (Revised
2026-11-21: `offsetof` was `int`. As a constant, its arithmetic is exact
and can't wrap, and it's used alongside sizes and passed to C as `u64`.)

Each type has one name, apart from `int`, which is there because it's the
default and is easier to type. So there's no `uint` (`u64`), `byte` (`u8`),
or `float` and `double` (`f32` and `f64`). `f16` is removed too until
there's something to do with it. SIMD types like a 4-wide float vector or a
4x4 matrix are more likely to come first.

`codept` stays as it is for now. It's meant to be a code point type that's
distinct from the integers, like Go's `rune`, and is mostly unused until
strings are settled (D23).

There's no `void` type in luv code. A function without a return type
returns nothing, and an untyped pointer is `*u8`. The parser's `TY_VOID` is
only the internal return type of such a function.

#### `foreign`

`foreign` is C's `extern "C"`, where `int` is 32 bits. `int` is an error
in `foreign` signatures, including inside pointer and other compound
types, so a C `int` is always written `i32`. Passing an `i64`
where the callee reads an `int` silently uses the low half, and the upper
half of an `int` result is garbage, so this would be an ABI mismatch with
no error.

There are no foreign-only types. A `foreign` signature uses luv types and
only has to match the C function's ABI, not its C types. `char*`,
`const char*` and `void*` are all `*u8`, and `size_t`, `long` and
`uint64_t` are `u64` or `i64` on LP64 targets. Only width matters, and for
arguments narrower than 32 bits, signedness.

Matching C's types exactly was considered, with foreign-only keywords such
as `c_char` (`const char`) and `voidptr`. It doesn't converge:

- Same-width integer typedefs are distinct C types that differ by platform.
  On macOS `uint64_t` is `unsigned long long` and `size_t` is `unsigned
  long`, and on Linux both are `unsigned long`.
- `const` appears on struct pointers (`const struct timespec*`), at other
  levels (`char *const argv[]`), and inside function pointer types (`qsort`'s
  comparator).

Exact types only matter for a C backend that also includes system headers.
A C backend doesn't need to include them. It can declare only the foreign
functions that are used, from the luv signatures, and then nothing
conflicts. clang and gcc still warn about redeclaring library functions
they know (`memcpy`, `printf`) with different types, which `-fno-builtin`
or `-Wno-incompatible-library-redeclaration` and
`-Wno-builtin-declaration-mismatch` turn off. If that's not enough, a
decorator on a `foreign` declaration could give the C declaration to emit
verbatim. Things that are macros in C (`errno`, `stdin`) need a C shim
either way.

`int` in struct fields isn't checked, so a struct passed to C with an
`int` field is the same silent mismatch. Finding these from the `foreign`
signatures means following fields and pointers transitively, with cycles,
structs declared later and generic instantiations. If it's checked, the
tentative choice is to mark C-compatible structs at their declaration
(`foreign struct S:`), require the mark on structs passed by value in
`foreign` signatures, and reject `int` fields and unmarked struct fields
in marked structs. Every check is then local to one declaration, and the
mark is also where C layout is promised. Nothing is done for now. It may
be left to a lint tool instead. D31 proposes no mark, since every struct
has C layout.

#### Untyped constants

Integer and float constants without a suffix are untyped until context
gives them a type. A declaration by assignment gives `int` for an integer
constant and `f64` for a float constant. The value must fit the type it
gets (`u8 x = 300` and `u32 x = -1` are errors). A float constant is
rounded to an `f32` or `f64` target, and only overflow is an error.

- An untyped integer constant can become a float (`f64 x = 1`, `y * 2` with
  `f32 y`).
- An untyped float constant can't become an integer, even when it's
  integral (`int x = 2.0` is an error).
- A constant takes the other operand's type, and never makes the other
  operand wider. `x + 3000000000` with `i32 x` is a range error, not an
  `int`.
- With untyped float constants, `x * 0.1` with `f32 x` stays `f32`. In C,
  `0.1` is a `double`, so the whole expression is promoted.

#### No implicit conversions

Between different numeric types, nothing converts without `cast`:
integers of different widths or signedness, `f32` and `f64`, and integers
and floats. That applies everywhere: assignment, arguments, `return`,
operands of arithmetic and comparisons, and `+=`.

Two places accept any integer type, because neither changes the type of a
value:

- An index. It's extended to 64 bits and bounds checked as unsigned, so a
  negative index fails the check.
- A shift count.

Lossless widening (`i32` to `int`, `u8` to `int`, `u32` to `int`, `f32` to
`f64`) was considered, either only where a target type is known or for
binary operands too, as in Zig's peer type resolution. With `int` as `i64`,
`len` and indexes as `int`, and adapting constants, little code mixes
widths, and what's left is mostly narrow storage being read into `int`
arithmetic. Not done, for these reasons:

- It hides where arithmetic happens. `int r = a * b` with `i32` operands
  multiplies in `i32` and wraps before widening. That's worse with narrow
  types: `int d = c - 48` with `u8 c` wraps at 256 when `c < 48`. Byte
  processing code, where widening would save the most casts, is where this
  happens most.
- Changing a variable's type to a narrower one is flagged at every use
  that mixes, instead of moving where its arithmetic wraps.
- int to float in particular is never added: `f64 avg = sum / n` with
  integer operands would be integer division followed by a conversion.
- Adding implicit widening later doesn't break any code, and removing it
  would. It's revisited if real code shows the casts are too painful.

The rule doesn't stop `cast int(a * b)` either, which has the same wrap as
the widened version. The error for a narrower value doesn't suggest
casting the operands instead. Errors say what's wrong, not how to fix it.

Comparisons need the same type, so signed and unsigned don't compare
(`compare_signed_unsigned.luv`). Comparing by mathematical value across all
integer types, as Swift does, would be correct and costs one extra
compare for a signed/unsigned pair, but isn't needed while `len` is signed.
Integers and floats never compare.

The IL already has the operations for `cast`: `SEXT32`, `ZEXT32` and
`TRUNC` between integer widths, `ITOF`, `UTOF`, `FTOI` and `FTOU`, and
`FEXT` and `FTRUNC` between float widths.

#### Casts

`cast T(x)` converts between these:

| From | To | Result |
|---|---|---|
| `T` | `T` | No-op, so generic code doesn't need a special case. |
| Any integer type | Any integer type | Truncated, or extended by the signedness of the source. |
| Integer or float | Integer or float | `ITOF`, `UTOF`, `FTOI`, `FTOU`, `FEXT`, `FTRUNC`. |
| Untyped constant | Any numeric type | The constant, which must be in range for `T`. |
| `*T` | `*U` | Same address. |
| `*T` | `i64` or `u64` | The address, and the reverse. |

Everything else is an error, including:

- Pointers to or from integers narrower than 64 bits, which would silently
  truncate an address. A cast through `u64` is explicit about it.
- `bool` to or from numbers. Comparisons give a `bool`, and `x != 0` goes
  the other way.
- Structs, arrays, lists and dicts, since reinterpreting a layout isn't
  what `cast` is for.
- `str` to or from anything, for now. `*u8` from a `str` for foreign calls
  depends on how strings are represented (D23).

A float that's out of range for the integer type, or NaN, gives an
unspecified value, but isn't undefined behaviour. Saturating, as in Rust,
gives the same result on every target, but costs extra instructions on
x86 for conversions that are wrong either way.

A constant out of range for `T` is an error, as in Go, so `cast u8(300)`
and `cast u64(-1)` are both rejected. Untyped constants are conceptually
arbitrarily large, so wrapping one to fit is a category error rather than
a conversion. `~cast u64(0)` or a hex literal gives all ones.

#### Steps

1. `int` as an alias of `i64`, untyped integer constants defaulting to
   `int`, and `describe_type()` printing `int`.
2. `uint`, `byte`, `float`, `double` and `f16` removed from the lexer, the
   tests and the vim syntax file.
3. `int` rejected in `foreign` signatures.
4. Indexes and `len` typed as `int`, any integer type accepted as an index.
5. Floats: `f32` and `f64` types, untyped float constants, and the
   conversion errors above.

Steps 1, 2, 3 and 5 are done, and step 4 only for `sizeof`, `alignof` and
`offsetof`. Remaining:

- `len` typed as `int`. It's one of the builtin names in `lex.c`, and calls
  aren't typed yet. It should get `int` from its declaration once builtins
  are in a prelude and calls have signatures.
- Indexes. Pointers and fixed-size arrays are typed and their indexes are
  checked (2026-11-21, D31 step 1). Lists and dicts are still `ty_unknown`,
  and a dict key isn't an integer, so those indexes can't be checked until
  their container types exist.
- Range checks on constants: `u8 x = 300`, `u32 x = -1`, and overflow of
  an `f32` or `f64` target. These need the constant's value in `ExpDesc`,
  and unary `-` folded, since it currently gives an unknown type.
- Casts are only checked between the types the checker has. Pointers are
  still `ty_unknown`, so their rows in the cast table aren't enforced, and
  constant range checks wait for constant folding.

### D28 Constants and folding

Decided 2026-11-15. Constant expressions are evaluated in `ExpDesc` while
they're parsed, before anything is emitted, so that range errors are found
and so that `[BLAH]int` has a size. The rules follow Go, apart from the
limits on precision.

#### Representation

`ExpDesc` records whether the expression is a constant, and if so its value:
an integer, a `double` or a `bool`. Literal values are already in the token,
so a literal only costs a copy.

Untyped integer constants are `__int128`, behind a `ConstInt` typedef, with
all arithmetic in helpers. Go uses arbitrary precision. 128 bits covers
every value of every integer type, and intermediates such as
`(1 << 64) - 1` that go past 64 bits and come back. A result past 128 bits
is an overflow error at the operator, like a typed one. MSVC has no `__int128`,
so the helpers will need a version over two `u64`s there. Callers don't
change.

Untyped float constants are a `double`, not arbitrary precision.

Typed constants fold at their own width, and overflow is an error at the
operator: `0x4000_0000_0000_0000i64 * 2`. The overflow checks use the
`__builtin_*_overflow` functions in helpers, which will be `#ifdef`ed to
MSVC's equivalents (such as `_mul_overflow_u64`).

#### Untyped constants

An untyped constant has no width, so its arithmetic can't overflow a type.
It's checked when it's converted to one, for an assignment, a declaration,
a `return`, an argument, a cast, or an operand of a typed value, and the
error is at the start of the value:

    x = 0x7fff_ffff_ffff_ffff + 1     # 9223372036854775808 doesn't fit in int.
    u64 HIGH = 1 << 63                # fine

Checking at the operator would mean picking a width, probably `int`, and
then `1 << 63` would be an error even when the target is `u64`.

A float constant is rounded to `f32` or `f64`, and is an error only if it
rounds to infinity, as in Go. An integer constant converted to a float type
has to be exactly representable, so `f32 x = 16777217` is an error. Go
rounds it. Decimal fractions like `0.1` have to be rounded, but an integer
that changes is probably a mistake.

`~` on an untyped constant is `-x - 1`, which is the bitwise complement of a
two's complement number with unlimited width. So `~0` is `-1`, and
`u8 MASK = ~0x0f` is a range error. `0xf0` or `~cast u8(0x0f)` is what's
meant. On a typed constant, `~` flips the bits of its width.

#### Errors

- Overflow and divide by zero are reported at the operator, like the type
  errors for operators.
- A constant zero divisor is an error even when the dividend isn't a
  constant (`x / 0`), for floats as well as integers.
- The minimum signed value divided by `-1` overflows.
- Division truncates toward zero, and `%` has the sign of the dividend.
- A negative shift count, or one at least the width of a typed left
  operand, is an error. `<<` that loses bits of a typed constant overflows.
- Negating a typed unsigned constant other than 0 is an error, and so is
  converting a negative constant to an unsigned type.
- A cast of a constant gives a typed constant, and its value has to be in
  range of the target, including for typed constants (`cast i8(200u8)`). A
  float constant cast to an integer type has to be integral, as in Go. An
  integer constant cast to a float type is rounded, unlike an implicit
  conversion, which has to be exact.

#### Named constants

An ALL_CAPS name is a constant, and its initializer must be a constant
expression. The constant has the type of its initializer, so `BLAH = 3`
is untyped, as in Go's `const BLAH = 3`, and `u8 x = BLAH` works.

That makes `xx = 3` and `XX = 3` different. `xx` is a variable, which has
storage and so needs a type, and gets `int`. `XX` is a value without
storage, which gets its type where it's used. Case already carries meaning
(D4), and with no implicit numeric conversions (D27), untyped constants are
what keeps casts from spreading. `BUF_SIZE = 4096` can be compared with a
`u32` length and used as an `i32` bound.

A constant is typed if its initializer is: `XX = 3i16` or `NEG = -1i64`.
`sizeof` and `alignof` of a type whose layout is known are untyped constants
(changed 2026-11-16, they were `int`), so `sizeof(int) * 100` can be passed
as C's `u64` size without a cast, and `SIZE = sizeof(T)` is untyped. `i16 XX = 3` is the same as `XX = 3i16`. A typed constant
doesn't adapt, so `x = MASK` with `MASK = 0x0fu8` makes `x` a `u8`.

Values of named constants are in a side array, indexed from the `Local` or
global, so variables don't pay for them.

#### Global variables

Decided 2026-11-16. A global variable's initializer must also be a constant
expression, as in C, so that no code runs before `main` and globals don't
need an initialization order. `myvar = somefunc()` at the top level is an
error. C-style constant literals (strings, and struct, list and dict
literals of constants) are planned, but are errors for now, so that emission
and the backends don't have to handle initialized aggregate data yet.
Evaluating calls at compile time is a much later idea.

#### Steps

Each step is driven by the todo tests named.

1. The constant value in `ExpDesc`, set by literals, `true` and `false`.
   Unary `-` and `~` fold untyped and typed constants. On other values they
   check that the operand is a number or an integer, and give its type
   instead of `ty_unknown`. `neg_const_not.luv`. Benched, since it's on the
   path of every literal.
2. Range checks when an untyped constant is converted, everywhere
   `check_assign` is used, and for `f32`. `const_out_of_range.luv`,
   `const_negative_unsigned.luv`, `return_too_big.luv`.
3. Folding `+`, `-` and `*`. The `*_add_overflow`, `*_sub_overflow` and
   `*_mul_overflow` tests, with their messages reworded where the error is
   now a range error.
4. `/` and `%`. The `*_div_zero` and `*_mod_zero` tests.
5. Shifts, and `|`, `&` and `^`, which can't overflow. The
   `*_shl_overflow` tests.
6. Casts of constants, which are range checked (D27), and comparisons,
   `not`, `and` and `or` on constants.
7. Named constants: a constant expression required for an ALL_CAPS
   initializer, the value stored and produced by uses of the name,
   `sizeof` and `alignof` of builtin types as constants, and the size of
   `[BLAH]int`. `const_invalid_function_call.luv` and a rewritten
   `const_no_cast_eval_yet.luv`. Forward references to constants wait for
   the D11 skim.

Strings as constants, and `sizeof` of structs, which needs their layout,
come later.

---

### D29 Tests as ninja targets (deferred)

`m <config> [targets...]` builds with ninja into `out/<platform><config>/`
(e.g. `out/wd`, `out/lr`), from a `build.ninja` written by
`misc/gen_ninja.py`. Ninja only builds binaries. `m` handles `test` and test
names itself, and passes anything else on to ninja.

Instead, the generator could write the runs as ninja targets too, so that `m`
passes every argument through and `ninja -f out/mr/build.ninja test` works
without `m`:

- A `test` phony that depends on an edge per test, and a phony per test named
  after its file (e.g. `add_int`), so that a single test is just a target.
- One generic rule that runs the compiler on one input and compares against
  its `CRET`/`CERR` header. This is only worth doing once there's a single
  compiler to run on every input. Until then, parsecheck, lexdump and
  unittest each need a separate kind of edge.
- Ninja can't glob, so `m` reruns the generator at the start of every run to
  pick up added or renamed tests. Writing the file only when its contents
  change keeps this from triggering ninja's regeneration.
- Test edges have no outputs or stamp files, so they always run. Running the
  same tests repeatedly is useful for profiling and for finding flaky tests.
  A test's result also depends on the modules it imports, which a stamp
  wouldn't track.
- Test edges use `pool = console` so their output isn't buffered, at the
  cost of ninja's own status and `FAILED:` lines in between.

### D30 Emission for the vertical slice (proposal)

Written 2026-11-16, for review before any code. It covers how the parser
produces D26's raw IL for the slice: int, bool and str literals, locals,
arithmetic and comparisons, `if`, `for` over `range`, `print`, functions
with parameters, calls and `return`. D26 is the IL itself. This is the
interface between `ExpDesc` and the IL buffer, in the style of Lua's
`lcode.c`.

Anything to do with the SSA pass is ignored for the slice: predecessor
lists, each loop's stored slots, block order invariants. Those are added
when the SSA pass is written.

#### The driver and the two builds

- `parsecheck` becomes `luv`, with subcommands: `luv parse` (parsecheck's
  current job), `luv il` (prints the dump) and later `luv run`.
- `parser.c` is compiled twice, from two small files that define `EMIT` as
  0 or 1 and then include it. Each subcommand calls the matching copy, so
  choosing between checking and emitting costs nothing while parsing. The
  entry point's name comes from a macro, so that both copies can be linked
  into one binary.
- In the parser, emission is written as `if (EMIT) { ... }` rather than
  `#if`, so both builds compile and type check every line, and the checking
  build's optimizer removes it. The `exp_*` helpers return a value that's
  unused in the checking build, such as a `Ref` of 0.
- `luv parse` stays the parser benchmark. Emission's cost can't be
  measured on dumbbench until enough of the language emits, so a
  benchmark input restricted to the slice is generated when emission is far
  enough along to matter.

#### What runs

- The slice is run by `misc/interp.py`, a Python interpreter that reads
  `luv il` output. It's scaffolding for developing emission. D25's "no
  interpreter" still stands, and making it a separate script makes that
  obvious. Python is used because it reads the text dump easily and the
  build script already requires it.
- The runtime's print functions (D23) are written in the script for now.
  The C runtime is written when there's native code to link it into.
- The dump is the interface between the two, so it has to be easy to read
  back. It's also what's used to diagnose the front end later, so it has to
  be easy for a person to read too. See "The dump format".
- The test runner pipes `luv il` into the interpreter for each test whose
  functions all emit, and checks `# OUT:` and `# RET:`. The number of tests
  that pass is the slice's progress.

#### Functions and the sink

- A `FuncState` per function being parsed holds the instruction buffer,
  constants, slot table and block table. Parsing a nested `def` uses the
  next `FuncState`, so the buffers are kept per nesting depth and reused.
- When a function ends, it's passed to a callback on `Ctx`, as files are
  loaded through `load_file`. For `luv il` the callback prints it, and the
  buffers are reset.
- In the emitting build, a construct that the slice can't emit yet is an
  error, "X isn't supported yet.", at the construct. The checking build
  doesn't emit at all, so every test still parses and type checks there.
- Each function gets a symbol, and calls refer to it with a `KSYM`
  constant. Symbols are in a table on `Ctx` because modules share it. Calls
  are define-before-use until the D11 skim exists, so a symbol always
  exists before a call refers to it. The symbol is found through a
  separate table indexed from `GlobalInfo`, like `global_consts`, rather
  than from the function's `TypeInfo`, because function `TypeId`s will be
  interned once there are function pointers.

#### ExpDesc

`ExpDesc` gets a kind that says where its value is, replacing `is_const`:

| kind | value | info |
|---|---|---|
| `EK_CONST` | in `v`, as now | |
| `EK_LOCAL` | in a slot, not loaded yet | slot index |
| `EK_REF` | an instruction's or constant's result | `Ref` |
| `EK_STR` | a string literal | the literal's index in a table of string data |
| `EK_VOID` | nothing, the result of a void call | |

Delaying the load of `EK_LOCAL` lets `x = y` be a `LOAD` and a `STORE`,
and lets `&x` use the slot's address without a load. `EK_STR` stays
unemitted until it's used, because it becomes two values.

Emission helpers are in `parser.c` next to `ExpDesc`, named `emit_*`:

- `emit_to_ref(p, e, t)`: emits whatever is needed to make `e` one value of
  type `t`, and returns its `Ref`. Untyped constants get their type from
  `t`, which has already been checked by `check_assign` or
  `type_for_arith`.
- `emit_to_str_refs(p, e, refs)`: the two values (data and len) of a `str`.
- `emit_store_local(p, local, e)`: an assignment or declaration.
- `emit_infix(p, e)`: called after a binary operator is read and before
  its right operand is parsed. A non-constant left operand is turned into a
  `Ref`, so that it's evaluated before the right operand. A constant is
  kept so that folding still works.

Everything that writes to the buffer is in a separate `il.c`, named
`il_*`: appending an instruction, interning a constant, adding a slot,
blocks and jumps, and the dump.

Mapping types: `bool` and integers up to 32 bits are `i32`, `i64`, `u64`
and pointers are `i64`, floats are themselves. Narrow arithmetic results
are extended immediately after each operation (`SEXT8` after an `i8`
`ADD`). That's more instructions than "where the narrow result is
observed", but it's one rule and the SSA pass's folding removes the extra
ones later.

#### Locals and parameters

- A local is a slot. `Local` gets the slot index, set when it's declared.
  Locals are function scoped (D6), so a slot is never reused within a
  function.
- A parameter is a value, not a slot (D26, "Slots"). `PARAM n` is emitted
  at the start of the entry block, and the parameter's `Local` holds its
  `Ref`. Reading the parameter gives an `EK_REF`.
- A `str` local is two slots, and a `str` parameter is two `PARAM`s, until
  structs exist (D23, "For the vertical slice").

#### Blocks and jumps

A block number is handed out when something first needs to refer to it,
and the block is started later. The block table holds where each one starts.
Blocks are then not numbered in source order, which only matters to the SSA
pass, and can be fixed there or by switching to Lua-style pending jump
lists when it's written.

- `il_block_declare(f)` returns a block number that isn't started yet.
- `il_block_start(f, b)` starts block `b` at the current position.
- `il_jump(f, b)` and `il_branch(f, cond, b)` are `JMP` and `BR`. `BR`
  ends its block, so `il_branch` also starts the fall-through block.

Statements:

- `if c: A elif d: B else: C`: a new block for the end, a new block for
  each next branch, `BR` to the next branch when the condition is false,
  and a `JMP` to the end after each suite unless it returns.
- `for i in range(a, b)` (and `range(b)`): a call to the builtin `range`
  as the whole loop expression is recognized and emitted as a counting
  loop. `b` is stored to a hidden slot so that it's evaluated once.

  ```
  STORE i, a
  BLOCK header
  BR (LT (LOAD i) (LOAD end)), exit
  BLOCK body
  ...                 (continue jumps to step)
  BLOCK step
  STORE i, ADD (LOAD i) 1
  JMP header
  BLOCK exit          (break jumps here)
  ```

  Any other loop expression isn't supported yet.
- A loop stack in `FuncState` holds each loop's step and exit blocks for
  `continue` and `break`. As planned with `Flow`, `break` and `continue`
  become `FLOW_JUMPS` and have to end their block like `return`, so that
  nothing is emitted into an unreachable block.
- `return` is `RET` or `RET v`. A void function that falls off the end gets
  a `RET`.
- `and` and `or` store each operand to a temporary slot on its own path and
  load it after the join (D26). A condition is then a bool value and a
  `BR`. Lua instead keeps a condition's jumps unresolved in two lists, one
  for where to go when it's true and one for false, so `if a and b:` is two
  branches straight to the else block without making a bool. That can come
  later, if the generated code shows it's worth it.

#### Recovering branches for `and` and `or` (for the SSA pass)

Written 2026-11-19, to read when the SSA pass is written. The jump lists
aren't the only way to get the better branches back. The SSA pass can
recover them with a local rewrite, jump threading, without changing the
parser.

After the temporary slot is promoted, `if a and b:` is:

```
:0  BR a, :J              (a is false: the phi gets 0)
:B  %b = ...              (continues into :J)
:J  %t = PHI [:0: 0], [:B: %b]
    BR %t, :else
:then ...
```

The rewrite applies to a block that holds only a phi and a `BR` of it, when
nothing else uses the phi. For each predecessor whose incoming value is a
constant, where the `BR` goes is already known, so that predecessor's edge
is changed to go there instead. Here `BR a, :J` becomes `BR a, :else`.
Then `:B` is the only predecessor left, the phi is just `%b`, and `:J` is
merged into `:B` as `BR %b, :else`. That's the same code the jump lists
give: `BR a, :else` and `BR b, :else`.

- It needs predecessor lists and phis, which the SSA pass builds anyway,
  and a way to change a branch's target in place, which is one operand.
- Every edge that's changed goes to a later block, so the D26 invariants
  (source order, predecessors before their block) still hold.
- The shape can be recognized when the forward walk reaches `:J`, so it
  doesn't need to be a pass of its own.
- For `or`, the constant edge is the true path, an empty block with
  `JMP :J`, and the phi gets 1. Its `JMP` is changed to go to `:then`. A
  chain like `a and b and c` is threaded one join at a time.
- When the value is used as a value (`x = a and b`), the phi has another
  use, so nothing changes and the bool is kept.
- A `BR` of a compare still becomes a compare and a conditional jump in the
  encoder, as it would have.
- `not` is the awkward case. `if not (a and b)` is a `BR` of `XOR %t, 1`.
  Removing the `XOR` means swapping the true and false targets, but `BR`
  only branches on zero and otherwise continues into the next block, so a
  swap means reordering blocks. A flag on `BR` that inverts the test is
  probably simplest, since the encoder chooses the jump's condition anyway.

#### Calls and print

- A call evaluates its arguments to refs left to right, then appends `CALL`
  and its `EXT` slots. The refs are kept in a fixed array on the C stack
  while the arguments are parsed, with a limit on the argument count. A
  `str` argument is two refs.
- `print` uses D23's runtime calls, one per kind, then `luv_print_end`. An
  interpolated string in `print` is one call per piece. The runtime
  functions are symbols too, created when the `Ctx` is set up.
- String literal bytes go into a table of data on `Ctx`. A constant of
  type `IT_DATA` refers to the bytes by their index in the table, and the
  length is a `KINT`. Like a `KSYM`, it's an address, and it becomes a
  relocation. It isn't a symbol only so that data doesn't need an interned
  name.

#### The dump format

A fixed line grammar, close to QBE's and LLVM's text forms. For example:

```
def int add(int a, int b):
    return a + b

def int main():
    int x = add(1, 2)
    if x > 2:
        print "big \(x)"
    return 0
```

```
data @.dat0 "big "

func i64 @add
:0
  %1 = PARAM i64 0
  %2 = PARAM i64 1
  ;     return a + b
  %3 = ADD i64 %1, %2
  RET i64 %3

func i64 @main
  slot $0 i64 x
:0
  ;     int x = add(1, 2)
  %1 = SLOT $0
  %2 = CALL i64 @add, 1, 2
  STORE i64 %2, %1
  ;     if x > 2:
  %5 = SLOT $0
  %6 = LOAD i64 %5
  %7 = GT i64 %6, 2
  BR %7, :2
:3
  ;         print "big \(x)"
  CALL @luv_print_str, @.dat0, 4
  ...
```

- Top level lines are `data` and `func`. Inside a function, `slot` lines
  come first, then a label line `:n` for each block, followed by its
  instructions.
- A `data` line is `data @.datN [align N] "bytes"`. The alignment is only
  printed when it's more than 1, as for a struct's default image.
- An instruction is `[%n =] OP [type] operand, operand, ...`. The type is
  left out when it's `void`, and for `SLOT`, which is always an `i64`
  address. Types are a fixed set of words that no operand can look like.
- A compare's type is its operands' type, which the unsigned compares need
  to know the width. Its result is always an `i32` 0 or 1.
- The first character of an operand says what it is: `%n` an instruction,
  `@name` or `@name+8` a `KSYM`, `:n` a block, `$n` a slot. A `KINT` starts
  with a digit or `-`, and is printed as signed decimal and read at the
  instruction's type. A `KFLT` is a hex float (`0x1.8p+0`), which is
  exact and is read by Python's `float.fromhex`.
- `%n` is the instruction's index in the buffer, so there are gaps where
  `BLOCK` and `EXT` are.
- A call's arguments are all on its line, wherever they're stored. The
  `EXT` slots aren't printed.
- A comment is a whole line starting with `;`, so the reader can skip
  them without looking inside other lines. Comments give the text of each statement's source line, with its indentation, from
  the source position table (D26), and above an instruction with float
  constants, their values in decimal (`; 0x1.8p+0 = 1.5`). Line numbers
  aren't printed, because the lexer only keeps offsets, and finding a line
  number means counting newlines from the start of the file.
- In `data @name "bytes"`, printable ASCII is written as itself except for
  `"` and `\`. Everything else is `\n`, `\t`, `\\`, `\"` or `\xNN`.
- Symbol names can't contain whitespace. Data is `@.datN`, which
  can't collide with a function's name because identifiers can't start
  with `.`.

#### Out of the slice

Globals, `mod.func` calls, nested functions, structs, lists, dicts,
comprehensions, `with`, `check`, `cast` of non-constants beyond numeric
conversions, pointers, `foreign` calls, and `str` results from functions.
Comprehensions need extra care later, since D10's rewind re-lexes the
element expression.

#### Steps

Each step is split into pieces small enough to review, driven by the tests
that start to pass.

1. `il.c`: the buffer, constants, slots, blocks and jumps, the source
   position table, and the dump, tested from `unittest.c`. No parser
   change.
2. `ExpDesc.kind` replaces `is_const`. No change in behaviour.
3. `parsecheck` becomes `luv parse`, with `parser.c` built twice. Then
   `FuncState`, the callback and `luv il`: `def int main(): return 0`.
4. The Python interpreter and the test runner running supported tests:
   `basic_int.luv`-style `# RET:` tests.
5. Locals, assignment, arithmetic and comparisons.
6. `print` and string literals.
7. `if`/`elif`/`else`, `and`, `or`, `not`.
8. `for` over `range`, `break` and `continue`, `FLOW_JUMPS`.
9. Parameters and calls.

#### Decided in review (2026-11-16)

- The driver is `luv`, and `luv parse` replaces parsecheck, but only if it
  costs nothing in parse speed, hence the two builds.
- The interpreter is a Python script reading the dump.
- Parameters (and later captured values) are values, not slots.
- Unsupported constructs are errors in the emitting build.
- SSA concerns are ignored for the slice, and blocks are numbered in
  whichever way is easiest for the front end.
- A function's symbol is in a separate table, not its `TypeInfo`.
- The interpreter is `misc/interp.py`.

#### Still open

1. A better name than `FuncState`, if one comes up.

### D31 Aggregates (proposal)

Written 2026-11-20, for review before any code. It covers structs and
fixed-size arrays as values: their layout, what each target ABI needs to
know about them, how they appear in D26's IL, how they can still end up in
registers, and how the parser emits them. `str` is the first consumer.
`range` needs an iteration protocol, and so method calls, first, and both
are out of scope here.

#### What exists now

Struct types aren't typed yet. There's no `TypeInfo` for a struct, fields
aren't recorded, and `sizeof` of a struct takes the non-constant path. The
first step is struct typing in the checking build: fields and their types,
field access, struct literals checked against fields, and the layout below,
which makes `sizeof` and `alignof` of every struct an untyped constant
(D28).

#### Layout

Every struct uses C's layout. Fields are in declaration order, each at the
next multiple of its own alignment. A struct's alignment is the largest
alignment of its fields, and its size is rounded up to that alignment.
Fields are never reordered.

- Scalars are naturally aligned. `bool` is 1 byte and pointers are 8.
- `[N]T` is `N * sizeof(T)` bytes with `T`'s alignment.
- `str` is `{*u8 data; int len}`: 16 bytes, aligned to 8.
- A field of struct type needs the full definition (D11), and a struct
  that contains itself by value is an error at its definition.
- A struct with no fields is an error (open question 3).
- Decided (2026-11-20): field defaults (`int x = 5` in a struct body) must
  be constant expressions, like global initializers (D28). A struct with
  defaults then has a constant image, which goes in the data table, and
  creating one is a copy of that image instead of a `ZERO`. A struct
  without defaults is zeroed.
- Each generic instantiation (D21) gets its own layout, computed when it's
  instantiated.
- Nothing in luv is aligned to more than 8, and there's no syntax to ask
  for more. There are no packed structs, bitfields or unions for now
  (unions and packed structs are likely later, see "The summary record").

Since every struct already has C's layout, a struct needs no mark to be
passed to C. This replaces the tentative `foreign struct` from D26. That
mark's other job, rejecting `int` fields in structs passed to C, is left to
a lint.

#### What each ABI needs

The three targets are Win64, SysV x64 (Linux) and AAPCS64 with Apple's
changes (macOS). Each section gives the rules for aggregates, then the
register and stack rules they interact with. Everything marked "checked"
was confirmed against clang 18's output for that target, either the LLVM IR
signature or the generated assembly. The rules were also compared with each
ABI's own document (2026-11-20): Microsoft's "x64 calling convention" and
"Stack usage" pages, the SysV psABI's "Parameter Passing" section
(gitlab.com/x86-psABIs, master), AAPCS64 release 2026Q3 (stages B and C,
"Result return"), and Apple's "Writing ARM64 code for Apple platforms".
Rule numbers like C.12 are AAPCS64's. Where a document and clang differ, or
a document doesn't say, the text says so.

##### Win64

The first four arguments are passed by position. Argument 1 goes in `rcx`
or `xmm0`, argument 2 in `rdx` or `xmm1`, argument 3 in `r8` or `xmm2`,
and argument 4 in `r9` or `xmm3`, depending on whether it's an integer or a
float. Each position uses one of its pair and the other is unused, so
integer and float arguments aren't counted separately as on SysV and arm64
(checked: in `f(int, double, int, double, int, double)`, the arguments
went in `ecx`, `xmm1`, `r8d`, `xmm3`, then the stack). The fifth and later
arguments take 8-byte stack slots each, narrow integers included
(checked).

- The caller always reserves 32 bytes of shadow space just above the
  return address, for the callee to spill the four register arguments
  into. Stack arguments start after it, at `rsp + 32` at the call
  (checked). `rsp` is 16-byte aligned at the call.
- Integer results are in `rax`, and float results in `xmm0`.
- `rbx rbp rdi rsi r12-r15` and `xmm6-xmm15` are callee-saved (checked:
  a caller spilled `xmm6` around its calls). Unlike SysV, `rdi`, `rsi` and
  the upper `xmm` registers are callee-saved.
- There's no red zone. Microsoft's documentation doesn't use the term,
  but says all memory below `rsp` is volatile and may be overwritten by
  the OS, a debugger or an interrupt handler.
- In a variadic call, a float argument in the first four positions is
  passed in both the `xmm` register and the matching GPR, so that the
  callee can spill all four GPRs and walk the arguments in memory
  (checked: `2.5` as argument 2 was in both `xmm1` and `rdx`).

Aggregates:

1. An aggregate of size 1, 2, 4 or 8 is passed as an integer of that size,
   in the position's GPR or in a stack slot. This applies even when every
   field is a float (checked: `{f32, f32}` was in `r8` and `{f64}` in `r9`,
   not in `xmm` registers).
2. Any other aggregate, including sizes 3, 5, 6 and 7, is copied by the
   caller, and a pointer to the copy is passed in the position's GPR or
   stack slot (checked: `{u8, u8, u8}` and `str` were passed as pointers).
   The callee may modify the copy, and the caller can't assume the copy is
   unchanged after the call. That's how compilers treat it, but
   Microsoft's documentation doesn't say it either way. The documentation
   says the copy "must be 16-byte aligned". clang aligns it only to the
   type's alignment (checked: a `{u8, u8, u8}` copy was at offset 60), so
   aligning it to 16 satisfies both.
3. A result of size 1, 2, 4 or 8 is returned in `rax` as an integer, even
   when it's all floats (checked: `{f64}` came back in `rax`, where a plain
   `f64` comes back in `xmm0`). The documentation adds C++ conditions
   (roughly, a C++03 POD) that every C or luv struct meets. It doesn't
   mention all-float structs, which fall under the same rule.
4. Any other result is written through a hidden pointer that the caller
   passes as argument 1, in `rcx`, which moves every other argument one
   position along. The callee returns the same pointer in `rax` (checked).

There's no register exhaustion rule to speak of. Each argument has a fixed
position, so an aggregate either fits in one GPR or is a pointer, and in
both cases it takes exactly one position. The only thing the classifier
reads is the size.

##### SysV x64

Integer arguments go in `rdi rsi rdx rcx r8 r9`, and float arguments go in
`xmm0` to `xmm7`. The two are counted separately, so a function taking an
int and a double uses `rdi` and `xmm0`. There's no shadow space. Stack
arguments take 8-byte slots each, including narrow integers (checked), in
order of increasing address. Integer results are in `rax` then `rdx`, and
float results in `xmm0` then `xmm1`. `rbx rbp r12-r15` are callee-saved,
and all `xmm` registers are caller-saved. Leaf functions may use 128 bytes
below `rsp` (the red zone). In a variadic call, `al` holds an upper bound
on the number of vector registers used.

Aggregates are classified by eightbytes, the 8-byte chunks of the
aggregate at offsets 0, 8, and so on:

1. An aggregate larger than 16 bytes is class MEMORY. The psABI's base
   rule says larger than 64 bytes, and a later rule sends anything over 16
   bytes to memory unless it's a vector type (`__m256`, `__m512`), which
   luv doesn't have. An aggregate with unaligned fields is MEMORY too,
   which can't happen in luv. An empty struct is NO_CLASS, and the psABI
   doesn't say how it's passed. QBE treats size 0 as MEMORY. GCC and clang
   ignore empty structs in C.
2. Otherwise each eightbyte gets a class from the fields that overlap it.
   If any such field is an integer or pointer, it's INTEGER. If all of them
   are floats, it's SSE. Padding doesn't count. With natural alignment no
   field crosses an eightbyte boundary, so each field is in exactly one
   eightbyte. Array elements count as fields.
3. An INTEGER eightbyte is passed in the next GPR. An SSE eightbyte is
   passed in the low 8 bytes of the next `xmm` register, so two `f32`s in
   the same eightbyte share one register (checked: `{f32, f32}` is one
   `<2 x float>` in `xmm0`).
4. If there aren't enough registers left for every eightbyte of an
   argument, the whole argument goes on the stack. The registers it would
   have used stay free for later arguments. The psABI says it as "If there
   are no registers available for any eightbyte of an argument, the whole
   argument is passed on the stack. If registers have already been
   assigned for some eightbytes of such an argument, the assignments get
   reverted." (checked: after five integer
   arguments, a `{i64, i64}` went to the stack and the next `i64` took
   `r9`. The same happened with `xmm7` and `{f64, f64}`).
5. A MEMORY argument is copied by the caller into the outgoing argument
   area on the stack, rounded up to 8 bytes, "at an address respecting the
   arguments alignment". It isn't a pointer to a copy as on Win64. The callee finds it at a fixed offset from its incoming
   `rsp` (checked). It uses no registers.
6. A MEMORY result is written through a hidden pointer that the caller
   passes as the first integer argument, in `rdi`. The callee returns the
   same pointer in `rax` (checked). Other results use the classes above
   with `rax`/`rdx` and `xmm0`/`xmm1`, so `{f64, i64}` comes back in `xmm0`
   and `rax`.

An eightbyte that's only partly filled (the last 4 bytes of a 12-byte
struct) is passed in the low bytes of the register. The rest of the
register is undefined, and the load that fills it must not read past the
end of the object. clang loads such pieces as `i32` or `i24`.

What the classifier reads: the size, and for aggregates of 16 bytes or
less, whether each of the two eightbytes is all floats.

##### AAPCS64 and Apple arm64

Integer arguments go in `x0` to `x7`, and float arguments go in `v0` to
`v7` (`s` for `f32`, `d` for `f64`). The two are counted separately, as on
SysV. Results use the same registers as the first argument would: `x0`/
`x1`, or `v0` to `v3`. `x8` is the indirect result register. `x19-x29`
and the low 64 bits of `v8-v15` are callee-saved. `x18` is the platform
register, reserved on Apple. `x29` is the frame pointer, which Apple
requires to always address a valid frame record, and `x30` is the link
register. AAPCS64 has no red zone, but Apple has one of 128 bytes.

An HFA (homogeneous floating-point aggregate) is an aggregate whose fields,
with nested structs and arrays flattened, are all `f32` or all `f64`, and
of which there are 1 to 4.

1. An HFA is passed one field per `v` register, without packing (checked:
   `{f32, f32}` is `s0` and `s1`, where SysV packs it into `xmm0`). This
   includes 32-byte HFAs like `{f64, f64, f64, f64}`, in `d0` to `d3`
   (checked). If there aren't enough `v` registers left, the HFA goes on
   the stack and every remaining `v` register is marked used, so later
   float arguments go on the stack too (checked: after seven `f64`s, a
   `{f64, f64}` and the next `f64` both went on the stack and `d7` was
   unused). The GPRs aren't affected. These are C.2, C.3 and C.6. AAPCS64
   adds that an HFA that isn't in registers is "always passed on the stack
   (never in general-purpose registers)".
2. Any other aggregate larger than 16 bytes is copied by the caller and
   replaced with a pointer to the copy, which then counts as an integer
   argument (checked, B.4). Like Win64, but the cutoff is 16 bytes and the
   size doesn't have to be a power of two.
3. Any other aggregate is rounded up to 8 or 16 bytes and passed in one or
   two consecutive `x` registers as its memory image, first 8 bytes in the
   first register. Floats in a non-HFA go in `x` registers (checked:
   `{f64, i64}` and `{f32, f32, f64}` are both `x0`, `x1`). If there aren't
   enough `x` registers left, it goes on the stack and every remaining `x`
   register is marked used (checked: after seven `i64`s, a `{i64, i64}` and
   the next `i64` both went on the stack and `x7` was unused). These are
   B.5, C.12 and C.13.
4. An aggregate is never split between registers and the stack. On the
   stack it's at an 8-byte aligned address (C.14), and it takes its size
   rounded up to 8 (B.5).
5. A result that isn't an HFA and is larger than 16 bytes is written
   through a pointer that the caller passes in `x8`. `x0` stays the first
   argument, and the callee doesn't return the pointer (checked).
   AAPCS64 says "there is no requirement for the callee to preserve the
   value stored in x8".
6. An aggregate aligned to 16 starts at an even-numbered register (C.10),
   except on Apple, which allows an odd one. luv never has one.

Apple's changes to AAPCS64 that matter here:

- Scalar stack arguments take their natural size and alignment instead of
  8-byte slots (checked: `char, short, char, int` were at offsets 0, 2, 4
  and 8). Apple says "Function arguments may consume slots on the stack
  that are not multiples of 8 bytes", which drops C.16's widening to 8.
  B.5 and C.14 still apply to aggregates, so an aggregate on the stack
  still takes an 8-byte aligned slot rounded up to 8 bytes (checked: a
  `{u8, u8}` between two `char`s was at offset 8). Apple doesn't spell
  this out.
- Variadic arguments always go on the stack, in 8-byte slots, never in
  registers.
- The caller extends integer arguments narrower than 32 bits, where
  AAPCS64 has the callee do it (below).
- "Functions may ignore parameters that contain empty struct types."
  AAPCS64 itself has no empty structs ("all members of a Composite Type
  have nonzero size").

What the classifier reads: the size, and whether it's an HFA of `f32` or of
`f64`. The number of fields is the size divided by the width.

##### Narrow integer arguments and results

Not about aggregates, but it's the same ABI step. The ABIs disagree about
who extends `i8`, `u8`, `i16`, `u16` and `bool` to 32 bits (checked, from
what clang's callers and callees assume):

| target | arguments | results |
|---|---|---|
| Win64 | callee extends | caller extends |
| SysV x64 | callee extends by the psABI. clang before 2026-11 relied on the caller having done it (open question 6) | caller extends |
| AAPCS64 (Linux) | callee extends | caller extends |
| Apple arm64 | caller extends | callee extends (clang only) |

What the documents say:

- Win64: "the callee can ignore the upper bits of the register", and "The
  state of unused bits in the value returned in RAX or XMM0 is undefined".
- SysV: bits beyond the type's size "are unspecified", and "the consumer
  side of those values needs to extend them". clang's callees assume the
  caller extended arguments to 32 bits anyway (checked), and clang and
  GCC both extend when calling. That's a known de facto rule that the
  psABI doesn't state.
- AAPCS64: "named integral values must be narrowed by the callee rather
  than the caller", and unused bits of a register are unspecified.
- Apple: "The caller of a function is responsible for signing or
  zero-extending any argument with fewer than 32 bits." Apple's page says
  nothing about results. clang's callees extend them (checked), so a luv
  caller shouldn't rely on it without a better source.

The IL keeps narrow integers extended in an `i32` (D30), so luv always
passes and returns extended values. A narrow parameter or result coming
from C needs a `SEXT`/`ZEXT` except where the table says the other side
did it.

##### The checked cases

Arguments, as clang lowered them. Results use the same registers, except
that the "memory" cases become a hidden result pointer.

| fields | size | SysV x64 | Apple arm64 | Win64 |
|---|---|---|---|---|
| `str` (`*u8, i64`) | 16 | `rdi`, `rsi` | `x0`, `x1` | pointer to copy |
| `f32, f32` | 8 | `xmm0` (packed) | `s0`, `s1` | one GPR |
| `f32, f32, f32` | 12 | `xmm0` (packed), `xmm1` | `s0`-`s2` | pointer to copy |
| `f64, f64` | 16 | `xmm0`, `xmm1` | `d0`, `d1` | pointer to copy |
| `f64` x 4 | 32 | memory | `d0`-`d3` | pointer to copy |
| `[4]f32` | 16 | `xmm0`, `xmm1` (packed) | `s0`-`s3` | pointer to copy |
| `f32, i32` | 8 | one GPR | one GPR | one GPR |
| `f64, i64` | 16 | `xmm0`, `rdi` | `x0`, `x1` | pointer to copy |
| `f32, f32, f64` | 16 | `xmm0` (packed), `xmm1` | `x0`, `x1` | pointer to copy |
| `f64, f32` | 16 | `xmm0`, `xmm1` | `x0`, `x1` | pointer to copy |
| `i32, i32, i32` | 12 | `rdi`, `rsi` (low 4 bytes) | `x0`, `x1` | pointer to copy |
| `u8, u8, u8` | 3 | one GPR (low 3 bytes) | one GPR | pointer to copy |
| `i16, u8` | 4 | one GPR | one GPR | one GPR |
| `i64` x 3 | 24 | memory | pointer to copy | pointer to copy |

#### The summary record

The three classifiers together need less than QBE's type records hold.
QBE keeps every field because its text IL is all that it sees. Here the
fields are known to typing, so the summary can be computed once, when the
struct's definition ends:

```c
typedef struct AggInfo {
  u32 size;
  u8 align;  // log2
  u8 abi;    // AGG_SSE0, AGG_SSE1, AGG_HFA_F32, AGG_HFA_F64
} AggInfo;
```

- `AGG_SSE0` and `AGG_SSE1` are set for an aggregate of 16 bytes or less
  when every field overlapping that eightbyte is a float, and there's at
  least one field in it.
- `AGG_HFA_F32` or `AGG_HFA_F64` is set when every field (flattened) is
  that float type and there are 1 to 4 of them.
- Win64 reads only `size`, SysV reads `size` and the SSE bits, and arm64
  reads `size` and the HFA bits.

QBE's `isdark`, its union handling, and the SysV classes for `long double`
and vectors (X87, SSEUP) aren't needed, since luv has no packed structs,
unions, `long double` or vector types.

Implemented (2026-11-22) as `il_agg_add` in il.c, which classifies from the
struct's scalars flattened into `AggLeaf {offset, IlType}`. A struct's
`TypeInfo` holds an `AggId`, which replaced its own size and alignment.
Leaves are only collected for structs of up to 32 bytes.

Win arm64 and Linux AAPCS64 need nothing more. Win arm64 differs only in
variadic calls, where floats and HFAs go in GPRs. RISC-V's LP64D (and
LoongArch's) would need a different kind of summary. An aggregate of at most
16 bytes that flattens to one float, two floats, or one float and one
integer is passed field by field in FPRs, or an FPR and a GPR (`{f32, i32}`
is `fa0` and `a0`). That needs each of up to two leaves' class, width and
offset. Since `il_agg_add` already gets the leaves, that's a local change.

C-style unsafe unions and an equivalent of `__attribute__((packed))` are
likely to be wanted eventually, though not soon. The summary record would
need to grow for them:

- Unions: the SysV bits are computed by merging every member's fields into
  each eightbyte, so an eightbyte is SSE only if every member's fields
  there are floats. A union is an HFA only if every member is an HFA of the
  same base type. Win64 still reads only the size.
- Packed structs: a field that isn't at its natural alignment makes the
  whole aggregate MEMORY on SysV, which needs a bit like QBE's `isdark`.
  AAPCS64 still classifies by size and HFA, and Win64 by size. Field loads
  and stores of a packed struct also need unaligned access, which the IL's
  `LOAD`/`STORE` would have to mark.

`[N]T` passed by value between luv functions is classified as a struct of
N `T` fields. C can't pass arrays by value, so `[N]T` in a `foreign`
signature is an error.

#### Aggregates in the IL

This refines D26 ("Aggregates are memory").

- A module's `AggInfo`s are in a table on `Ctx`, and the IL refers to them
  by index. A struct's `TypeInfo` holds its index.
- There's no aggregate value type. An aggregate only appears as an address,
  an `i64`.
- `IlSlot` gets a size and alignment, or an `AggInfo` index, so that a
  slot can hold an aggregate.
- A field is `ADD addr, KINT offset`, then `LOAD` or `STORE` of the field's
  type. Nested fields (`a.b.c`) add their offsets in the front end, so
  they're still one `ADD`.
- `ZERO dst, agg` has the `AggInfo` index as its second operand, and
  `COPY dst, src` has it in an `EXT`, since both of its operands are
  addresses. The index gives the alignment as well as the size, so the
  code generator can choose wide moves. The size can't come from the
  instruction's type byte, which only holds an `IlType`, or from the
  destination slot, since the destination can be a field (`a.inner = b`)
  or a pointer (`*p = s`). `COPY` is much rarer than scalar operations,
  so the `EXT` costs little.
- `ARGAGG addr, agg` is an aggregate argument. `PARAMAGG n, agg` is the
  address of aggregate parameter `n`, which is the callee's own copy.
  Aggregate parameters are immutable like other parameters, so their
  fields and elements can't be assigned or have their address taken.
- Decided (2026-11-23): a call that returns an aggregate has
  `RESULTAGG dst, agg` as its first argument, where `dst` is a new
  temporary slot in the caller, and the call's value is that slot. A
  function that returns an aggregate is `func #N @name` (`IlFunc.ret` is
  `IT_AGG`, with `ret_agg`), and returns with `RET addr`, the address of
  the value. The ABI step turns `RESULTAGG` into the hidden pointer, or
  into stores of the result registers after the call, and `RET addr` into
  a copy through the hidden pointer, or loads into the result registers.
  QBE instead has a typed call (`%r =:t call`) whose result is a pointer to
  memory that its ABI pass allocates. That was rejected so that all memory
  is a slot before the ABI step, and so that `dst` could later be the final
  destination when nothing aliases it.
- Captured values (D12) need nothing extra. A nested function that
  captures gets a pointer to a block of its captured values as a scalar
  first parameter (D26). A captured aggregate is copied into that block
  when the nested function is defined, and reading it, or a field of it,
  is an address at a constant offset from that pointer, like any other
  `EK_ADDR`. `ARGAGG` and `PARAMAGG` are only for parameters and
  arguments the user wrote.

Up to the ABI step, all of this is the same for every target.

#### Where the ABI step goes (open)

The step turns `ARGAGG`, `PARAMAGG`, aggregate results and narrow
extensions into what the target does. It looks at a whole call, or a whole
parameter list, at once, so it can apply the rules that depend on earlier
arguments (SysV rule 4, arm64 rules 1 and 3) and give every piece its
actual register or stack offset, as QBE's `abi1` does. Everything after it
sees "this value is in `rsi` here", and not the ABI's rules.

The decision is where it runs relative to the SSA pass that promotes
slots:

- **(a) Late, as QBE does.** QBE's `abi1` runs after promotion, load
  forwarding, slot coalescing, GVN and code motion, and nothing after it
  forwards stores to loads. The optimizer sees an aggregate argument or
  parameter as an opaque use of memory, so anything passed or received is
  in memory, and lowering adds stores and loads that are never removed.
- **(b) First, before promotion.** The parser's output is the same as for
  (a). At the end of the function, the ABI step runs first, then the SSA
  pass. Promotion then sees only `LOAD`s and `STORE`s at constant offsets,
  and with the splitting below, a `str`'s data and length can become SSA
  values that go straight into or come straight out of registers.
- **(c) In the parser.** As (b), but `exp_*` splits aggregates when the
  call is emitted. That saves a pass, but puts per-target code in the
  parser and makes `luv il`'s output depend on the target.

(c) is ruled out (2026-11-20). The parser should emit the same IL for
every target, so that the backends can be tested on identical input, and
target details stay out of the parser. The lean is (b).

An example, with notation that's only for illustration: `PARAM i64 rdi` is
the value that arrives in `rdi`, and `CALL @len, rdi=%5, rsi=%6` passes
values in those registers. The luv code is:

```
def int len(str s):
    return s.len

def int caller(*u8 d, int n):
    x = str(d, n)
    return len(x)
```

The parser emits the same IL for every target:

```
func i64 @len
:0
  %1 = PARAMAGG 0, str     # the address of s
  %2 = ADD i64 %1, 8
  %3 = LOAD i64 %2
  RET i64 %3

func i64 @caller
  slot $0 str               # x
:0
  %1 = PARAM i64 0          # d
  %2 = PARAM i64 1          # n
  %3 = SLOT $0
  STORE i64 %1, %3
  %4 = ADD i64 %3, 8
  STORE i64 %2, %4
  %5 = ARGAGG %3, str
  %6 = CALL i64 @len, %5
  RET i64 %6
```

**SysV under (a).** `x`'s address is used by `ARGAGG`, so its slot stays in
memory through the SSA pass. So does `s`, since `PARAMAGG` is opaque. Then
the ABI step runs:

```
len:                        # PARAMAGG became a slot and two stores
  STORE i64 (PARAM i64 rdi), $s
  STORE i64 (PARAM i64 rsi), $s+8
  %3 = LOAD i64 $s+8        # reloads what was just stored
  RET i64 %3
caller:
  STORE d, $0               # from the parser
  STORE n, $0+8
  %5 = LOAD i64 $0          # ARGAGG became two loads
  %6 = LOAD i64 $0+8
  CALL @len, rdi=%5, rsi=%6
```

This is exactly what QBE generates. Compiling the equivalent QBE IL with
`qbe -t amd64_sysv` gives:

```
len:    subq $16, %rsp           caller: ...
        movq %rdi, 0(%rsp)               movq %rdi, -16(%rbp)
        movq %rsi, 8(%rsp)               movq %rsi, -8(%rbp)
        movq 8(%rsp), %rax               movq -16(%rbp), %rdi
        addq $16, %rsp                   movq -8(%rbp), %rsi
        ret                              callq len
```

`-t arm64_apple` is the same, with an address computation for each access.

**SysV under (b).** The ABI step runs first and gives the same rewrite as
above. But then the SSA pass runs, and `$s` and `$0` are only used by
`LOAD`s and `STORE`s at offsets 0 and 8. Each is split into two scalar
slots, and both are promoted. `%3` becomes `PARAM i64 rsi`, and the
caller's `%5` and `%6` become `d` and `n`:

```
len:
  %3 = PARAM i64 rsi
  RET i64 %3                # mov rax, rsi; ret
caller:
  CALL @len, rdi=d, rsi=n   # d and n are already in rdi and rsi
```

**Win64, either way.** `str` is 16 bytes, so it goes by pointer. In `len`,
`PARAMAGG` becomes the incoming pointer, `PARAM i64 rcx`, with no copy (see
below), and the field is a load through it: `mov rax, [rcx+8]`. The caller
has to make a copy, and passes its address:

```
caller:
  slot $t str               # the copy
  COPY $t, $0, 16
  CALL @len, rcx=$t
```

Under (b), `$0` is only stored at constant offsets and copied, so it's
split and promoted. The `COPY` from it becomes two stores of `d` and `n`
into `$t`. `$t` is passed by address, so it stays in memory. That's what
Win64 requires anyway. QBE does the same here, except that `$0` stays in
memory, so `d` and `n` are stored to `$0` and then copied to `$t` through
`rax`.

**Where the pieces don't match the fields.** SysV passes `{i32, i32}` as
one `i64`. After the ABI step, the slot has an `i64` access at offset 0 as
well as the `i32` field accesses at 0 and 4. They overlap differently, so
the slot isn't split. It stays in memory, and the result is the same as
(a). That's correct, just no faster.

**Drawbacks of (b), and why QBE is late.** The ABI step puts physical
registers into the IL. In QBE, that means copies to and from registers
like `rdi`, which aren't SSA values: `rdi` is assigned again before every
call. QBE's passes between promotion and `abi1` (load forwarding, GVN,
global code motion, if-conversion) would all have to leave those copies
alone, or keep them next to their calls. Lowering late keeps physical
registers out of the optimizer entirely, and keeps QBE's optimizer
target-independent. QBE also prints its IL after each pass for debugging,
and before `abi1` it reads the same for every target.

For luv, the costs of (b) are smaller, but they're real:

- The SSA pass runs on target-specific IL, so its output differs per
  target and has to be tested on each. The parser's output, the input to
  every backend, doesn't change.
- Physical registers must not appear as ordinary instructions. The ABI
  step only writes them as attributes: the register of a `PARAM`, and the
  register or stack offset of each of a `CALL`'s arguments and results.
  There's no "copy to `rdi`" instruction until register allocation, so
  the SSA pass never sees a register assigned twice. With only one
  forward pass (promotion, folding and CSE, no code motion), that's
  enough. A later optimized tier that moves code would have to keep the
  same rule.
- The SysV MEMORY argument area, and stack arguments in general, only
  exist at the call, relative to `rsp`. The ABI step needs a way to say
  "store this at outgoing offset 16 for this call". That's needed under
  (a) too, so it's not a cost of (b) as such.
- Pieces that don't match fields (`{i32, i32}` on SysV) leave the slot in
  memory. The ABI step could build the `i64` from the two fields with a
  shift and an `OR`, as clang does, so that the slot is still split.
  That's an improvement for later.

The benefit is mostly for small aggregates passed by value in registers:
`str`, and later ranges, slices and small structs like points. Large
aggregates go through memory on every target either way.

**The callee's copy.** On Win64, and for an arm64 aggregate over 16 bytes,
the callee gets a pointer to the caller's copy. luv parameters are
immutable and their address can't be taken, so the callee can use that
copy directly, and `PARAMAGG` is the incoming pointer. A SysV MEMORY
parameter is the same, at its address in the caller's argument area. The
caller has to make the copy unless its source is a temporary that isn't
used again, like a call result or a literal.

#### Promoting aggregate slots

D26 has aggregate slots escaped from the start, so they're never promoted.
With that rule, every `str` local stays in memory, and every read of
`s.len` is a load.

The proposal is to split unescaped aggregate slots into scalars:

- An aggregate slot is escaped only if its address is used for something
  other than a `LOAD` or `STORE` at a constant offset, `COPY`, or `ZERO`.
  With (b), the ABI pieces are `LOAD`s and `STORE`s, so they count as
  constant-offset uses.
- When the function ends, each distinct (offset, type) that an unescaped
  aggregate slot is accessed at becomes its own scalar slot. A `COPY`
  between two such slots becomes per-field loads and stores, and a `ZERO`
  becomes zero stores. Promotion then continues as it does now.
- If two accesses overlap differently (an `i64` at 0 and an `i32` at 4),
  or an array is indexed by a non-constant, the slot stays in memory.

QBE's alternative is load forwarding (`loadopt`). For each `LOAD`, it
searches backwards for the most recent `STORE` to the same address. If
alias analysis can show that nothing in between may write that address,
the load is replaced by the stored value. When the search reaches a join
in the control flow, it continues into each predecessor and inserts a phi
of the values found. Alias analysis is what makes it work: QBE tracks each
pointer's base (which slot or global) and constant offset, and whether the
slot has escaped, so it knows that a store through an unrelated pointer
can't change the value.

Load forwarding is more general than splitting. It works on slots whose
address has escaped, and on memory reached through pointers, as long as
nothing in between may alias. But it's a separate pass with its own alias
information and a search for every load, where splitting is promotion
keyed by (slot, offset) instead of by slot, inside the existing forward
pass. It also wouldn't help QBE with the ABI's stores and loads, which are
added after it runs.

The two don't exclude each other. Splitting covers locals, which are what
`str` and small structs mostly are. Load forwarding could be added to an
optimized tier later, if repeated loads through pointers (`p.x` in a loop
body) turn out to matter.

#### The parser

- `ExpDesc` gets `EK_ADDR`: an aggregate, or a field of one, in memory at
  `ref` plus a constant `off`. A field of an `EK_LOCAL` aggregate is
  `EK_ADDR` with the slot's `SLOT` as the base, and a field of an
  `EK_ADDR` adds its offset. A field of an aggregate parameter has
  `PARAMAGG` as the base. A scalar field is only loaded when it's used,
  like `EK_LOCAL`.
- Assigning one aggregate to another is `COPY`. Assigning a field is
  `STORE`.
- A struct literal is built in a new temporary slot, with a `ZERO` (or a
  `COPY` of the struct's default image, if it has defaults) and a `STORE`
  for each field given, and then copied to wherever it goes. A
  call that returns an aggregate gets a new temporary slot as its
  destination.
- Building a literal straight into its destination could be added for
  declarations, but splitting may make it unnecessary: a `COPY` between two
  unescaped slots becomes scalar moves, which promotion removes.
- Temporary slots are never reused within a function (D30). Later, slot
  coalescing can reuse them for slots that stay in memory.

#### The interpreter

`misc/interp.py` addresses slots by number now. With aggregates, each frame
gets a `bytearray`, slot addresses become byte offsets (with a base per
frame, so that addresses in different calls differ), and `LOAD`/`STORE`
use `struct.pack` for each type. `COPY`, `ZERO`, `ARGAGG` and `PARAMAGG`
are done directly. The interpreter can't check an ABI, so it only checks
layout, copies and field access.

#### Checking the classifiers before there's a backend

D25's test, which passes many struct shapes across a C boundary in both
directions and compares the bytes, needs a code generator. Most mistakes
are in classification, though, and that can be checked sooner. A script
can generate C structs, run `clang -target <triple> -S -emit-llvm`, and
compare clang's lowered signatures (`<2 x float>`, `[2 x i64]`, `byval`,
`sret`, as in the table above) with what our classifier produces for the
same luv structs. The table above came from doing this by hand.

#### Steps

1. Struct typing in the checking build, with layout and constant
   `sizeof`/`alignof`. Probably several pieces, driven by tests.
2. `AggInfo` and its bits, with unit tests taken from the checked cases.
3. Aggregate slots, `COPY`, `ZERO`, `EK_ADDR`, field loads and stores,
   struct literals, and a byte-addressed interpreter. The `struct_*` tests
   that don't pass structs to functions start to run.
4. `ARGAGG`, `PARAMAGG` and aggregate results, before any ABI step. The
   interpreter passes them by copying. `struct_small_by_value.luv` and
   `struct_large_by_value.luv` start to run.
5. `str` as `{*u8 data; int len}`: `str` locals, parameters and results,
   and `print` of a non-literal `str`. `print.luv` starts to run.

The classifiers, the clang comparison script, the ABI step and splitting
aggregate slots come with the backend.

#### Open questions

1. Where the ABI step goes: (a) or (b). (c) is ruled out. Both lean (b),
   unless a cost comes up that's not listed above.
2. Splitting aggregate slots, or QBE-style load forwarding. The lean is
   splitting now, with load forwarding possible in an optimized tier.
3. Decided (2026-11-20): empty structs are an error. A struct that needs
   no data can have a `u8` field. That avoids the question of matching C,
   where GCC and clang make an empty struct size 0, C++ makes it size 1,
   MSVC's C rejects it, and Apple ignores empty-struct parameters.
4. Decided (2026-11-20): `str` is a builtin type with a fixed layout, at
   least at first. The compiler needs detailed knowledge of it either way,
   so whether its definition is written in the prelude doesn't change
   much.
5. When the caller can skip the copy for an aggregate passed by pointer
   (Win64, arm64 over 16 bytes) or in SysV's argument area. A luv callee
   never writes its parameters, so for luv-to-luv calls the copy is only
   needed if the original could change during the call. That can't
   happen to an unescaped local or a temporary. It can happen to a local
   whose address was taken, if the callee writes through that pointer
   while reading its parameter. A C callee owns the copy and may write
   it, so calls to `foreign` functions always copy.
   Decided (2026-11-20): for now, an aggregate passed or returned by
   value in a `foreign` signature is an error. Calling the basic C
   runtime doesn't need it. The rule doesn't depend on the target, so a
   program doesn't compile on SysV and fail on Win64, where the same
   `str` would go by pointer. Runtime helpers that take a `str` on
   Win64 may need it sooner. Until then they can take the data and length
   as two scalar arguments, as `luv_print_str` does.
6. Decided (2026-11-20): extend narrow integers on both sides, on every
   target. On SysV the psABI says the opposite of what clang does: the
   bits above the type are unspecified, so the callee extends. clang's
   callees historically don't, and rely on the caller having extended to
   32 bits, which clang and GCC callers do. GCC extends on both sides.
   clang changed on 2026-11-16 (llvm-project b1c38d9b, "Fix arg extension
   incompatibility with platform psABI", #236450, based on D124435). It
   added `-msmall-int-abi-compat={platform,clang,both}`, defaulting to
   `both`: the caller extends and the callee doesn't assume it. That
   applies to SysV x86-32 and x86-64 except Darwin and PlayStation, where
   clang's old convention is now the platform ABI. clang 18, used for the
   checks above, predates it. Doing both is safe with every
   compiler. luv callers always extend (they already do), and luv callees
   extend too, at the cost of a `movsx`/`movzx` per narrow parameter (and
   per narrow result returned from C). When it's convenient, the callee's
   extension can go only in D26's second entry point, which every
   function that C can call already has, so that calls between luv
   functions skip it.

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
   - To check later: there's only one active Parser (D24), so it could be a
     file static in `parser.c` instead of a `Parser*` passed to every
     function. Its fields would then be at fixed addresses, which frees a
     register and might let the compiler fold addresses, and the static
     could be `alignas(64)` so that where `LexPos` lands no longer depends
     on `malloc`. The cost is that the library isn't reentrant, so two Ctxs
     can't parse at once (on different threads). Measure whether it's
     notably faster before deciding.
7. **Real code generator** behind the interface: luv60's IR, sqbe, or a
   bytecode interpreter in the spirit of `rewrite.txt`'s reference
   implementation. See D25.

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

Tracked in the **Decision log** at the top of section 2. Open: D20 (general `for`). Provisional: D21 (generics syntax). Open: D22a (methods on builtin types and the prelude), D23 (arrays, lists, slices and their memory), D25 (backend and execution model). Provisional: D26 (the IL). Decided, not implemented: D27 (numeric types and conversions). Deferred: D15 (to measurement), D5b (until
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
