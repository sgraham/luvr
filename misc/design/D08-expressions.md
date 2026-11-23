# D8 ★ Expressions: engine, operator set, precedence

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
