# 8. Check against the 10 LL(1) rules from the earlier chat

| # | Rule | This plan | Notes |
|---|---|---|---|
| 1 | Every construct starts with a distinguishing token | **Mostly follows** | Every statement except declaration/expression statements starts with a keyword ([D5](D05-decl-vs-expr.md) table). Declaration/expression statements start with `TYPESTART` or `NAME`, and [D5](D05-decl-vs-expr.md) resolves those. |
| 2 | Avoid C-style declarations | **Deliberately breaks** | Requirement. Restored to LL(1) by the case-based TYPENAME token ([D4](D04-types-vs-values.md)) and left-factoring ([D5](D05-decl-vs-expr.md)). What it costs: the naming convention becomes a hard language rule, and prefix `*` deref is unavailable. |
| 3 | No `<>` generics; type vs. expression decided syntactically | **Follows** | No `<>`. Types are `*T`, `[N]T`, `[]T`, `{K}V`, TYPENAME, `Name{T, ...}`. Generics use `{}` ([D21](D21-generics.md)), unambiguous only because of [D4](D04-types-vs-values.md). `<>` would mostly work too because of [D4](D04-types-vs-values.md), except explicit call-site type arguments. |
| 4 | Required block delimiters, no dangling else | **Follows, via layout** | INDENT/DEDENT *are* the delimiters ([D2](D02-blocks.md), [D3](D03-layout-tokens.md)). Dangling else can't happen because `else` must line up with its `if`. A [D3a](D03-layout-tokens.md) one-line suite allows simple statements only, so no nested `if` there. |
| 5 | Lambdas distinguished up front | **N/A for now** | No lambdas. If added, start them with a keyword (`def(int x) int: ...` or similar) so they're never `(a, b) => ...`. |
| 6 | Precedence climbing / Pratt | **Follows** | Lua 4 `subexpr` precedence climbing ([D8](D08-expressions.md)), replacing luv60's Pratt table. |
| 7 | Self-terminating or explicitly terminated statements | **Follows, via layout** | NEWLINE/DEDENT/EOS terminate simple statements ([D3](D03-layout-tokens.md)). Optional-semicolon problems go away because newlines inside brackets are whitespace and nowhere else. |
| 8 | Context-free lexer, no context-dependent keywords | **Follows (better than luv60)** | Drops luv60's import-dict lookup in the lexer ([D4](D04-types-vs-values.md)). TYPENAME vs. NAME depends only on the identifier's characters. One caveat: the lexer keeps state for layout (indent level, bracket depth), but that depends only on characters, never on parser state. |
| 9 | Assignment: keyword, or parse an expression then check for `=` | **Follows the Lua approach** | [D5](D05-decl-vs-expr.md): parse the postfix chain, then branch on `expdesc.k` and the next token (`=`, augmented op, NAME for declarations). Named arguments need no trick: `.name=` starts with a token no expression can ([D9](D09-calls.md)). |
| 10 | No left recursion | **Follows** | Postfix chains and binary operators are loops (`var_or_func_tail`, `subexpr`). |

Two items go beyond those rules and aren't covered by them: comprehensions
([D10](D10-comprehensions.md)) and forward references ([D11](D11-forward-refs.md)). The rules assume an expression's names
can always be resolved left to right in one pass. Python-order comprehensions
and use-before-definition break that assumption whatever the grammar is.
