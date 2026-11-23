# D10 ★ Comprehensions in a single pass

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
[Section 4](lookahead.md) shows (b) is the only non-LL(1) mechanism in the design, and it's
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
