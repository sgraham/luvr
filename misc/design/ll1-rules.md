# 8. Check against the 10 LL(1) rules from the earlier chat

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
