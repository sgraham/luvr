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

[Section 8](ll1-rules.md) checks the plan against the 10 LL(1) design rules from the
earlier chat.

This file has sections 0 to 2 and 7. Each decision in section 2 is in its
own file, `Dnn-name.md`, linked from the decision log. The other sections
are [grammar.md](grammar.md) (3), [lookahead.md](lookahead.md) (4),
[lexer.md](lexer.md) (5), [implementation-steps.md](implementation-steps.md)
(6), [ll1-rules.md](ll1-rules.md) (8) and [formatter.md](formatter.md) (9).

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
| `expdesc` with `VLOCAL/VGLOBAL/VINDEXED/VEXP`, discharged late | **Keep and extend** (add `VTYPE`, `VNAME_UNRESOLVED`, ...) | This is what lets us "parse a prefix, then decide" without backtracking (see [D4](D04-types-vs-values.md), [D8](D08-expressions.md)). |
| `namestat`: parse `var_or_func`, then branch on `v.k` (`lparser.c:980`) | **Keep the pattern** | Same trick as declaration vs. expression statement. |
| `check_match(ls, what, who, line)` error with opener line | **Keep** | Cheap and gives good errors. |
| `luaK_*` code generator interface (`lcode.c`) | **Keep as an interface**, start with a null implementation | Lets us measure parse-only speed honestly, like luv60's `parse_syntax_check.c`. |
| Errors via `lua_error` → `longjmp` | **Keep** | First error aborts. No recovery machinery on the fast path. |
| Free-form layout with `end`-terminated blocks | **Change** to indentation ([D2](D02-blocks.md)) | That's the language we want. [Section 3](grammar.md) shows it stays LL(1). |

---

## 2. Decision points

Each decision lists the options, what they cost in **lookahead** and **speed**,
and a recommendation. **★** marks the decisions that determine whether the
grammar is LL(1). The rest are local.

### Decision log

| # | Status | Decision |
|---|---|---|
| [D1](D01-single-pass.md) | **decided** 2026-11-07 | Single pass as much as possible: (c) hybrid. The only exceptions are [D10](D10-comprehensions.md)'s lexer rewind and [D11](D11-forward-refs.md)'s skim-on-miss. No general AST. |
| [D2](D02-blocks.md) | **decided** 2026-11-07 | Significant indentation, fixed 4-space unit. Tabs and non-multiples of 4 are errors. |
| [D3](D03-layout-tokens.md) | **decided** 2026-11-07 | One layout token per line boundary (`NEWLINE` / `INDENT` / `DEDENT`×k), and newlines are whitespace inside brackets. [D3a](D03-layout-tokens.md): **yes**, one-line suites for simple statements. [D3b](D03-layout-tokens.md): **no** `\` continuation. The `newline_before` flag variant is only a later measurement idea. |
| [D4](D04-types-vs-values.md) | **decided** 2026-11-07 | Lexical naming convention as a hard rule (CamelCase / single capital = `TK_TYPENAME`, builtin types are keywords). Drop `IDENT_CONST` and `IDENT_IMPORT` lexing. A single capital is always a generic type parameter ([D21](D21-generics.md), 2026-11-12). |
| [D5](D05-decl-vs-expr.md) | **decided** 2026-11-07 | Statement-start dispatch as in the [D5](D05-decl-vs-expr.md) table: no prefix deref for now (revisit, postfix form, if `p[0]` gets tedious), only call/assignment expression statements. [D5a](D05-decl-vs-expr.md): **no** multiple declarators. [D5b](D05-decl-vs-expr.md): **deferred**. The multiple-values-not-tuples proposal is kept below for reference. Revisit when real code needs it, alongside a Zig-like `!T`/`try` error type, which may be the better answer. The parenthesized-RHS formatter escape is not agreed. |
| [D6](D06-assignment-declares.md) | **decided** 2026-11-07 | (b) declare at first assignment, visible from that point on (current luv60 behaviour), function-level scope, `global` to opt out. An intentional difference from Python's whole-function rule. |
| [D7](D07-keywords.md) | **decided** 2026-11-07 | As in the [D7](D07-keywords.md) table. **`print` stays a keyword statement** (`print expr`). **`cast T(x)`** with required parens (matching `sizeof(...)` etc.): after `cast` the parser reads a type, and `(` must follow. No cast precedence rule needed. Intended for pointer↔int and similar. `sizeof`/`alignof` use the first-token rule, `typeof(e)` is a type form, `offsetof` is type-only. `len`/`range` are builtin symbols (`fmt` is dropped, [D13](D13-literals.md)), not keywords. |
| [D8](D08-expressions.md) | **decided** 2026-11-07 | Lua 4 `subexpr` precedence climbing with the [D8](D08-expressions.md) table. `not` at Python precedence (below comparisons). Comparisons non-associative (`a < b < c` is an error). **No ternary.** `not in` via `not` in binop position, slices as in [D8](D08-expressions.md), augmented assignment as statements only, assignment is not an expression. |
| [D9](D09-calls.md) | **decided** 2026-11-07 | No `T(x)` conversion syntax; `Name(...)` is only a compound literal (see [D7](D07-keywords.md)). Named arguments use **C-style designators**: `Stuff(.a=44, true)`. A leading `.` can't start an expression ([D13](D13-literals.md)), so this is LL(1) from one token, with no lookahead slot and no parse-then-reinterpret. Name lookup is never deferred, and `f(x=1)` typos are errors. Named parameters are **not** planned for ordinary functions, so `.name=` only ever means a struct field (may be revisited). |
| [D10](D10-comprehensions.md) | **decided** 2026-11-07 | Python order via **lexer rewind** (b). Defaults recorded: a comprehension opens its own scope, so its loop variables don't leak (`compr_shadow.luv`), the one exception to [D6](D06-assignment-declares.md)'s function-level scope. Multiple `for`/`if` clauses are allowed, nesting left to right per PEP 202. Multiple loop targets (`for k, v in`) allowed. Dict comprehensions deferred (the skim must pass the first depth-0 `:`). |
| [D11](D11-forward-refs.md) | **decided** 2026-11-07 | Assume define-before-use, **skim on first miss** (c), with the limits listed in [D11](D11-forward-refs.md). Defaults recorded: no forward-declaration syntax (skim covers mutual recursion). Nested functions are strictly define-before-use (only indent-0 lines are skimmed, and [D12](D12-nested-functions.md)'s capture-at-definition implies it anyway). |
| [D12](D12-nested-functions.md) | **decided** 2026-11-07 | Implicit capture **by value at definition time**, multi-level, via the `FuncState->prev` chain. Pointers, escaping and name-resolution details are for later. |
| [D13](D13-literals.md) | **decided** 2026-11-07 | Floats as in the [D13](D13-literals.md) table (digit after `.` or exponent; `f32`/`f64` suffixes; no `1.f`). Strings `"` only. **Swift-style `\(expr)` interpolation** via a lexer mode stack (`STRING_BEGIN`/`MID`/`END`, details in [D13](D13-literals.md)). Raw newlines are errors anywhere in a string, including inside `\( )`. **`fmt(...)` is dropped**: it was compile-time only, so interpolation covers it. Manual argument reordering (`fmt("{1} {0}")`) is accepted as lost. Format specs deferred. Escapes: `\n \t \r \\ \" \0 \xHH \u{...}` and `\(`, none of Lua 4's `\a \b \f \v \ddd`. **No raw or multiline strings** for now (`r"..."`/`"""..."""` can be added later without conflict). |
| [D14](D14-comments.md) | **decided** 2026-11-07 | `#` to end of line, skipped with `memchr`. **Trailing comments allowed** (luv60 rejected them only because of its SIMD indexer). No block comments. |
| [D15](D15-interning.md) | **decided** 2026-11-17 | One table for keywords and identifiers (Lua `marked`-style), and a `binding` pointer on each `Name` for one-load scope lookup (chosen over a fixed per-function locals table with a linear scan, 2026-11-08). The intern table is one shared open-addressing table of `{tag, val}` slots, positioned by the tag, which is also used for `NameMap` and IL constants. Benchmarked layouts, load factors and Verstable all came out the same or slower. |
| [D16](D16-errors.md) | **decided** 2026-11-07 | Line counter **in the lexer only** (free, since layout already visits every `\n`). Used for errors and debug line info. Tokens carry only a `u32` offset, and columns are computed lazily on error. Errors: first error → `longjmp` to the handler. **Library-safe**: all state lives in a context struct (no mutable globals, unlike luv60's `glob`/`tu`), all allocation comes from context-owned arenas reset by the handler, no `exit`/`abort`/stdio in the library (message + offset stored in the context, internal asserts take the same path), keywords interned per context (Lua 4's `luaX_init` per `lua_State`). |
| [D17](D17-toplevel-grammar.md) | **decided** 2026-11-07 | Separate `toplevel_stat()` and `stat()` switches instead of luv60's repeated `if (toplevel) error(...)`. `module -> { import } { toplevel_stat } EOS`. Nested `def` allowed; `struct`/`on`/`foreign`/`import` top-level only. |
| [D18](D18-future-syntax.md) | n/a | Guidance for future syntax, nothing to decide now. |
| [D19](D19-formatter-syntax.md) | **decided** 2026-11-07 | [D19a](D19-formatter-syntax.md): trailing commas **only in list/dict literals** (not calls, params, or compound literals). [D19b](D19-formatter-syntax.md): **no** adjacent string concatenation. [Section 9](formatter.md) rules 1–5 remain constraints on everything else. |
| D20 | open | **Looping constructs.** No `while` keyword (removed from the lexer). Investigate a general `for` that covers `while`-style loops (e.g. `for cond:` vs. `for x in xs:`). Interesting for LL(1): after `for`, telling `NAME [, NAME] in` from a condition expression needs care, since a condition can also start with `NAME`. Options include parse-then-decide on `in`, as with [D5](D05-decl-vs-expr.md)'s expdesc dispatch. |
| [D21](D21-generics.md) | **decided** 2026-11-08 (provisional), **revised** 2026-11-12 | Generics use `{}`: `List{int}`, `struct Pair{A, B}`, `def T max{T}(...)`, explicit `max{u64}(...)`, inferred by default. Monomorphized by re-parsing the body per instantiation. Revised: type parameters are exactly one capital letter (`[A-Z]`), and a single capital letter is always a type parameter, never a struct name. `{...}` lists are still required and give the order of explicit type arguments. A return type that uses parameters before `{T}` is checked afterwards with a 26-bit mask, so binding stays single-pass. Options explored (`<>`, `[]`, `'`, `!`, parameter placement, `$T`, alphabetical order) are recorded in [D21](D21-generics.md), since the user may revisit this after writing generic code. **Revised 2026-11-13:** no free generic functions. Type parameters only come from a struct's list or a method's receiver, so type arguments are never inferred from function arguments. Free generics can be added later without breaking anything. |
| [D22](D22-methods.md) | **decided** 2026-11-08 | Methods (`on`): keyed on (generic type, name), no specialization, no methods on pointers, methods only in the receiver type's own module (builtin types: the prelude, [D22a](D22-methods.md) open), no free-function overloading. Static methods (`List{int}.new()`) and dunders as operator spellings. A method is a free function with nicer syntax and a namespace: `xyz.f(5)` is `Xyz.f(&xyz, 5)`. |
| [D23](D23-arrays-lists.md) | **open** 2026-11-12, option B preferred | Arrays, lists and slices, and how their memory is managed. What `a = [1, 2, 3]` is, what `a[1:]` is, what a comprehension produces, and what the user has to do to free each of them. This affects literal typing, comprehensions ([D10](D10-comprehensions.md)), `[]T`/`{K}V` ([D21](D21-generics.md), [D22a](D22-methods.md)) and likely more syntax, so it's settled before further type work. Option A: lists in locals own a region, a current pool, copying results out of scratch blocks. **Option B (preferred):** one dynamically scoped current arena that every allocation goes into, `with Arena():` and `into a:` blocks, and opt-in storage annotations. Under B: literals are lists, slicing copies, comprehensions produce lists, views are deferred to a later `View{T}`/`Slice{T}` type. Strings are immutable `{ptr, len}` with a builder, structs are values, `[]T` is `*List{T}`, and `&Stuff(...)` allocates in the current arena. |
| [D24](D24-modules.md) | **decided** 2026-11-13, **implemented** 2026-11-14 | Modules and imports. A module's canonical name is its path relative to the root, normalized lexically, and keys a dict of `Module`s on Ctx, so each file is loaded and parsed once however it's imported. Files are loaded through a callback on Ctx. A `Module` (name, buffer, table of globals) lasts as long as the Ctx, and a `Parser` is per-parse state that points at one. Globals move from `Name.global` to the module's table, with a `Name.binding` cache only if the table probe costs too much. Resolves [D21](D21-generics.md)'s cross-module instantiation question: the one active Parser is re-pointed at the defining module. |
| [D25](D25-backend.md) | **open** 2026-11-14, direction agreed | Backend and execution model. Survey of existing small backends and execution models. Direction: run in process first, on x64 Windows, x64 Linux and aarch64 macOS. One backend of our own: a linear SSA IL per function built in the single pass, ABI lowering with structs by value as QBE does it, and a backward pass that allocates registers, selects and encodes, into sections with relocations. A JIT is the first consumer, and C objects are linked into a host program by the user. The optimized tier is more passes on the same IL. No interpreter, no `.s`, no third-party backend. sqbe is the fallback and the comparison. Debug info, executable writers and a C emitter are deferred. |
| [D26](D26-il.md) | **provisional** 2026-11-14 | The IL. Per-function flat array of 8-byte instructions (opcode, type, two 24-bit references, constants below a bias), value types `i32`/`i64`/`f32`/`f64`, aggregates as memory. Two stages: the parser appends raw instructions with every local as a frame slot, then one forward pass at the end of the function promotes slots whose address wasn't taken to SSA and does folding and CSE. Blocks in source order with structured control flow. C calling convention for luv functions, QBE-style aggregate arguments, and the luv context pointer in a pinned callee-saved register. |
| [D27](D27-numeric-types.md) | **decided** 2026-11-15 | Numeric types and conversions, as in Go. `int` is an alias of `i64`, and error messages say `int`. `len` and indexes are `int`, and `sizeof` is an untyped constant ([D28](D28-constants.md)). No other aliases: no `uint`, `byte`, `float` or `double`, and no `f16` for now. `int` is an error in `foreign` signatures, since C's `int` is 32 bits. There are no foreign-only types, and `foreign` signatures match C by ABI, not by C type. No `void` in luv code. `codept` is left for [D23](D23-arrays-lists.md). Untyped constants adapt to context, defaulting to `int` and `f64`, with range checks. No implicit conversions between numeric types (widths, signedness, `f32`/`f64`, int/float). Any integer type is accepted as an index or shift count. Lossless widening was rejected because it hides where arithmetic wraps, and is revisited only if the casts prove too painful. `cast` converts between any numeric types, between pointers, and between pointers and `u64` (revised 2026-11-21: not `i64`, as Go only converts pointers to `uintptr`; a pointer difference is a `u64` subtraction, cast to `i64` if needed). Float to integer out of range is unspecified, and constants must be in range for the target. |
| [D28](D28-constants.md) | **decided** 2026-11-15 | Constants and folding, as in Go. Constant expressions are folded in `ExpDesc` as they're parsed. Untyped integer constants are `__int128` (a two-`u64` implementation later for MSVC), untyped floats are `double`, and they're range checked only when converted to a type. Typed constants fold at their width, and overflow is an error. A constant zero divisor is an error. ALL_CAPS constants keep their initializer's untypedness, and a type or suffix makes them typed. `~` on an untyped constant is `-x - 1`. |
| [D29](D29-test-targets.md) | **deferred** 2026-11-16 | Tests as ninja targets. `m` would pass all of its arguments to ninja, with `test` and each test's name as phony targets. Waits until there's one compiler binary to run on every input, so that one generic rule covers every test. `m` reruns the generator each time to pick up new tests, and test edges have no stamp files, so they always run. |
| [D30](D30-emission.md) | **proposal** 2026-11-16 | Emission for the vertical slice. `ExpDesc` gets a kind (constant, local slot, ref, string literal, void) replacing `is_const`, `exp_*` helpers in the parser and `il_*` buffer functions in `il.c`. `parser.c` is built twice (`EMIT` 0 or 1) behind one `luv` driver (`luv parse`, `luv il`), so checking costs nothing extra. Unsupported constructs are errors in the emitting build. The slice runs `luv il` output in a throwaway Python interpreter. SSA concerns are ignored until the SSA pass. |
| [D31](D31-aggregates.md) | **proposal** 2026-11-20, partly decided | Aggregates. C layout for every struct (no `foreign struct` mark), natural alignment, nothing above 8, empty structs are errors, `str` a builtin with a fixed layout. A per-struct summary (size, alignment, two SysV SSE bits, an HFA kind) is all the Win64, SysV and Apple arm64 classifiers need, with the per-ABI rules checked against clang and the ABI documents. Aggregates are addresses in the IL (`COPY`, `ZERO`, `ARGAGG`, `PARAMAGG`, aggregate results). The ABI step is never in the parser. Aggregates by value in `foreign` signatures are errors for now. Narrow integers are extended on both sides. Leaning: the ABI step before promotion, and splitting unescaped aggregate slots into scalars. |
| [D32](D32-prelude.md) | **proposal** 2026-11-23 | A prelude in luv. `src/prelude.luv` is compiled into the binary and parsed as module 0 by the same Parser before the root module. It starts with only `struct str: *u8 _data; int _len`, which fills in the reserved `TY_STR` TypeInfo so that `ty_str` stays a constant. Later it holds methods on builtin types (`__len__`, `__eq__`, `__str__`), `range`, generic containers, and the runtime's functions as `foreign` declarations. Only what has to be in the prelude goes there. Free prelude names are found in an array indexed by NameId, which works because the prelude's names are interned first. They can be shadowed silently, and `max = 5` declares a local. Embedded with a header from gen_ninja.py. Deferred: how an allocating operation gets the current arena. |

## 7. Decisions that are the user's call

Tracked in the **Decision log** at the top of section 2. Open: D20 (general `for`). Provisional: [D21](D21-generics.md) (generics syntax). Open: [D22a](D22-methods.md) (methods on builtin types and the prelude), [D23](D23-arrays-lists.md) (arrays, lists, slices and their memory), [D25](D25-backend.md) (backend and execution model). Provisional: [D26](D26-il.md) (the IL). Decided, not implemented: [D27](D27-numeric-types.md) (numeric types and conversions). Deferred: [D15](D15-interning.md) (to measurement), [D5b](D05-decl-vs-expr.md) (until
real code needs it).
