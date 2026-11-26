# D36 Compilation units (open)

Written 2026-11-26 to collect a discussion. Nothing here is decided. It came
up when calls to functions in imported modules were first emitted, and two
modules both defining `helper_func` gave two IL functions with the same
name.

The front end is meant to be fast enough that compiling the whole program
every time is fine. But the IL shouldn't assume that every function ends up
in one file, so this note is about what a unit of compiled output is, and
what linking units would involve.

## What's whole-program now

One `Parser` and `Ctx` parse the root module and everything it imports
([D24](D24-modules.md)). Functions are passed to `Ctx.on_func` one at a
time, but every kind of number in the IL is shared by the whole Ctx:

- Function symbols are indexes in `Ctx.syms`.
- Data is `@.datN`, an index in `Ctx.data`.
- Aggregate layouts are `#N`, an index in the Ctx's aggregate table.

So `luv il` prints one unit, and splitting it later means changing how
those numbers are assigned, not only where they're printed.

## Options

### A. Whole program, one unit

What exists now. Every build parses, checks and emits everything that's
reachable from the root.

- Simplest, and inlining across modules needs nothing special.
- Nothing below the whole program can be cached or compiled in parallel.
  Once there's a backend, its time grows with the program, and it's
  expected to be much slower than the front end
  ([D25](D25-backend.md)).
- Jai works this way, and Zig with its single compilation unit.

### B. Separate compilation per module

Each module is compiled by its own run of `luv` into its own unit. An
importer needs the declarations of what it imports.

- The checking build ([D30](D30-emission.md)) is a cheap way to get them.
  An importer parses its imports with `EMIT` 0, which gives types,
  signatures and constants, and emits IL only for its own module. At the
  front end's speed, parsing imports again in every unit costs little.
- A generic struct instantiated in several units is emitted in each of
  them, and duplicates have to be removed when linking.
- Inlining across units needs a copy of the callee's IL in the caller's
  unit.

### C. Whole-program front end, a unit per module

Parse and check everything once, but emit each module's IL into its own
unit, with its own numbering and references to other units by name.

- The front end stays whole-program, which its speed allows. The backend,
  where caching and parallelism matter, gets units that can be cached by a
  hash of their contents and compiled on separate threads. Rust's codegen
  units are the closest model.
- Moving to B later only changes where an importer's declarations come
  from, not the format of a unit.
- Leaning: C, keeping B possible.

## What units and linking need

These apply to B and C alike.

1. **Function symbols qualified by module**, as in `@impsub.helper_func`, or
   `@sub/util.helper` for a module in a subdirectory, using its canonical
   name ([D24](D24-modules.md)). The root module's functions and the
   runtime's stay unqualified (`@main`, `@luv_print_i64`). Every option
   needs this as soon as two modules can use the same name, even A. Open:
   whether a symbol holds its module, or is interned as one qualified name.
2. **Data that's local to a unit.** `@.datN` would be private to its unit.
   A global variable that another module reads would become a named,
   exported data symbol (`@impsub.counter`).
3. **An aggregate table per unit.** Each unit declares the layouts it uses.
   Two units with the same layout is harmless, since a layout is only a
   size, an alignment and leaves, and the ABI step only needs the copy in
   its own unit.
4. **Declarations of functions in other units**, such as
   `extern func i64 @impsub.helper_func(i64)`, so that the ABI step and the
   test interpreter can check a call without seeing the callee.
5. **No initialization order.** Global initializers are constants
   ([D34](D34-constant-data.md)), so there's nothing to run before `main`
   and no order between modules to decide.

Linking IL is then concatenating units, resolving names, dropping
duplicates of link-once symbols (below), and renumbering what's local to
each unit. That's a few hundred lines.

It's only needed in two places, though. If units go through the backend
to object files, or into the JIT ([D25](D25-backend.md)), the machine code
is linked by the system linker or the JIT, not as IL. IL is linked only for:

- The test interpreter, which can load several unit files and resolve
  calls by name.
- Optimizing across units. Inlining is the case that matters. Fast `for`
  loops may need small prelude methods inlined into the caller
  ([D33](D33-iteration.md)). Across units, that needs either IL linked
  before optimizing, or a copy of a small function's IL in each unit that
  calls it, like LLVM's `available_externally`.

## Generic instantiations

The worry is a repeat of C++ templates: the same instantiation compiled
in many units, then thrown away at link time.

- With C, the front end sees every instantiation in the program before any
  unit is finished, so each one can be assigned to exactly one unit, for
  example the unit of the module that first uses it, or one unit just for
  instantiations. Nothing is emitted twice, and nothing has to be removed
  when linking.
- With B, each unit only knows its own uses, so duplicates are unavoidable
  without a shared cache. They'd be link-once symbols with deterministic
  names (`@List{i64}.append`). With our own JIT or linker, removing one is
  a hash table lookup. With the system linker, it's COMDAT sections, which
  every format supports.
- Much of the C++ pain comes from other things: parsing the same headers
  in every unit, code bloat from many instantiations, and ODR violations
  when two units' copies differ. The first doesn't apply, since parsing is
  cheap. Identical copies are guaranteed if an instantiation's IL depends
  only on the generic and its type arguments, which [D21](D21-generics.md)'s
  struct-only generics should allow.

## Open

- A, B or C, and when. Nothing forces a decision until there are more
  modules (a standard library) or a backend whose time matters.
- How a symbol records its module (point 1 above).
- Which unit an instantiation is assigned to under C.
- How `luv il` prints several units: one file per unit, or one file with a
  `unit` line before each.
- Whether exported data symbols should be qualified the same way as
  functions.
