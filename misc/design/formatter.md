# 9. Keep the grammar formatter-friendly (constraints for a later formatter)

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
   [section 3](grammar.md)'s grammar becomes the normative spec, and the formatter's tests
   include `format(f)` re-lexing to the same token stream as `f` (ignoring
   layout, trivia and added grouping parens) for every file in the test
   corpus.
