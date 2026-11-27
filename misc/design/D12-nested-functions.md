# D12 Nested functions and captures

luv captures **by value at definition time** (`nested_simple_upval.luv`,
`nested_capturing_func_with_upval.luv`). Lua 4.0.1 had the same semantics,
spelled `%name` and limited to the immediately enclosing function
(`pushupvalue`, `lparser.c:229`).

- Implicit capture (current luv): the parser walks the `FuncState->prev`
  chain like `search_local` (`lparser.c:187`) and records upvalues per
  function. LL(1) either way, since this is name resolution.
- Explicit `%x` (Lua 4): cheaper analysis, uglier.

**Recommendation: implicit**, with multi-level capture as luv already
supports (`nested_two_levels.luv`). Use Lua's `FuncState` chain directly.

## By value or by reference (settled for now)

Reopened 2026-11-27, because [D38](D38-defer.md)'s cleanup options depend on
it: a deferred call to a nested function would read its captures at the
`defer` under capture by value, and at exit under capture by reference.

The decision above records luv60's behaviour, and luv60's tests pin it
down. In `nested_simple_upval.luv`, `z = 123` after `inner` is defined
doesn't change what `inner` sees, and
`nested_two_levels_with_mutation_after_capture_in_middle.luv` checks the
same through two levels and a pointer write. An enclosing function's
variables also can't be assigned from a nested function ("Upvalues are
immutable.").

### Settled for now (2026-11-27): an explicit capture list

Every nested function that uses an enclosing function's variables lists
them after its parameters. A bare name is a copy made at the definition,
and `&name` is the enclosing function's variable itself, read live and
writable:

    def outer():
        n = 0
        def middle() uses &n:
            def inner() uses &n:
                n = 1             # writes outer's n
            inner()
        middle()
        return n                  # 1

    def run():
        n = 0
        def loggy() uses &n:
            log("done \(n)")      # reads n when it's called
        defer loggy()             # D38: logs the final n
        for i in range(10):
            n += i

    def step() uses &total, scale:
        total += scale            # scale is a copy, total is shared

- A name that isn't listed isn't visible from the enclosing function. So
  `n = 1` in a nested function that doesn't capture `n` declares a new
  local, and reading an enclosing function's variable without capturing it
  is an error ("n isn't captured"), which is also where a missing `&` is
  caught. A nested function sees its parameters, its captures, and
  globals, like a top-level function with a declared interface.
- A copy is read-only, as parameters are ([D22](D22-methods.md)): `n += 1`
  or `n.x = 1` on a copy are errors, while `xs[0] = 1` on a list and
  `p.x = 1` through a pointer change what's shared.
- `nonlocal` isn't needed: `&n` is how a nested function writes an
  enclosing function's variable.
- The choice between a copy and the live variable is made per variable.
  A copy is about as fast as the same code written inline, and only the
  variables marked `&` have to be kept in memory in the enclosing
  function, which answers the performance reservation below.
- Forwarding is explicit: `middle` lists `&n` to pass it to `inner`.
- The capture set is known before the body is parsed, so the capture
  block's layout (below) is fixed at the start, with no discovery and no
  forwarding up the scopes.
- An escaping function, if those are ever added, could only list copies,
  which is visible on the definition line.
- It's LL(1): only `:` can follow the parameters now.

The marker is written `uses` here. It isn't needed for parsing, since the
list could follow `)` directly, but it makes the line read as a declared
interface, and it can be searched for. It has to be a reserved word:
without one, a name can start the list, so a contextual `uses` would be
ambiguous with capturing a variable called `uses`. `captures` is the
longer, more literal alternative. PHP is the precedent for the whole
feature: `function () use ($a, &$b)` has to list every capture, with `&`
for a reference.

The cost is to the programmer: luv60's nested function tests need lists,
and a function that captures many variables has a long first line. It's
easy to relax later, by choosing a default for unlisted names once real
code shows what's common, which keeps every list valid. The sections below
are the options for that default.

### Nested functions can't escape

Functions aren't values: there's no syntax for function types, and a
function's name can only be called. So a nested function can only be
called while the function that encloses it is running, and can never
outlive its frame. That's the situation of Pascal's, Ada's, and GCC's
nested functions, which capture by reference without a garbage collector.

Escaping functions (function values stored in fields, returned, or passed
to something that keeps them) probably don't fit luv. A nested function
that captures nothing is just a function, and could become a function
pointer if those are added. One that captures anything, by value or by
reference, has a capture block in its parent's frame (below), so it
can't outlive the frame. If escaping functions are wanted later, they'd be
explicit, for example a separate kind of definition whose capture block is
allocated in the current pool ([D23](D23-arrays-lists.md)), which can only
capture by value. Capture lists in the C++ style (`[=]`, `[&]`, Rust's
`move`) are a lot to explain in a language without function values.

### Assigning to captured variables, and `nonlocal`

What should `n = 1` in `inner` mean here?

    def int outer():
        n = 0
        def middle():
            def inner():
                n = 1
            inner()
        middle()
        return n

| `n = 1` in `inner` | `outer` returns | Hazard |
|---|---|---|
| An error (luvr now: "Upvalues are immutable.") | doesn't compile | Moving code into a nested function can collide with a name in some enclosing function, which is odd for encapsulation. |
| Writes `outer`'s `n` (plain capture by reference) | 1 | Reusing a name silently changes an enclosing function's variable, with no error if the types match. |
| A new local `n` in `inner` (Python) | 0 | In code like this example, it looks as if `outer`'s `n` should change. |

The leaning is the third, a new local, because it's what
[D6](D06-assignment-declares.md) already does for module globals: an
assignment in a function shadows a global from that point on, unless the
function says `global x`. Then a function's assignments are always its own
unless it says otherwise, and moving code in or out of a nested function
can't start or stop writing another function's variable. Under D6's rule
(declared at the first assignment), a read before `n = 1` sees `outer`'s
`n`, and reads after it see the local, as `assign_global.luv` does for a
global.

**Writing an enclosing function's variable**, for example to update a
counter, then needs a declaration. The leaning is `nonlocal n`, which
mirrors `global x`:

    def int outer():
        n = 0
        def middle():
            def inner():
                nonlocal n
                n += 1
            inner()
        middle()
        return n               # 1

- It's Python's keyword and rule. In Python, `global x` binds `x` to the
  module, and `nonlocal x` binds it to the nearest enclosing function that
  has an `x`, skipping the module.
- Like `global`, it has to come before the name is used, which single-pass
  parsing needs anyway, so it sits near the top of the function, about as
  visible as a capture list on the definition.
- Only the function that writes declares it. `middle` forwards `n`
  without saying so, as luv60's `create_upval()` forwards captures.
- A variable that's declared `nonlocal` is captured by reference, and every
  other capture is a snapshot (by value). So `nonlocal` is also the switch
  between the two kinds of capture, which is what [D38](D38-defer.md)'s
  `defer` needs for reading a variable at exit.

**Augmented assignment and field writes.** `n += 1` and `n.x = 1` can't
shadow, since they need the existing value, so without `nonlocal` they're
errors, with a message that suggests `nonlocal`. In Python, `n += 1`
without `nonlocal` is also an error (`UnboundLocalError` at run time, since
the assignment makes `n` local). `n.x = 1` works in Python without
`nonlocal`, because Python variables refer to shared objects, and it only
changes the object. In luv a struct is a value, and without `nonlocal` the
nested function has a copy, so changing a field of the copy would be
silently lost.

**The rule in one line, under capture by value:** a capture that isn't
`nonlocal` behaves like an extra parameter passed by value, and a
`nonlocal` capture is the enclosing function's variable. (Under the revised
leaning below, captures are live, and a capture is like a read-only
parameter passed by reference. The table's answers are the same, but the
"no" rows are because they'd change the enclosing function's variable.) Parameters already work this way
([D22](D22-methods.md)): `def f(Stuff s)` can't do `s.x = 1` ("Function
parameters are immutable."), but `def f(*Stuff p)` can do `p.x = 1`. So:

| In `inner`, without `nonlocal` | Allowed? | Why |
|---|---|---|
| `xs[0] = 1`, `xs.append(4)` with `[]int xs` | yes | A list is a pointer ([D23](D23-arrays-lists.md)), so the copy points at the same list. |
| `p.x = 1` with `*Stuff p` | yes | The same, through a pointer. |
| `n.x = 1` with `Stuff n`, `a[0] = 1` with `[4]int a` | no | Structs and fixed arrays are values, so it would change the copy. |
| `n += 1` | no | The same, for a scalar. |
| `n = 1`, `xs = [1]` | declares a new local | Plain assignment shadows, as for globals. |

The errors can be the same kind as for parameters, and suggest `nonlocal`.

luv60 reserves `nonlocal` as a keyword but never parses it.

### Which is more useful

- Without escaping, nested functions are mostly for factoring code inside
  one function: a helper that updates an outer counter, a recursive visitor
  that accumulates into an outer list, a cleanup called from `defer`. These
  want to see the enclosing variables' current values, and often to change
  them. Under capture by value, each needs an explicit `*T` parameter
  instead.
- Python captures by reference (cells, with `nonlocal` to assign). Its
  well-known surprise, `[lambda: i for i in range(3)]` all returning 2,
  needs functions that are stored, which luv can't do.
- Capture by value matters when a function outlives its frame, which luv's
  can't.

Capture by value can already express all of this, by passing pointers
explicitly. With [D38](D38-defer.md)'s `defer` of a call, this reads `n` at
exit, since `&n` is the address of a local and can be passed at each exit:

    def func():
        n = 0

        def loggy(*int n):
            log("done \(*n)")
        defer loggy(&n)

        for i in range(10):
            n += i

So the choice is convenience against explicitness, not what can be
expressed:

| | By value | By reference |
|---|---|---|
| Reading the current value | pass `&n`, read `*n` | `n` |
| Updating an outer counter | pass `&total`, `*total += x` | `total += x` |
| Keeping a snapshot on purpose | automatic | copy it to a local first |
| What a reader sees | shared variables are in the signature | sharing is implicit, as in Python |

The last row is the argument for capture by value: a helper's signature
shows what it can modify. It's the same tension between implicit and
explicit as in D38's cleanup options.

### Marking captures by reference

A third position keeps capture by value as the default, and marks the
variables that are shared. Both forms below are implemented with the same
static link as capture by reference. The marker only changes what the
parser allows.

**A capture list on the definition:**

    def func():
        n = 0

        def loggy() &n:
            log("done \(n)")
        defer loggy()

        for i in range(10):
            n += i

- The shared variables are listed once, on the line that reads as the
  function's signature, which is where the explicit-pointer version puts
  them too. So it keeps that version's advantage without `&n` at each call
  and `*n` in the body.
- It's LL(1): only `:` can follow the parameters now, so an optional
  `&name {',' &name}` before the `:` is decided by its first token.
- PHP's closures are the closest precedent: `function() use ($a, &$b)`
  captures by value unless a variable is marked `&`. Swift's capture lists
  are the reverse, marking snapshots in a language that captures by
  reference. C++'s capture lists also have defaults (`[=]`, `[&]`),
  `this`, and initializers, none of which are needed here.
- luv60's tests stay valid, since unmarked captures are still by value.

**A marker at each use, `^n` or `%n`.** `%n` is the reverse of Lua 4's
`%x`, which marked a capture by value. `^n` looks like a reference to an
outer level, and `^` isn't used for pointers ([D37](D37-dereference.md)).
Either parses, since a prefix operator where an operand is expected is
distinct from the binary operator (xor or modulo) after one, as with `-`.
But `a ^ ^n` and `x %n` (which is modulo) read badly, the marker is
repeated at each use, and the function's first line doesn't show what it
can modify. It's weaker than the capture list on every point except
showing each shared access where it happens.

| | Default | Sharing is visible in | Ceremony |
|---|---|---|---|
| By value with pointers (luv60) | value | the signature and each call | `&n` at calls, `*n` in the body |
| By reference | reference | nowhere | none |
| Capture list | value | the definition | `&n` once per shared variable |

### Leaning

Revised 2026-11-27: Python's rules throughout. A capture reads the
enclosing function's live variable (by reference), plain assignment
declares a new local (as for globals), and `nonlocal n` is only for
writing. Then `nonlocal` has the one meaning it has in Python. Under
capture by value, unmarked captures were snapshots, so `nonlocal` also
switched a capture from a snapshot to the live variable, and a function
that only reads `n` needed `nonlocal` to see its current value:

    def run():
        n = 0
        def loggy():
            log("done \(n)")     # a snapshot would always be 0
        defer loggy()            # live: logs the final n
        for i in range(10):
            n += i

Capture by value came from luv60, rather than being chosen for nested
functions that can't escape, and it matters most for functions that do
escape (below).

With live captures, the rule for writes is that a capture is like a
read-only parameter passed by reference. `xs[0] = 1` on a list and
`p.x = 1` through a pointer change what's shared, as before. `n.x = 1` on a
struct and `n += 1` need `nonlocal`, now because they'd change the
enclosing function's variable, rather than because the change would be
lost on a copy.

**Reservation (the user's):** capture by value is more flexible for
performance. Its default is about as fast as the same code written inline,
and sharing is still available, with some ceremony, by passing a pointer
and dereferencing it. Capture by reference puts every captured variable in
memory, and there's no way to ask for a copy, apart from copying to another
local before the nested function is defined, which is still captured by
reference. A possible mitigation: a capture that's never written after the
definition, by the parent or through `nonlocal`, can be copied into the
capture block without changing the meaning. The parser knows about
`nonlocal`, but whether the parent writes the variable later is only known
after the function is emitted, so it would be an optimization in the SSA
pass rather than in the parser.

Not decided until there's real luv code written both ways. The capture
list (`def loggy() &n:`) remains an alternative.

### Implementation: a capture block per nested function

This is what luv60 does (`create_upval()` and the end of a nested
definition in `luv60/src/parse.c`), and it handles every option above.

- Each nested function gets a hidden first parameter, a pointer to its
  capture block. The block is an aggregate slot in the parent's frame.
- When the body first refers to an enclosing function's variable, it's
  appended to the block at the next offset (aligned for its type). The
  offset is fixed when the capture is discovered, so the body's loads are
  emitted with their final offsets, and nothing is patched later.
- A recursive call passes the block pointer that the function was given,
  so captures discovered after the call don't matter.
- The parent fills the block at the definition, after the body has been
  parsed, which is where parsing resumes in the parent, so the full set is
  known. A capture by value copies the value in, which is the snapshot at
  definition. A capture by reference stores the variable's address.
- Captures through several levels are flattened. If `inner` uses `x` from
  `outer`, through `middle`, then `x` is appended to `middle`'s block too
  (luv60 recurses up the scopes), and `middle` copies it into `inner`'s
  block when `inner` is defined. That works because each block only grows
  while its function's body is being parsed. For a capture by reference
  the forwarded value is the address, and a capture list would require
  `middle` to list `&x` too.
- A sibling call needs the sibling's block, which is in the parent's
  frame, so `f` calling `g` captures `g`'s block address. luv60 stores a
  captured function as two words, its symbol and its block.

It needs no new IL: an aggregate slot, stores into it, a hidden pointer
parameter, and loads through it. Slots that a capture by reference points
at stay in memory, and the promotion to registers after emission
([D26](D26-il.md)) already handles address-taken slots, as for `&x`.

Two other implementations were considered, and the capture block is
simpler than both:

- **A static link** (Pascal): a hidden pointer to the parent's frame, with
  captures read from the parent's slots through it. It needs an IL
  operation for "the parent's slot N", and slot offsets aren't known until
  the backend lays out the parent's frame, after the nested function has
  been emitted.
- **Lambda lifting**: each capture is a hidden parameter. Call sites
  depend on the full set of captures, which isn't known while the body is
  being parsed, so a recursive call before a later capture would need
  patching.

### Escaping functions (exploration)

What an explicit escaping function could look like, written down to see
what it would cost, not as a plan. Escaping means a function value that can
be stored, returned, or passed to something that keeps it, which needs
function types ([D18](D18-future-syntax.md) reserves `def(int) int` in type
position). The placeholder syntax here is `escaping def`.

**Why it has to be marked at the definition.** A nested function's body is
emitted while it's parsed, with its captures read through its capture
block. An escaping function's captures have to be read from storage that
outlives the frame, so the body is compiled differently, and the parser
has to know before the body. Deciding when the name is first used as a
value would be too late in a single pass.

**A possible shape:**

- Its capture block is allocated in the current pool
  ([D23](D23-arrays-lists.md)) when the definition is reached, instead of
  in the parent's frame. Its value is two words, the code and the block.
- Captures are copies, made at the definition. It can't use `nonlocal`,
  since that would refer to a variable in a frame that may be gone.
- It can't capture anything whose lifetime is tied to the frame: a pointer
  to a local (`&x`), or a nested function that isn't escaping (its block is
  in the frame). D23's roots already give every pointer expression a root,
  so this is the same check as storing a value into a structure: the
  captured value's root has to live at least as long as the current pool.
- The function value is rooted in the current pool, so it has the same
  lifetime rules as anything else allocated there. For example, it can't be
  returned from the function whose `with Pool()` made the pool.
- Every call through a function value passes the block as a hidden first
  argument. A top-level function used as a value would need a small thunk
  that drops it, or every function would take a hidden argument, which
  isn't wanted.

**The alternative is no closures as values.** Only functions that capture
nothing can be function values, and context is passed explicitly as a
pointer, as in C (`qsort_r`), Zig, Odin, and Jai. Anything closure-like is
a struct with a method. This needs no pool allocation and no lifetime
rules for functions.

**Precedents.** Clang's blocks (C and Objective-C) are close to this whole
design. A block starts on the stack and captures by value, `__block`
variables are shared (like `nonlocal`), and a block that has to escape is
copied to the heap explicitly with `Block_copy`, which moves the `__block`
variables with it. C++ lambdas are values with capture lists, and
`std::function` allocates. Rust's closures are types, and a `Box<dyn Fn>`
allocates. Go and Swift use their GC or reference counting.

**A third shape, copying at the use.** Instead of a different kind of
definition, an ordinary nested function could be converted when it's used
as a value, as with `Block_copy`: each captured variable is copied into the
pool, and the new block holds addresses of the copies. The body reads
through addresses either way, so it doesn't need to be compiled twice. But
writes through `nonlocal` after the copy would go to the copies, not the
frame, so the two diverge silently, which is what `Block_copy`'s moving of
`__block` variables avoids with indirection that luv doesn't have.

### Open questions

- The marker's word (`uses`, `captures`), or no marker.
- Whether nested function names have to be listed to be called (as they
  are now, `def sub() uses helper:`). Leaning no: functions can't be
  assigned, so there's no choice between a copy and the live variable, a
  global function doesn't need listing either, and a recursive nested
  function would otherwise list itself. A call to a nested function that
  captures would add its capture block's address to the caller's block
  implicitly.
- Whether to relax the list later, and to which default.

- Whether plain assignment to an enclosing function's name shadows it
  (the leaning), is an error (now), or writes it.
- `nonlocal n`, a capture list, or explicit pointers for sharing.
- Live captures (the leaning) or snapshots by default. To be decided after
  writing real luv code both ways.
- Whether escaping functions exist at all, and if so whether they're a
  separate kind of definition, or a copy made at the use.
- Whether a capture block holds snapshots and addresses side by side (the
  leaning), or capture by reference reads through a separate pointer.
