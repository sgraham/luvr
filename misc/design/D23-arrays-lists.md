# D23 Arrays, lists and slices (open)

Paused on 2026-11-12 to think through, because how this memory is managed,
and what the user has to do with it, will change later decisions and syntax.

The questions:

- What `a = [1, 2, 3]` is. In luv60 it's a fixed array `[3]i32`, so
  `a.append(4)` is an error ("Undefined member function append on type
  [3]i32.", `array_no_append.luv`). An owning list needs a declared type
  (`[]int nums = [1, 2, 3]`). The user wrestled with this in luv60 without a
  satisfying answer.
- What a slice `a[i:j]` ([D8](D08-expressions.md)) is: a copy, or a view into `a`. In luv60 it
  was a view of some kind. `list_slice_append.luv` expects `nums[2:]` of a
  `[]int` to fail a runtime check on `append`.
- What a comprehension produces, and from what. `[x*2 for x in xs]` over an
  array, a list, a slice or `range`. The existing errors
  (`compr_array_from_non_array`, `compr_filter_array`,
  `compr_array_from_diff_sized_array`) show where luv60 drew the boundary
  between arrays and lists.
- Who owns each of them and how they're freed. There's no decision on
  memory management at all yet. Go can hide the difference between an
  owning list and a view partly because it's garbage collected.
- The spelling. `[]T` was chosen to look like Python, so it owns its storage
  like `std::vector`. In Go, Zig and Odin `[]T` is a view (a slice), which
  may confuse readers, and a view type is needed regardless.

What's agreed so far:

- The compiler is closely tied to the implementations of `List` and `Dict`,
  because of literals and comprehensions. It knows `List{T}`'s layout and
  builds literals and comprehensions directly, and preallocates when the
  length is known. The methods (`append`, growth, dunders) are luv code in
  the prelude ([D22a](D22-methods.md)). `Dict` literals probably call a prelude insert
  function rather than the compiler knowing the hash table.
- `[]T` and `{K}V` are transparent synonyms for `*List{T}` and
  `*Dict{K, V}`, where `List` and `Dict` are ordinary prelude structs (see
  "Values and references" below). The compiler knows about them only to
  make code less noisy. Diagnostics print the short form. This replaces the
  inclination in [D21](D21-generics.md) to replace them with verbose synonyms.
- Fixed arrays `[N]T` stay primitive value types.

## Option A (explored 2026-11-12): regions, a current pool and scratch space

This is one coherent option, written down so that the details can be worked
through. Nothing in it is decided. It started from Goose
(aardappel.github.io/goose), which keeps each growable array alone at the top
of its own bump-allocated stack, so it grows in place and never moves, and
frees by resetting the stack when a scope exits. Goose assigns stacks and
checks lifetimes with whole-program analysis. That doesn't fit a single-pass,
per-file compiler ([D1](D01-single-pass.md)), so this option replaces both analyses with things that
can be done at runtime or checked inside one function.

**Regions.** A region is a large reservation of address space (Goose uses
2 GB) with a guard area at the end and a bump pointer. Each thread keeps a
free list of regions. Taking or returning one is a pointer pop or push and a
reset of the bump pointer, and a reused region already has committed pages.
Regions are always released in reverse order of acquisition, so the free
list plays the part of Goose's compiler-assigned stacks without any analysis.

**Lists in locals own a region.** A list whose lifetime is a local variable
(`[]int xs = []`, `ys = [x*2 for x in xs]`) takes a region when it's created
and gives it back when the function returns.

- It's the only thing in its region, so `append` is a store and a bump, with
  no capacity check and no reallocation. A list never moves, so growing it
  never invalidates a slice or reference into it.
- It doesn't use the current pool (below), because that may live much longer
  than the function. A function that's called a million times would
  otherwise leak a list per call until the pool ends.
- Returning a list hands its region to the caller, which takes no copy, no
  hidden destination argument and no return-value optimization. This matters
  for a single-pass compiler, which can't know at `[]str out = []` that `out`
  is the value that will be returned.
- Each region has one owner, so `ys = xs` is a move (`xs` can't be used
  afterwards, checked locally), with an explicit `copy(xs)` for two lists.
  Copy-on-assignment is the alternative.

**Annotations, when the user knows more.** Regions remove the usual reason to
annotate (reserving capacity to avoid reallocation), so annotations only
change where storage lives:

| what the user knows | syntax | where it lives |
|---|---|---|
| nothing (the default) | `[]int` | its own region, grows in place |
| never more than N | `[:64]int` | the frame, or inline in a struct, aborts past N |
| the size when it's made | `[n]int` (runtime `n`) | the frame, or a region if large |
| a compile-time size | `[4]int` | a value, as now |

`[:N]T` reuses slice syntax, where `a[:N]` means "up to N", and is LL(1)
because `:` can't start an array size expression. `[n]T` needs no grammar
change, only relaxing the rule that the size is a constant. All of these
should convert implicitly to the view type, so that functions that only read
take a view and don't care which kind they were given. That makes the
spelling of the view type more pressing, because it's the type that would
appear most often in function signatures.

**The current pool.** Allocations whose lifetime isn't a local's go into the
current pool. That includes objects behind pointers, strings and lists
stored inside structs or other lists, and anything a callee builds that the
caller keeps inside another structure. The current pool is a per-thread,
dynamically scoped variable, like a Common Lisp special variable, and callees
see the binding of their caller. It could be a thread-local pointer that's
saved and restored around a block, or a hidden argument. At program start it's
a pool that lives until exit, so simple programs work without thinking about
memory, and long-running code opts into narrower scopes.

```
with Pool() as p:          # makes a pool, makes it current, frees it at the end
    tree = parse(text)     # parse()'s allocations go into p

into p:                    # makes an existing pool current, doesn't free it
    names.append(make_name(i))
```

`with Pool()` owns its pool and `into p` borrows one. A plain `with p:` can't
tell its `__exit__` whether to free, so borrowing needs its own statement.
`in` is already membership and part of `for`, hence `into`. [D3a](D03-layout-tokens.md)'s one-line
suites cover single statements (`into p: xs = load(f)`), and function-level
scope ([D6](D06-assignment-declares.md)) keeps `xs` visible after the block. Explicit placement methods
(`p.new(...)`, `p.list{T}()`) were considered and aren't wanted.

**Checking lifetimes inside one function.** Every view and pointer
expression has a root, tracked in the expression descriptor. A root is a local
owner, a parameter, static data, or a pool. Inside a function, "the current
pool" is either the innermost enclosing `with`/`into` in that function, which
is visible in the source, or otherwise the caller's current pool, which acts
like an implicit parameter.

- Values allocated into the current pool, including results of calls made in
  that block, get its root.
- A function may return values rooted in its parameters or in the caller's
  current pool. It may not return values rooted in a local or in a `with
  Pool()` inside itself.
- A value can be stored into a structure only if its root lives at least as
  long as the structure's. Two different unknown roots are conservatively an
  error. That's why `make_name(i)` above is made inside `into p:`, so its
  result is rooted in `p`.
- A call that returns a view is treated as returning something rooted in all
  of its view arguments, like Rust's lifetime elision but with no syntax for
  exceptions. No function body needs to be seen, only signatures, which the
  [D11](D11-forward-refs.md) skim provides for forward references.
- Slices keep their own length, and lists never move, so a slice of a list
  that later shrinks reads stale values of the right type, not freed memory.

**Scratch space.** Two cases use the same mechanism, a pool that's reset when
something ends, with only the result surviving.

1. *Statement temporaries.* A call whose result isn't assigned to a variable,
   returned or stored is a temporary, for example `print "x is \(x)"`, or
   `n = count(split(s))` where `split`'s result is only read by `count`.
   Such calls run with the current pool set to a per-statement scratch
   region, which is reset at the end of the statement. So all of the callee's
   allocations, including its internal garbage and its result, are discarded
   without bookkeeping. Calls whose result is kept run with the normal
   current pool.
2. *Declared scratch blocks.* The user marks a region of code whose
   allocations are temporary. Only the results are kept.

   ```
   []str top = []
   with Pool():
       words = split(text)
       counts = count_words(words)
       top = most_common(counts, 10)
   # words and counts are gone. top was copied out.
   ```

   The rule is that nothing allocated in a `with Pool()` block survives it,
   except by being copied out through a variable declared before the block.
   An assignment to such a variable inside the block copies the value's owned
   data (lists, strings, pointed-to objects) into the outer current pool.
   Variables first assigned inside the block die with it, and a later use is
   an error ("'words' was in scratch space released at line N."). This is
   checkable in one pass: at the end of the block, its locals are marked dead,
   as a scope is popped. A function can wrap its own work this way and return
   only its result, so the caller's pool gets the result and none of the
   garbage. Whether a separate spelling (`scratch:`, `with Scratch():`) is
   worth having is open.

**What's still open in this option:**

- Nested ownership: `[][]int`, structs that contain lists, lists of owned
  strings. Inner parts either go in the current pool or each own a region,
  and the latter doesn't scale to millions of small lists. `[:N]T` helps for
  bounded inner data.
- Whether a list declared inside `into p:` comes from `p` or still owns a
  region.
- How a single object is allocated without placement methods: `new(Node(...))`,
  or `&Node(...)` allocating in the current pool (it's an error today).
- What exactly copying out a value copies, especially views inside it whose
  roots are outside the scratch block.
- How strings are represented (owned bytes, views, or both), since built
  strings are the most common allocation.
- Address space: 2 GB regions allow about 64K live regions in a 47-bit
  address space, so deep recursion that keeps a list in every frame could run
  out. Smaller reservations, or Goose's rule against keeping growable data
  across a recursive call, would help.
- The program-lifetime default pool grows without limit in long loops that
  don't use `with Pool()`. Allocation statistics in debug builds would make
  that easy to spot.
- One thread-local read per allocation, which a function can load once.
- The current pool could grow into a context (temporary pool, logger), or
  user-defined special variables. Starting with only the pool keeps that
  open.

Option A became complicated, both to implement and as a model for the
programmer. In particular, copying a result out of a scratch block isn't
well defined once the result contains pointers. That led to option B.

## Option B (preferred, 2026-11-12): one current arena

Every allocation goes into the current arena. There are no per-local regions,
no moves and no copying out. This is close to where luv60's thinking had got
to, stated more clearly, and it suits how early the language is.

The model for the programmer is one rule: everything allocated lives until
the arena that was current when it was allocated is reset, and arenas are
blocks.

```
with Arena():          # a new arena from the free list, current for this block and its callees
    ...                # everything allocated in here dies at the end of the block
into a:                # makes an existing arena current
    ...
```

- The current arena is a per-thread, dynamically scoped variable, as in
  option A, and starts as an arena that lives until exit. `with Arena()`
  takes an arena from the free list and releases it at the end of the block.
  `into a:` borrows one. [D3a](D03-layout-tokens.md)'s one-line suites give a per-statement form
  (`into a: xs = load(f)`).
- Returning a value needs nothing. A callee allocates into its caller's
  arena unless it opens its own, so what it returns already lives there.
- Pointers between allocations are unrestricted within an arena, and from a
  shorter-lived arena to a longer-lived one. Nothing is ever copied out, so
  the question of what copying a pointer means doesn't come up.
- Lifetime checking is one local rule: a value allocated inside a `with
  Arena()` block can't be stored in, or returned through, anything that
  outlives the block. Roots are "the caller's arena" or a block's arena, and
  both are visible in the function. Leaving it unchecked, with debug builds
  poisoning a reset arena, is the fallback.
- The compiler uses the same machinery for its own allocations (the arena
  task for the compiler itself). Odin and Jai have essentially this model,
  with an allocator in an implicit context.

The caller's arena is named with `Arena.current()`, a prelude function that
reads the special variable. That's enough for the usual pattern of a scratch
arena plus an output arena:

```
def []str top_words(str text, int n):
    out = Arena.current()
    with Arena():                  # scratch
        counts = count_words(split(text))
        into out:
            return most_common(counts, n)
```

`with Arena()` always takes a fresh arena from the free list, so the scratch
arena can never be the same arena as `out`. That's a known pitfall when this
pattern is written by hand.

Costs, which are the next thing to work on:

1. Garbage accumulates until a reset. A function called in a loop leaves its
   temporaries in the caller's arena unless it wraps its work in `with
   Arena()`.
2. A list in a shared arena grows in place only while it's the most recent
   allocation. Otherwise it doubles and copies to the top, like
   `std::vector`. That's amortized O(1), but the old copies are garbage until
   the reset (typically about the final size again). A list can also move, so
   a pointer to an element taken before an `append` refers to the old copy.
   That memory is still mapped, so reads are stale rather than crashing, but
   writes through it are silently lost.
3. Peak memory depends on the programmer scoping temporary work. A long loop
   without a `with Arena()` just grows.

What it doesn't cost: allocation is still a bump, locality is often better,
and freeing is still free.

Storage annotations go on top of option B as opt-ins, rather than being the
default as in option A. `[:N]T` (bounded, inline) and `[n]T` (sized once at
runtime) carry over unchanged. A declaration could also opt a list into
option A's own region, so that a big, growing list never moves and leaves no
copies behind. The spelling is open. The statement-temporaries rule from
option A (calls whose result isn't kept run in a per-statement scratch arena)
also carries over unchanged, because it's mechanical and needs no copying
out. Whether to keep it is a separate choice.

Follow-ups agreed for option B (2026-11-12):

- Garbage until a reset is accepted. Automatically wrapping small pieces of
  list and comprehension work in a `with Arena()` was considered, but it
  needs escape analysis, and realistic code calls helper functions, which
  would have to be assumed to let everything escape.
- A list grows in place when it's still at the top of its arena. The check
  is only on the growth path (`len == cap`): whether the end of the list's
  storage equals the arena's bump pointer, which is a load and a compare.
- A local rule against appending to a list while a pointer or slice into it
  is still used was rejected, because a callee that does the append isn't
  covered. Stale pointers after a move are left to debug tooling instead:
  filling the old copy with a pattern, guard pages, or integrating with
  AddressSanitizer by poisoning the old copy and reset arenas with its
  manual poisoning interface.
- Per-arena high-water marks with the allocation sites that used the most,
  and an optional arena limit (`with Arena(.limit=...)`) that aborts past
  it, belong with the developer tools. That's around the same time as the
  formatter, alongside profiling tools for memory and performance, whose
  main job would be noticing a bad peak.
- Storage annotations: the user's real interest is assigning large blocks of
  memory to a subsystem. That isn't clear enough yet and is too early to
  design. `[:N]T` and `[n]T` remain as recorded above.

**Decided under option B (2026-11-12): everything is a list, and slices
copy.**

- `a = [1, 2, 3]` is a list (`[]int`), not a fixed array. A list costs a
  bump allocation and a header, so there's no longer a reason for literals to
  default to fixed arrays, and `a.append(4)` works. A fixed array is chosen
  by declaring one: `[3]int a = [1, 2, 3]`. `array_no_append.luv` no longer
  describes an error and changes when type checking arrives.
- `a[i:j]` makes a new list in the current arena, as in Python. Slicing
  never creates a view into a list's storage, so growing a list can't
  invalidate a slice.
- A comprehension produces a new list in the current arena. Into a declared
  array type (`[4]int all = [...]`) it fills the array and checks the size,
  which is what the `compr_array_*` error tests describe.
- Passing a list to a function passes the list itself, as in Python, so a
  callee's `append` is visible to the caller, because `[]T` is a pointer
  (see "Values and references" below).
- The remaining hazard is an element pointer (`&xs[i]`) taken before the
  list grows. That's the same rule as `std::vector`, and it's left to the
  debug tooling, or to a list with its own region.
- Views are deferred. If they're wanted later, they'll be a separate type
  (`View{T}` or `Slice{T}`). Whether that needs syntax, and how views
  interact with arrays, lists and strings, is open. Views are safe for
  storage that can't move (fixed and bounded arrays, and strings if they're
  immutable once built), which is a likely place to start.

**Decided under option B (2026-11-12): strings.**

- `str` is immutable UTF-8 bytes, `{ptr, len}`, with the length in bytes.
  The bytes are in static data, the source buffer or an arena, and never
  change or move. So a `str` is always safe to copy, and slicing a string
  can give a view, because a view of immutable bytes can't be told apart
  from a copy. Literals without escapes stay zero-copy slices of the source
  ([D13](D13-literals.md)).
- A string builder (`StrBuilder`, or `[]u8`) is the growable form, and
  finishing it gives a `str` without copying, because its bytes are already
  in the arena.
- For the programmer, the whole model is that strings are immutable values,
  and making a new one allocates in the current arena. A function that builds
  a string and returns it needs nothing special, because a callee allocates
  into its caller's arena. Returning a string built inside the function's own
  `with Arena()` is caught by the usual lifetime rule.
- These are optimizations that can't be observed:
  - Interpolation evaluates its embedded expressions left to right first,
    then formats all the pieces straight onto the top of the current arena,
    so the result grows in place and is never copied. User types take part
    through a formatting method that appends to a builder (something like
    `on T def __format__(self, *StrBuilder b)`) rather than returning a
    `str`, so nested formatting also appends in place.
  - `print` of an interpolated string writes the pieces directly and builds
    no string.
  - `s = s + x` extends `s` in place when its bytes are still at the top of
    the arena. Existing strings keep their own length and can't see the
    extra bytes. This is the same top-of-arena check as list growth, and it
    makes the usual string-building loop amortized linear.
- At run time `str` is an ordinary struct, as if it were declared
  `struct RuntimeStr: *u8 data; int len`, and the IL has nothing specific
  to strings. A literal's bytes go in the output's read-only data, copied
  from the source slice or from its decoded copy in `Ctx.strs`. The data
  isn't guaranteed to be NUL terminated.
- Eventually, the runtime's `print` will only print a `str`, and other
  values are converted by luv code, such as `on int def str __str__(self)`,
  which interpolation uses too.

**For the vertical slice (2026-11-16):**

- There are no struct values in the slice, so a `str` is emitted as its two
  words (data and len). This goes away when structs are implemented.
- `str + str` and interpolation outside `print` need an arena. They're
  typed by the front end but are rejected by emission as not supported yet.
- `print` remains a single expression and adds a newline. Multiple values
  are printed using interpolation. The runtime has one printer per kind,
  none of which write the newline: `luv_print_i64` (narrower signed ints are
  sign extended), `luv_print_u64` (zero extended), `luv_print_f64` (`f32`
  is widened, printed with `%f`), `luv_print_bool` (`true`/`false`),
  `luv_print_str(data, len)` and `luv_print_end` for the newline. An
  interpolated string in `print` emits one call per piece then
  `luv_print_end`, so no string is built.
- Output is buffered by the runtime and flushed at exit. The interpreter
  calls the same C functions that native code links against.

**Decided under option B (2026-11-12): values and references.**

| kind | semantics | representation |
|---|---|---|
| numbers, `bool`, structs, `[N]T` | values, copied on assignment | inline |
| `str` | a value (immutable, so sharing is invisible) | `{ptr, len}` |
| `[]T`, `{K}V` | references, like Python's list and dict | `*List{T}`, `*Dict{K, V}` |
| `*T` | an explicit pointer | a pointer |

- `s = Stuff(...)` is a value in the frame, or inline in whatever contains
  it, like C's `Stuff s = {...}`. This follows from compound literals ([D7](D07-keywords.md),
  [D9](D09-calls.md)) and from luv having `*T` and `&`.
- `List{T}` is an ordinary value struct in the prelude (`{data, len, cap}`),
  written the way a user would write `MyList{T}`. The reference behavior
  comes from the synonym, so `*MyList{int}` behaves exactly like `[]int`,
  and a plain `List{int}` value behaves exactly like a plain `MyList{int}`.
  Copying a plain `List{int}` value aliases its storage, as Go slices do,
  but only for someone who chose the value form.
- `.` auto-dereferences ([D22](D22-methods.md)), so `xs.append(4)` works on a `[]int`, and
  methods are defined `on List{T}`.
- A single object is allocated with `&Stuff(.a=1)`: the value goes into the
  current arena, and the result is a pointer to it. A list literal
  `[1, 2, 3]` is the same thing for `&List{int}(...)`, with its elements in
  the current arena, so the literal isn't special. The parser accepts `&` of
  a struct literal (`address_of_struct_literal.luv`) but not of other
  values such as `&5` or `&f()`. `new(...)` is the alternative if
  `&Stuff(...)` turns out to be too subtle.
- Open: zero values in general, and in particular what `[]int xs` with no
  initializer holds. Allocating an empty list is cheap (a bump) and matches
  Python, which has no null lists. Treating null as the same as an empty
  list would avoid that, but a method can't replace a null `self` with a new
  list, so `xs.append()` on a null `xs` would still need the declaration to
  allocate. Making it work would mean a method on `*[]int` that can replace
  its `self`, which [D22](D22-methods.md)'s rule against methods on pointers rules out, and
  that's probably not worth reopening. It probably belongs with a general
  decision about zero values.

**To decide during codegen:** whether calls whose result isn't kept run in a
per-statement scratch arena (statement temporaries, from option A). Keep it
if the compiler can elide temporaries cheaply.
