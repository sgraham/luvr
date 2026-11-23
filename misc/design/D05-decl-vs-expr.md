# D5 ★ Declaration vs. expression at statement start

Statement-start FIRST sets, assuming [D4](D04-types-vs-values.md)(a):

| First token | Can be | Decision |
|---|---|---|
| keyword (`if`, `for`, `def`, `return`, ...) | that statement | LL(1) switch, like Lua's `stat()` (`lparser.c:1022`) |
| builtin type, `*`, `[`, `{` | **declaration only** | Requires the rules below: no expression statement may start with these |
| `TK_TYPENAME` | `Stuff s` (decl) or `Stuff(1).f()` (expr) | **Left-factor**: parse the type. If the next token is `NAME` it's a declaration. If it's `(`, it's a compound literal; continue as a postfix expression |
| `TK_NAME` | `x = ...`, `x += ...`, `f()`, `a.b = ...`, `mod.Type v` | Parse the postfix chain into an `expdesc` (Lua `var_or_func`). Then branch on `v.k` and `t`: `VTYPE` followed by `NAME` → declaration. Lvalue followed by `=` or augmented op → assignment. A call → call statement. Otherwise "syntax error" |
| anything else | — | error "statement expected" |

Constraints this puts on the expression language:

- **No prefix dereference `*p`.** luv already uses `p[0]` and auto-deref on
  `.`, so a statement starting with `*` is always a pointer type. If deref
  syntax is ever needed, make it postfix (`p.*` or `p^`) so it doesn't take
  `*` away from types. (`*` followed by `NAME` would be LL(1) for one level,
  but `**p` vs `**int` makes it LL(k).)
- **Only certain expression statements are allowed**: calls, assignments,
  augmented assignments. Lua enforces the same rule (`namestat` +
  `check_condition(... "syntax error")`). That makes `[f(x) for x in xs]` or
  `{...}` as a bare statement illegal, which is no loss.
- **Qualified types (`mod.Type x`) need no lexer help.** The parser resolves
  `NAME '.' TYPENAME` as a postfix chain. Because `Type` is lexically a
  type, the `expdesc` becomes `VTYPE`, and the following `NAME` makes it a
  declaration.

Declaration form sub-decisions:

- **D5a:** multiple declarators (`int a, b = 1, 2`)? **Lean no.** One
  declarator per statement keeps the code simple, and the tests never use it.
- **D5b (deferred; proposal kept for reference): multiple return values, not tuples.** A function may
  return several values (`def int, bool parse(str s):`,
  `return n, true`), but a value list is **not a type**. It can't be stored
  in one variable, passed as one argument, indexed, or nested. It can only
  be consumed by multi-assignment (`n, ok = parse(s)`, `a, b = b, a` with
  the whole right side evaluated first) or forwarded with `return f()` from a
  function with the same return types. `x = f()`, `g(f())`, `print f()` on
  a multi-value `f` are errors (Go's rule, not Lua's truncation). The ABI
  returns an anonymous struct, a backend detail.
  - LL(1): signature `def T {',' T} NAME '('`: after a type, `,` means more
    types, `NAME` is the function name. Multi-assignment is Lua's recursive
    `assignment` (`,` after the first target). Declaring follows [D6](D06-assignment-declares.md): new
    names on the left are declared with types from the signature. No
    typed multi-declaration (consistent with D5a).
  - **Not** `def (int, bool) f(`: at statement start, `def (` would clash
    with function-type declarations `def(int) int cb = ...` ([D18](D18-future-syntax.md)). Unless
    function types use a different keyword (`fn(int) int`), keep the
    unparenthesized list.
  - `_` becomes a reserved blank name for discarding (`n, _ = parse(s)`).
  - Formatter ([section 9](formatter.md)): a parenthesized expression list is allowed
    *only* as the entire right side of a multi-assignment or `return`
    (`return (a,\n b)`). That's grouping, not a tuple. After `(` and an
    expression, `,` means a list. One token.
  - Alternatives: return a struct (`return ParseResult(n, true)`, works
    today) or out-params (`*int out`). An error/optional type (`!T`/`?T`
    with `try`) for no-exceptions error handling is a separate, later
    decision that this doesn't block.
