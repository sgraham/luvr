# D1 ★ Single-pass emit vs. building an AST

Lua 4 parses and emits bytecode in one pass. luv60 also works in one pass
(parse → type check → IR). Some luv features don't fit one pass well:
comprehensions ([D10](D10-comprehensions.md)), forward references ([D11](D11-forward-refs.md)), and capture-by-value
analysis ([D12](D12-nested-functions.md)).

- **(a) Strict single pass, Lua style.** Fastest, with no AST allocation.
  But every construct has to be ordered so names and types are known when
  first seen, and that shapes the syntax.
- **(b) Flat AST** (an arena array of fixed-size nodes, with indices instead of
  pointers) followed by a separate check/emit pass. It costs one write and one
  read per node. It frees the syntax from emission order and makes
  comprehensions, hoisting and incremental work (luv goal #2) easier.
- **(c) Hybrid.** Single pass, with escape hatches: a top-level skim pass
  ([D11](D11-forward-refs.md)) and token-range replay for the few constructs that bind late ([D10](D10-comprehensions.md)).

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
- [D10](D10-comprehensions.md) uses skim and rewind. Instead of lexing the bracket twice, the skim
  can record tokens into a small scratch buffer of 8-byte packed tokens and
  replay them. That's local and bounded, so it's an implementation detail
  rather than an architecture change.
