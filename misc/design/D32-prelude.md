# D32 Prelude

Written 2026-11-23, and revised after the first review. Steps 1 to 3 are
done. The first need is `str`. As a builtin type with a fixed layout
([D31](D31-aggregates.md)), each place that handles aggregates needs a
`TY_STR` case alongside structs and arrays: layout, size, alignment, leaves,
and its AggInfo. Defining `str` as an ordinary luv struct removes those
cases. The same place is where methods on builtin types and common functions
go later, which the builtins list in lex.c (len, max, range) is a stopgap
for.

[D22a](D22-methods.md) already says that builtin types get their methods
from a prelude module that's implicitly imported everywhere, written in
ordinary luv, and that `List` and `Dict` become ordinary structs in it.
[D23](D23-arrays-lists.md) refers to prelude functions such as
`Arena.current()`. This is how that prelude is loaded, and what goes in it
first.

## What it is

- `src/prelude.luv`, an ordinary luv source file. gen_ninja.py writes a
  header with it as a C array followed by `LEX_PADDING` zero bytes. C23
  `#embed` isn't used, so that recent compilers aren't required. Errors in
  it are reported as `<prelude>`.
- It's parsed as module 0, before the root module, by the same Parser
  that parses imports. TypeIds, symbols and AggIds are shared by every
  module, so its definitions are usable everywhere without copying.
- It's parsed in both the checking and the emitting build. It's a few
  dozen lines at first, so parsing it every time isn't measurable, but
  it's benched on dumbbench and betterbench when it's added.

At first it only contains:

```
struct str:
    *u8 _data
    int _len
```

The fields are only for the prelude's own methods. User code passes the
`str` itself, and uses `len(s)` for the length. The leading `_` marks them
as internal. A later rule could make `_` fields accessible only from the
struct's own module. That's one check at field access (the name's first
byte and the struct's module), and code in C is unaffected since it only
depends on the layout. Whether to enforce it is left for later.

Later:

- Methods on builtin types: `on str def int __len__(self)`, `__eq__`,
  and something like `__str__` for `print`. Methods can only be defined in
  their receiver type's module, so `str`'s methods have to be in the
  prelude anyway.
- `range`, over `int` only, so it isn't generic. It needs the iteration
  protocol first.
- `len`'s use of `__len__` for everything but fixed-size arrays (whose
  length stays a constant from the builtin).

`max` and `min` can't be written in the prelude as free functions, since
there are no free generic functions ([D21](D21-generics.md)). Untyped
constants don't change that. They let the `1` in `max(x, 1)` take `x`'s
type, but `max` still has to accept every numeric type. So `max` and `min`
stay builtins like `len`, or become methods on each numeric type in the
prelude (`x.max(1)`, where the untyped `1` takes the parameter's type).
- Generic containers (`List{T}`), once generic structs are instantiated
  ([D21](D21-generics.md)).
- The runtime's functions (`luv_print_i64` and so on) as `foreign`
  declarations, replacing the `RUNTIME_SYMS` X-macro.

Everything in the prelude is visible in every module, can't be imported
selectively, and shows up in every IL dump (see below). Only what has to
be there goes in it, such as methods on builtin types. Anything that can be
in an ordinary module should be.

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
  0 is `TYPE_FUNC` now. `type_is_struct` is
  `(type_is_created(t) || t.u == TY_STR) && kind == TYPE_STRUCT`. Checking
  only the kind is enough, but it costs 0.5 to 2.4% on the benchmarks,
  because it loads the TypeInfo of every builtin type.
- `str` is a keyword, so the prelude is allowed to write `struct str:`
  (only while parsing the prelude), which binds to `TY_STR`. In the
  `TK_STRUCT` case of the parser, that's accepting `TK_STR` in module 0,
  filling in the existing entry instead of pushing one, and skipping
  `declare_definition`, since a keyword is never looked up. `str` is the
  only keyword type that's a struct. `List` and `Dict` are TypeNames, so
  they're declared normally.

Making `str` not a keyword would mean renaming it `Str` to fit the naming
rules of [D4](D04-types-vs-values.md), which isn't worth it to avoid a
special case of a few lines.

The alternative to the fixed TypeId is to add the prelude's `str` as a new
TypeId and keep it in `Parser.ty_str`. That needs every `TY_STR` and
`ty_str` use (18 now) changed to a load, in some checking-build paths.

## Finding prelude names

Types that are keywords (`str`, `int`) don't need a lookup, and methods
are found on their type, not by name. That leaves free names like `range`
and `List`. The options are:

1. A second lookup in the prelude's `Module.globals` after the module's
   own globals miss. It's only on a miss, but every use of a prelude name
   is one.
2. A field on `Name` with the prelude's `GlobalInfo` for that name,
   replacing `Name.is_builtin`. Names are interned per `Ctx`, so it's the
   same in every module, and finding it is one load with no extra hash.
   But `Name` is 24 bytes with one byte of padding, and a `u32` field
   makes it 32.
3. Copying the prelude's globals into each module's `globals`. That's a
   cost per module, and it isn't needed.
4. A dense array indexed by NameId. NameIds are allocated in the order
   names are first interned, and the prelude is lexed before any other
   source, so every name the prelude defines has a smaller NameId than any
   name that appears first in user code. After the prelude is parsed,
   `ctx->names.num` is recorded as `prelude_name_end`, and
   `GlobalInfo prelude_globals[prelude_name_end]` holds each prelude
   global's info, with 0 for the rest (keywords, and names that are only
   locals or fields in the prelude). The lookup is
   `name.u < prelude_name_end ? prelude_globals[name.u] : 0`.

Proposal: 4. It has no hash, no field on `Name`, and `Name.is_builtin`
goes away. The array is a few hundred `u32`s. It depends on the prelude
being the first source that's interned, which has to be written down where
the prelude is loaded and where `prelude_name_end` is declared. Names that
the compiler interns itself before the prelude (keywords, `name_len`) are
also below `prelude_name_end`, which is harmless. An ASSERT that no other
module has been loaded when the prelude starts checks the order.

The order is locals, then the module's globals, then the prelude. Keywords
stay reserved.

### Shadowing

A prelude name or a builtin can be shadowed by a local, a parameter or a
module's own global, silently. Otherwise adding anything to the prelude could break
existing programs, and every prelude change would be a breaking change.
Go allows it for its predeclared names for the same reason, and Python for
builtins. The realistic cases are locals and parameters named `len`, `max`,
`min`, `range` or `sum`, and a user's own `List`, and whatever names are
added later.

Because assignment declares ([D6](D06-assignment-declares.md)), a name
that's found only in the prelude or as a builtin doesn't count as defined
when it's the target of `=`. `max = 5` declares a local `max`, as it does
now, instead of being an assignment to the builtin. In that scope, `max(a, b)`
is then an error, as in Python.

## Decided in review

- Shadowing prelude names is allowed silently (above).
- The source is embedded with a header generated by gen_ninja.py, not
  `#embed`.
- Allocating operations (2026-11-23). `a + b` on `str` and interpolated
  strings outside `print` make new strings, and under
  [D23](D23-arrays-lists.md) option B those go in the current arena.
  Generated code reaches it through [D26](D26-il.md)'s pinned register,
  which points at the thread's context block, with the arena as a field, so
  finding it is one load (`CTX` and a `LOAD`). A thread-local that the
  runtime owns and an implicit context parameter on every function (as in
  Jai and Odin) were the alternatives. The context block's layout could be
  a struct in the prelude.
- The prelude's structs get their AggInfo when the definition ends, so
  every IL dump starts with their `agg` lines. They're left in. Each one
  is a reminder that the prelude has a cost.
- Builtins that the compiler implements (`len`, `max`, `range`) have
  `GK_BUILTIN` entries in `prelude_globals`, which replaces
  `Name.is_builtin`. So they're found and shadowed the same way as names
  that the prelude defines.
- The prelude isn't in `Ctx.module_ids`, so it can't be imported, and a
  user's `prelude.luv` doesn't conflict with it. Its filename is
  `<prelude>`, which is what errors in it show.
- The prelude is `MODULE_PRELUDE` (0) and the root module is `MODULE_ROOT`
  (1). Imports are found relative to the root module's directory.
- `luv parse` doesn't count the prelude's bytes in its throughput, so that
  numbers stay comparable as the prelude grows. Its parse time is included.

## Steps

1. Loading the prelude, with nothing useful in it yet: embedding, module 0,
   lookup through `prelude_globals`, `<prelude>` in errors, and a bench.
   Done 2026-11-23. Cycles on the desktop were within noise (dumbbench
   +1.2% by median with overlapping ranges, betterbench -0.1%).
2. `struct str` in the prelude bound to `TY_STR`, and the `str` cases in
   the layout functions removed.
   Done 2026-11-23. Under EMIT, `str` parameters and locals now take the
   aggregate paths, and a string literal used as a value is still "isn't
   supported yet" until step 3.
3. `str` values as aggregates on top of that, redoing the change that was
   shelved for this. Most of its `str` special cases go away. What's left
   is string literals, which become a value with two stores into a
   temporary, and `print` of a `str`.
   Done 2026-11-23. The offsets of `_data` and `_len` are looked up by name
   once, at the end of the prelude's `struct str`.
4. Later: typed methods (`__len__`, `__eq__`, dispatch for `print`), then
   generic containers and the runtime's `foreign` declarations.
