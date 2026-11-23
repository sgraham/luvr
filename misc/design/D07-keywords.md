# D7 Keywords vs. builtin names

luv60 reserves many words (`fmt` is now dropped, D13): `print len range fmt sizeof offsetof alignof
typeof cast check alloc del relocate typeid`, plus about 18 type names. Each
keyword is one less identifier and one more interned "reserved" entry, but it
costs nothing at runtime. The question is which ones need **special syntax**:

| Word | Special syntax? | Recommendation |
|---|---|---|
| builtin types (`int`, `u8`, `str`, ...) | yes: they start type FIRST sets (D5) | keyword |
| `print` | `print expr` without parens | keyword statement (or make it a function and drop the rule; **user's call**) |
| `cast` | `cast T(x)`: a type, then a parenthesized operand | keyword. **Decided: required parens**, like `sizeof(...)`. Plain `T(x)` can't work for all types in expression position (`[4]int(x)` vs. a list literal), so the `cast` keyword is what enables types there. With parens, there's no precedence rule, and `cast i64(x) * -1` vs. `cast i64(x * -1)` is explicit. A long operand wraps inside the parens (these parens are required syntax, not grouping, so [section 9](formatter.md) rule 2 is unaffected) |
| `sizeof`, `alignof` | take a type **or** an expression | keyword. First token decides: builtin type or `*` → type. Literal, `(`, `-`, `not`, `&` → expression. `NAME`/`TYPENAME` → D5 postfix chain, then `VTYPE` → type, otherwise expression (`Stuff(1)` has the same size as `Stuff` anyway). `[`/`{` → **always type** (fixed rule, still LL(1)). `sizeof(([1,2]))` forces an expression. No `sizeof_expr` keyword needed |
| `typeof` | takes an expression, **produces a type** | keyword, and a type-starting token: `typeof(x) y = ...`, `sizeof(typeof(e))` |
| `offsetof` | `(type, field)` | keyword, type only |
| `check`, `pass`, `global`, `with`, `on`, `foreign`, `struct`, `import`, `elif` | statement introducers | keyword |
| `len`, `range` | no, ordinary call syntax | **not keywords**. Resolve as builtin symbols in the global scope (compile-time intrinsics). (`fmt` is dropped in favour of `\(expr)` interpolation, D13) |
| `true false null` | literals | keyword |
| `in`, `not`, `and`, `or`, `as` | operators | keyword. (Lua 4 avoided making `in` reserved with a "dirty" check, `lparser.c:873`. We don't need that.) |
