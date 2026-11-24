# 6. Implementation steps

The original plan, from before any code, with each step's status. Later
work is planned in each decision's own file, such as
[D30](D30-emission.md), [D31](D31-aggregates.md) and
[D32](D32-prelude.md).

1. **Lexer and token dump.** Done: `src/lex.c` per
   [section 5](lexer.md), and `lexdump`, which prints tokens, counts them,
   or checks rewinding (`-r`). Lexing `dumbbench.luv` alone is the floor
   for everything else. Speed comes from the char-class table, padding (no
   bounds checks), zero-copy names and strings, and libc's `memchr`, with
   no SIMD of our own.
2. **Parser skeleton.** Done 2026-11-07: `src/parser.c`. All of the luv60
   tests parse once converted to the decided syntax, and its syntax errors
   are reported at the same line and column as luv60. When written, parsing
   `dumbbench.luv` took 0.45 s against 0.43 s for lexing alone and 1.20 s
   for Lua 4 with its code generator stubbed.
3. **`ExpDesc` and name binding** ([D5](D05-decl-vs-expr.md),
   [D6](D06-assignment-declares.md), [D9](D09-calls.md),
   [D15](D15-interning.md)). Done, along with typing.
4. **Forward references** ([D11](D11-forward-refs.md), lazy skim). Not
   done. Calls are define-before-use until then. Its tests: functions
   called before their definition, mutual recursion, and a malformed later
   header referenced earlier (which must report the header's error, not
   "undefined"). For coverage of the skim, a debug flag that runs the skim
   up front on every file must give identical results on the whole suite,
   and a script that reverses each test's top-level definitions must too.
   dumbbench should be unchanged, since it never misses.
5. **Comprehensions** ([D10](D10-comprehensions.md)). List comprehensions
   are done. Dict comprehensions are later.
6. **Profile and tune.** Ongoing. What's been found:
   - Where the hot lexer state lands in a cache line changes the whole
     parse by 5–8% (2026-11-13). It seemed to matter whether all 40 bytes
     of `LexPos` are in one line. That depends on allocations made before
     `parse_file()`, so it's fragile, and not worth tuning yet.
   - `lex_next`'s address changes dumbbench by up to 4%, even with
     functions aligned to 64 bytes, so `lex.c` is linked first.
   - Inlining into the recursive `primary()`/`subexpr()` shows up. Moving
     rarely run code out of hot functions at function granularity helps,
     but moving whole `primary()` cases that are common in realistic code
     doesn't. Judge by interleaved cycles, and by betterbench when the two
     benchmarks disagree.
   - To check later: `Parser.locals` is a large array between `ls` and the
     rest of the parser's fields, which may spread warm data over more
     lines than it needs. Try moving it out of line.
   - To check later: there's only one active Parser
     ([D24](D24-modules.md)), so it could be a file static, `alignas(64)`,
     instead of a `Parser*` passed to every function. That frees a
     register and makes where `LexPos` lands independent of the allocator.
     The cost is that two Ctxs can't parse at once on different threads.
   - To check later: LTO and PGO.
7. **Code generator.** See [D25](D25-backend.md) and
   [D26](D26-il.md). The parser produces IL ([D30](D30-emission.md)), which
   `misc/interp.py` runs for now.
