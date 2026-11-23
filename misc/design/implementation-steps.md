# 6. Implementation steps

Each step is measurable on its own.

1. **Lexer + token dump.** `llex.c/h` per [section 5](lexer.md), and a driver that lexes a
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
