# D18 Things that are easy to make non-LL(1) by accident (future syntax)

| Future feature | Trap | LL(1)-safe form |
|---|---|---|
| Methods without `on` | `def Stuff.method()` vs. `def mod.Type func()` needs 2–3 tokens | keep `on T def name(self...)` |
| Generics | `List<int>` vs. less-than, `List[int]` vs. indexing | `{}` (D21): `{` after a name or type always opens type arguments. This works only because of D4 |
| Function types | `def(int) int` in type position | `def` is a type FIRST token only in type position, never at statement start, where `def` is a definition |
| Prefix deref | `*p = 1` vs. `*int p` | postfix deref (D5) |
| Tuples / parenthesized targets | `(a, b) = ...` | don't add. Use D5b's `a, b = ...` |
| Decorators | `@name` before `def` | `TK_DECORATOR` token, LL(1) prefix |
| Enums | `Color.RED` | `TYPENAME '.' NAME` in expression position, which `VTYPE` handles |
