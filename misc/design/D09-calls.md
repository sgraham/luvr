# D9 Calls, keyword arguments, compound literals

`Stuff(a=44, true)` and `Stuff(x=99, false)` exist (`compound_literal.luv`,
`struct_with_initializer.luv`). `NAME '='` looks like it needs two tokens. In
Lua 4, this is exactly the one place that uses `lookahead()`
(`constructor_part`, `lparser.c:554`).

- **(a) Use the lookahead slot**, like Lua. Simple, and costs one branch in
  every `next()` (see [section 1](README.md)).
- **(b) Parse first, decide after.** Parse an argument as an expression. If
  the resulting `expdesc` is a bare, undischarged `VNAME` *and* `t == '='`,
  turn it into a keyword argument. This only works if a bare name isn't
  resolved or emitted until it's discharged. Lua's late discharge already
  works this way.

**Recommendation: (b)**, which helps remove the lookahead slot entirely.

**Decided instead: (c) C-style designators**, `Stuff(.a=44, true)`. An
expression can't start with `.` ([D13](D13-literals.md) bans `.5`), so `'.' NAME '='` in
argument position is LL(1) from its first token. Compared with (b): no
deferred name resolution inside argument lists (with (b), `Stuff(a=44)`
can't look up `a` until it sees `=`), and `f(x=1)`-for-`f(x==1)` typos
become errors. Update `compound_literal.luv`, `struct_with_initializer.luv`
and the README examples.

Other decisions here:

- **Compound literal = call on a type.** `TYPENAME '(' args ')'`.
  **Decided (via [D7](D07-keywords.md)):** no `T(x)` conversion syntax. Conversions are always
  `cast T(x)`, and bare `Name(...)` is only a compound literal.
- **Argument evaluation order** and promotion (`int_promotion_in_call.luv`)
  are semantic and outside the parser's scope.
