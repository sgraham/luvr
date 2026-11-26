# D22 Methods (`on`) and overloading

`on T def name(...)` defines a function looked up by (receiver type, method
name). The receiver's static type is known when `.name` is reached, so
lookup is one exact probe and nothing is ranked. This is overloading on the
first argument only, and it's kept to that.

Decided (2026-11-08):

- **Key.** A method on a generic type is keyed on the generic type itself:
  `on List{T} def len` is stored under (`List`, `len`). `xs.len()` with `xs`
  a `List{int}` probes (`List`, `len`) and instantiates it with `T = int`.
  Builtin compound types key the same way (`[]T` under slice, `{K}V` under
  dict).
- **No specialization.** `on List{int}`, `on []u8` etc. are errors. The type
  arguments of a receiver are always new type parameters, so it doesn't
  matter whether their names match the struct definition, and the question
  of whether `T` refers to an existing type doesn't come up. Specialization
  is rarely needed for restricting methods to some types: a method body is
  only type-checked when it's instantiated, so `on List{T} def T sum(self)`
  works for numeric `T` and is an error only when called on `List{str}`.
  Type-specific operations like `decode_utf8` are free functions.
- **No methods on pointers** (`on *T`), because `.` auto-dereferences, so
  `p.f()` would be ambiguous. No methods on `[N]T` (would need a value
  parameter, [D21](D21-generics.md)) or `typeof(...)`.
- **Methods can only be defined in the receiver type's own module** (option
  (c) below). `on mod.Thing` is a syntax error. Dunder methods are
  therefore also only defined with the type, so a module can't change how
  another module's type prints. `memfn_on_basic_type.luv` (a method on
  `int` in user code) was removed.
- **No free-function overloading.** Methods on a generic struct cover
  "same code, many types", and methods on different types cover "different
  code per type". Method names don't enter
  module scope, so a free `free` and a method `Stuff.free` coexist
  (`memfn_shadow_foreign_name.luv`).
- **Static methods**: a method whose first parameter isn't a bare name has no
  `self`. `on List{T} def List{T} new(int cap)` is called as
  `List{int}.new(4)`. This is how a function that would have been a free
  generic function is written ([D21](D21-generics.md), 2026-11-13).
- **Operators** on structs map to dunder methods (`<` to `__lt__`) through
  the same lookup. That's a spelling for `<` on that type, not overloading.
- **Errors inside instantiations** must stay readable. Report the error in
  the generic body with the instantiating call site and type arguments, and
  avoid deep "instantiated from" chains.

Receiver grammar enforced by the parser:

```
receiver -> TypeName [ '{' TypeName { ',' TypeName } '}' ] | builtin
          | '[' ']' TypeName | '{' TypeName '}' TypeName
```

Options considered for where methods can be defined:

- (a) Methods on a struct only in its module. Methods on builtin types
  anywhere, visible only where the defining module is in scope.
- (b) Anywhere, one global key, an error when two conflicting definitions
  are visible.
- **(c) Only in the type's own module. Builtin types get methods only from
  the module that defines them.** Chosen.

D22a (open): what (c) means for builtin types. The "module that defines
builtin types" is a prelude module that's implicitly imported everywhere.
Builtin receivers (`on int`, `on str`, `on []T`, `on {K}V`) are accepted by
the parser but are a semantic error outside the prelude. The prelude is
ordinary luv code. What's special is only that it's the home module of the
builtin types, plus whatever intrinsics or `foreign` declarations it needs
for primitive operations. Other library modules can't add methods to `str`
either, and provide free functions instead. If `[]T` and `{K}V` are later
replaced by generic structs (`List{T}`, `Dict{K, V}`, see [D21](D21-generics.md)), those become
ordinary structs defined in the prelude, and only true primitives (`int`,
`str`, ...) are left needing the special case.

Methods on primitives are one definition per type, because `on T` isn't a
receiver: `on int def int min(self, int b)`, `on u64 def u64 min(...)` and
so on, in the prelude. `a.min(b)` and `(3).min(b)` parse already. The
static form `int.min(a, b)` doesn't, because a builtin type can't start an
expression.

## When methods are typed

Noted 2026-11-25 ([D34](D34-constant-data.md) step 6b). A field of a
parameter is immutable when the parameter is a known struct, but `self`
has no type until methods are typed, so a field of a parameter whose type
isn't known is still assignable (`self.length = self.length + 1` in
`manual_list_int.luv` and three other method tests). This is the
`through_pointer` case in postfix field access. Since `xyz.f(5)` is
`Xyz.f(&xyz, 5)`, `self` will be a `*T`, so its fields are assignable
through the pointer, and the unknown-type case can be removed.

## The type of self

Decided 2026-11-26. `self` is a pointer for receivers that can be modified
in place, and a value for those that can't:

- Structs (and List, Dict, and other containers): `*T`, since `xyz.f(5)` is
  `Xyz.f(&xyz, 5)`. Methods can assign to fields, and large structs aren't
  copied for each call.
- Numbers, `bool`, and `str`: `T`. A pointer can't be used to modify them, so
  it would only add a store and loads. A value receiver also means that
  rvalues like `(a + b).min(c)` and `"\n".join(parts)` don't need a
  temporary.

The rule is by type, so there's no syntax for choosing a value receiver.
Since methods on builtin types are only defined in the prelude, user code
only ever sees `*T`. The case that loses something is a small read-only user
struct (such as a `Vec2`), which would be slightly cheaper by value. That's
left to optimization (or builtin SIMD types) rather than a language choice.

## Fields and methods share names

Decided 2026-11-26. A method can't have the same name as a field of its
receiver, and it's an error at the method's definition. Struct definitions
come before their methods, so it's one field lookup. Keeping fields and
methods in separate namespaces (`s.len` the field, `s.len()` the method) was
considered, but it's mostly useful for getters, which the `_field`
convention already covers. It would also make a function-pointer field
uncallable as `s.f()`, and it's confusing to read.

## Receivers that can't be modified

Decided 2026-11-26. A method can only be called on an assignable value
or through a pointer. Calling one on a parameter or a constant is an error,
in the same way that assigning to them is, because `self` is a pointer and
the method could modify them through it. The workaround is an explicit copy
(`copy = stuff`, then `copy.f()`), or taking a `*T` parameter.

This also means that a struct parameter's memory is never written while the
callee runs, so it can be passed by reference without a defensive copy, and
loads from it can be reused across calls. Copying into a temporary for each
call would hide the cost, and passing the address anyway would be unsound
(constant data, or the caller's memory).

Possible relaxation: methods are defined before use, so whether a method
modifies self (assigns through it, takes `&self`, passes it as a `*T`, or
calls a method that does) is known at every call. Read-only methods could
then be called on parameters and constants. The cost is that changing a
method's body to modify self would break its callers without any change to
its signature. It's left until the restriction is shown to be a problem
(getters on parameters, like `r.width()` in `def int area(Rect r)`).
