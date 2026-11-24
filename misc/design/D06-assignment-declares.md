# D6 ★ How plain `x = 1` introduces a variable (and scope granularity)

This is semantic, but it determines whether a single-pass parser can work
at all.

What the tests show:

- `create_local_over_global.luv`: assigning to a global's name inside a
  function creates a local.
- `for_is_not_scope.luv`: `for` doesn't open a scope. Scope is
  function-level, Python-style.
- `scopes.luv`: `global x` opts out.

Options:

- **(a) Python's rule: "assigned anywhere in the function → local
  everywhere in it".** This needs to see the whole body before resolving
  the first use, so it's **not single-pass**.
- **(b) Declare at first assignment, visible from that point on** (Lua's
  `local` without the keyword). Single-pass friendly. It is what luv60
  currently does: `assign_global.luv` prints the global, then assigns,
  which wouldn't work under rule (a).
- **(c) Explicit declaration operator** (`x := 1` declares, `x = 1` only
  assigns). LL(1) via the token, catches typos (`cuont = 1`), and is
  single-pass. Less Python-ish.

**Recommendation: (b)**, documented as an intentional difference from Python.
Keep (c) in mind if typos start biting. With function-level scope, the Lua
`block()` → `removelocalvars` step (`lparser.c`, `block`) becomes a no-op
inside a function. Lua's `actloc` stack only gets pushed per function and per
comprehension ([D10](D10-comprehensions.md)).

A global function's name is shadowed the same way (2026-11-24): `f = 5` in
a function declares a local `f`, and so does `for f in xs`, as for global
variables and prelude names ([D32](D32-prelude.md)). Functions aren't values,
so `f = g` is still an error at `g`.
