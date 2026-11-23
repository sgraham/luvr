# D32 Prelude (proposal)

Written 2026-11-23, for review before any code. The first need is `str`. As
a builtin type with a fixed layout (D31), each place that handles
aggregates needs a `TY_STR` case alongside structs and arrays: layout,
size, alignment, leaves, and its AggInfo. Defining `str` as an ordinary
luv struct removes those cases and gives `s.data` and `s.len` as fields.
The same place is where methods on builtin types and common functions go
later, which the builtins list in lex.c (len, max, range) is a stopgap for.

D22a already says that builtin types get their methods from a prelude module
that's implicitly imported everywhere, written in ordinary luv, and that
`List` and `Dict` become ordinary structs in it. D23 refers to prelude
functions such as `Arena.current()`. This is how that prelude is loaded, and
what goes in it first.

## What it is

- `src/prelude.luv`, an ordinary luv source file. The build turns it into
  a C array with `LEX_PADDING` zero bytes after it. That could be done by
  gen_ninja.py writing a header, or by C23 `#embed` if every compiler
  that's used supports it. Errors in it are reported as `<prelude>`.
- It's parsed as module 0, before the root module, by the same Parser
  that parses imports. TypeIds, symbols and AggIds are shared by every
  module, so its definitions are usable everywhere without copying.
- It's parsed in both the checking and the emitting build. It's a few
  dozen lines at first, so parsing it every time isn't measurable, but
  it's benched on dumbbench and betterbench when it's added.

At first it only contains:

```
struct str:
    *u8 data
    int len
```

Later:

- Methods on builtin types: `on str def int __len__(self)`, `__eq__`,
  and something like `__str__` for `print`. Methods can only be defined in
  their receiver type's module, so `str`'s methods have to be in the
  prelude anyway.
- `range` and `max`, and `len`'s use of `__len__` for everything but
  fixed-size arrays (whose length stays a constant from the builtin).
- Generic containers (`List{T}`), once generic structs are instantiated
  (D21).
- The runtime's functions (`luv_print_i64` and so on) as `foreign`
  declarations, replacing the `RUNTIME_SYMS` X-macro.

## Builtin types keep their fixed TypeIds

`Parser.types` already has unused entries for the `FixedType`s. The
prelude's `struct str` fills in `types.items[TY_STR]` instead of adding a
new type. So:

- `ty_str` stays a constant, and none of the `TY_STR` checks in the
  checking build change or get slower.
- `type_info(p, ty_str)` is a real struct with fields, an AggId and an
  image. `type_has_layout`, `type_size`, `type_align`, `type_agg_leaves`
  and `type_agg` lose their `str` cases, and `type_is_aggregate` doesn't
  need one.
- `TypeKind` needs a `TYPE_NONE` as 0, since unused entries are zeroed and
  0 is `TYPE_FUNC` now. `type_is_struct` checks the kind rather than
  `type_is_created`.
- `str` is a keyword, so the prelude is allowed to write `struct str:`
  (only while parsing the prelude), which binds to `TY_STR`. Any other
  builtin that gets a luv definition later can do the same.

The alternative is to add the prelude's `str` as a new TypeId and keep
it in `Parser.ty_str`. That needs every `TY_STR` and `ty_str` use (18 now)
changed to a load, in some checking-build paths.

## Finding prelude names

Types that are keywords (`str`, `int`) don't need a lookup, and methods
are found on their type, not by name. That leaves free names like `range`,
`max` and `List`. The options are:

1. A second lookup in the prelude's `Module.globals` after the module's
   own globals miss. It's only on a miss, but every use of a prelude name
   is one.
2. A field on `Name` with the prelude's `GlobalInfo` for that name,
   replacing `Name.is_builtin`. Names are interned per `Ctx`, so it's the
   same in every module, and finding it is one load with no extra hash.
3. Copying the prelude's globals into each module's `globals`. That's a
   cost per module, and it isn't needed.

Proposal: 2. The order is locals, then the module's globals, then the
prelude, so a prelude name can be shadowed by a module's own definition,
as Go allows for its predeclared names and Python for builtins. Keywords
stay reserved.

## Open questions

- Allocating operations. `a + b` on `str` and interpolated strings outside
  `print` make new strings, and under D23 option B those go in the current
  arena, a dynamically scoped special variable that `Arena.current()`
  reads. What's still open is how the generated code reaches it: a
  thread-local that the runtime owns, or an implicit context parameter
  passed to every function (as in Jai and Odin), whose struct the prelude
  would define. That decides how `__add__` on `str` and its IL are
  written, so it's worth settling before the first allocating operation.
- Whether shadowing a prelude name should be allowed silently, warned
  about, or be an error.
- How the source is embedded: a generated header, or `#embed`.
- The prelude's structs get their AggInfo when the definition ends, so
  every IL dump starts with their `agg` lines. That's harmless, but the
  lines could be printed when they're first used instead.

## Steps

1. Loading the prelude, with nothing useful in it yet: embedding, module 0,
   lookup through `Name`, `<prelude>` in errors, and a bench.
2. `struct str` in the prelude bound to `TY_STR`, and the `str` cases in
   the layout functions removed.
3. `str` values as aggregates on top of that, redoing the change that was
   shelved for this. Most of its `str` special cases go away. What's left is string literals, which become a
   value with two stores into a temporary, and `print` of a `str`.
4. Later: typed methods (`__len__`, `__eq__`, dispatch for `print`), then
   generic containers and the runtime's `foreign` declarations.
