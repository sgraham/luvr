# D3 ★ Layout tokens (how newlines and indentation reach the parser)

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
