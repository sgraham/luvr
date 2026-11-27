# D33 Iteration

Written 2026-11-24 to collect where the discussion is. The decisions are
in the next section, and the rest is the discussion behind them, with the
questions that are still open. [D30](D30-emission.md)'s slice was paused
before `for` over `range`, because `range` needs some form of the
iteration protocol. The emitting build currently rejects every `for`
("'for' isn't supported yet."), and the checking build declares loop
variables as `ty_unknown`.

The question is where to start. One option is the least work that makes
`for i in range(10)` run. The other is a general protocol that every
iterable uses, with fast integer loops added later as an optimization.

## Decided

2026-11-26, after methods were typed and emitted:

- **Approach 2, the protocol first**, with `bool __next__(self, *T out)`.
  Range and RangeIter go in the prelude, and every `for` calls `__iter__`
  and `__next__`. Baked cases come later, with benchmarks, once there's
  native code to measure.
- **A type with `__next__` is iterated directly**, without `__iter__`.
  Otherwise `__iter__` is called, and its result must have `__next__`.
- **A zero step gives an empty loop.** RangeIter works out the number of
  elements up front, and finds none for a zero step. That's reasonable
  behaviour, so there's no trap and no compile-time check.
- **The loop variable is visible after the loop** (function scoping, as
  in Python) and keeps the last element. If the loop doesn't run, it keeps
  the value it had before, which is zero for a variable that the loop
  declares.

## Leanings so far

These are leanings from discussion, not decisions.

- **Range is a value, and its iterator is a separate type.** `struct Range`
  holds `start`, `stop` and `step`. `__iter__` returns a `RangeIter` that
  holds the position, like Python's `range` and `range_iterator`. A Range
  can be iterated more than once, nested, printed and tested with `in`
  (`print_range.luv`, `range_in.luv`). Range can't be its own iterator,
  because calling `__next__` on it by hand would consume it.
- **`bool __next__(self, *T out)`.** Returns false when the iterator is
  exhausted, and otherwise writes the next element through `out`. The
  element type is the pointee type of `out`. Only `for` and comprehensions
  call it implicitly. There's no free `next(it)`.
- **Changing a collection's size while iterating over it is an error**, for
  every collection, as Python does for dicts and sets (Python lists are the
  exception, and see appended elements). Assigning to existing elements or
  keys is allowed. Iteration snapshots the data pointer and length. A
  debug build compares the length with the snapshot on each step and traps.
  A release build doesn't check, so a grow reads stale storage, the same
  hazard as `&xs[i]` in [D23](D23-arrays-lists.md). A list used as a queue
  needs a loop that pops or indexes explicitly. There's no `while`, so that
  depends on D20's conditional `for`.
- **Comprehensions use the same lowering as `for`** ([D10](D10-comprehensions.md)),
  so every case that's added covers both.

## The `__next__` signature

Python's `__next__` returns the value and raises `StopIteration` at the
end. Without exceptions, the end has to be signalled some other way.

| | `IterResult{T} __next__(self)` | `bool __next__(self, *T out)` |
|---|---|---|
| Needs generics | Yes. `IterResult{T}` is a generic struct, and the iterator's methods need it instantiated | No. Each iterator's `__next__` is concrete |
| Return | An aggregate per step. For `int` that's 16 bytes, two registers on SysV. A large `T` goes through a hidden result pointer, then a copy to the loop variable | A `bool` in a register. `T` is written in place |
| Loop lowering | Test `.ok`, copy `.value` | Pass the address of a hidden slot, test the result, load the slot into the loop variable |
| Several targets | `T` has to be a struct that `for` destructures | One out parameter per target, for example `bool __next__(self, *K key, *V value)` |
| Calling by hand | `r = it.__next__()`, then test `r.ok` | `int x` declared first, then `it.__next__(&x)` as the loop condition (D20) |

The hidden slot keeps the loop variable itself from being address-taken,
so it can still be promoted to a register ([D26](D26-il.md)). Only the slot
needs a load per step. The loop variable is visible after the loop
(`for_is_not_scope.luv`, [D6](D06-assignment-declares.md)), and it keeps the
last element, since nothing is loaded from the slot after `__next__` returns
false.

Several out parameters remove the need for tuples or [D5b](D05-decl-vs-expr.md)
multiple values just for `for k, v in`. It's open whether an iterator with
two out parameters can be used with one target.

## Does every iterable need `__iter__`?

Python's iterators have an `__iter__` that returns themselves, so that
`for` can be given either an iterable or an iterator. A rule in `for`
would remove that boilerplate: if the type has `__next__`, it's used
directly. Otherwise `__iter__` is called, and its result must have
`__next__`. This matters for methods that return an iterator, such as
`d.keys()`.

The iterator is a hidden local of the loop, initialized from `__iter__`'s
result, so `__iter__` returns an aggregate by value ([D31](D31-aggregates.md)'s
`RESULTAGG`). An iterator over a list holds the `*List{T}`, not a copy of
it.

## Approach 1: a counting loop for `range` only

D30 already describes this: `range(...)` as the whole loop expression
emits a counting loop. `start` and `stop` are evaluated once into slots.
A header block tests `i < stop`, a step block (the target of `continue`)
adds the step, and an exit block is the target of `break`.

Needs:

- Recognizing `range` in the `for` header. It's already a builtin symbol
  (`len`, `max` and `range` in parser.c).
- `break` and `continue` (`FLOW_ENDS`) (needed by every approach).
- A decision about step signs. A constant step chooses `<` or `>` when the
  loop is emitted. A non-constant step needs either a test of the sign on
  each step, or two copies of the loop with the test hoisted before them.
- A decision about a zero step. Python raises `ValueError` when the range
  is constructed. A constant zero could be a compile error, and a
  non-constant zero a trap.

Doesn't need method typing or emission, the prelude Range, generics or
aggregate returns. It makes `for_range.luv`, `for_break.luv`,
`for_continue.luv`, `for_is_not_scope.luv`, `for_with_return.luv`,
`for_uncond_return_range.luv` and `compr_simple_range.luv` runnable,
assuming the rest of their contents are supported.

Risks:

- It's tempting to add `for x in list` and arrays the same way before the
  protocol exists. Then the special cases define what iteration means,
  instead of the protocol.
- `r = range(10)` followed by `for x in r` doesn't match, since only the
  syntactic form does. That needs Range values, and either the protocol or
  a case keyed on the type instead of the syntax.

Very little of this is lost later. Under the "baked cases" option below,
this counting loop is exactly the code emitted for a RangeIter, as long as
it behaves identically to RangeIter's `__next__`.

## Approach 2: the protocol first

Every `for` looks up `__iter__` and `__next__` by name on the iterable's
type, and calls them.

Needs:

- Method typing and emission. The 2026-11-23 IL TODO survey found 21
  tests blocked on methods, 7 of them on `with`/`__enter__`, which is a
  similar compiler-called protocol.
- The compiler calling a method it found by name, with no call written in
  the source.
- Range and RangeIter in the prelude with their methods, and `range(...)`
  constructing a Range.
- Aggregate returns from `__iter__`.

What "everything works" covers is limited by the rest of the compiler:

- Lists are `*List{T}` ([D23](D23-arrays-lists.md)), and a list iterator is
  `ListIter{T}`. Both need generic instantiation, which isn't implemented
  ([D21](D21-generics.md)). Until then, lists aren't iterable whichever
  approach is chosen.
- Fixed arrays (`[4]int`) can't have a prelude iterator that's
  parameterized by their length, since D21 has no value parameters. An
  iterator that holds a pointer and a runtime length (`ArrayIter{T}`)
  works, but also needs generics.
- So in the near term, approach 2 makes Range and user-defined iterators
  work, and the rest waits for generics.

Performance: without inlining, every step is a call, and the element is
stored to memory and loaded again. A tight integer loop is probably
several times slower than a counting loop. For now, `luv il` output only
runs in a throwaway interpreter ([D30](D30-emission.md)), so this can't
be measured until there's native code. Compile speed isn't affected
noticeably, since there's one method lookup per `for`.

## Getting the speed back

Options, not exclusive:

- **A general inliner** in the fast tier, for small prelude methods, plus
  [D31](D31-aggregates.md)'s splitting of unescaped aggregate slots into
  scalars. This is the most general option and the most work. The prelude's
  IL is currently thrown away after each function is finished, so it would
  need to be kept for inlining.
- **Baked cases.** For a fixed set of prelude iterator types, `for` emits
  its own IL for that type's `__next__` instead of a call. User types still
  make real calls. The prelude's `__next__` stays the definition of what
  the loop means, so a baked case is only an optimization and can be
  removed. Proposed criteria for baking a case:
  1. The compiler already knows the type's layout: Range, List and fixed
     arrays (literals and comprehensions already build them, D23), and
     `str` ([D32](D32-prelude.md)).
  2. A step does very little work, so a call is a large fraction of the
     cost. True for Range, List and arrays. Not true for dicts, which scan
     slots and skip empty ones, and whose layout is only in the prelude.
  3. Each baked case is benchmarked against the protocol version. If an
     inliner arrives later, the cases that no longer win are removed.
- **Forms recognized in the `for` header**, which change more than the
  cost of a call:
  - A slice, `for x in xs[1:]`. Slices copy (D23 option B), so this avoids
    an allocation, which inlining wouldn't.
  - `for i, x in xs` for lists, arrays and `str`, in place of `enumerate`.

## Adapters

`enumerate`, `zip` and `reversed` can't be prelude functions, because D21
has no free generic functions. Each is either syntax or a method on each
container.

- `enumerate`: the `for i, x in xs` form above.
- `zip`: dropped. `for i in range(len(a))` covers two lists, and a general
  `zip` needs generics across two types.
- `reversed`: a negative step covers indices. `xs.reversed()` returning a
  prelude iterator could be added later.
- Dict views (`keys()`, `values()`, `items()`) and plain `for k in d`:
  prelude iterators going through the protocol, with `items()` using two
  out parameters.

## An order that keeps both options open

1. `break` and `continue`, needed by everything.
2. Approach 1's counting loop for `range(...)` in the header, specified as
   exactly what RangeIter's `__next__` will do: the arguments evaluated
   once, the step sign, zero step, and the loop variable's value after the
   loop.
3. Method typing and emission, which `with` needs as well.
4. Range and RangeIter in the prelude, and the protocol for every other
   `for`. The counting loop becomes the first baked case, now keyed on the
   type rather than the syntax, which also covers `for x in r`.
5. Lists, arrays and `str` after generic instantiation, as protocol
   iterators first and baked cases second, each with a benchmark.

The risk in this order is the same as approach 1's, of adding more special
cases in step 2 before step 4 exists.

## Open questions

- Whether an iterator with two out parameters can be used with one target.
- Which cases are baked, and whether a general inliner replaces them.
- Whether the size check during iteration is a length comparison, which
  misses a shrink followed by a grow back to the same length, or a
  modification counter in every collection.
- How this relates to D20 (a general `for` that also covers conditional loops).
