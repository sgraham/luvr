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

## By value or by reference (open)

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

### Nested functions can't escape

Functions aren't values: there's no syntax for function types, and a
function's name can only be called. So a nested function can only be
called while the function that encloses it is running, and can never
outlive its frame. That's the situation of Pascal's, Ada's, and GCC's
nested functions, which capture by reference without a garbage collector.

Escaping functions (function values stored in fields, returned, or passed
to something that keeps them) probably don't fit luv. If they're added
later, they'd capture by value or not at all, decided by what the function
is rather than by capture syntax. Capture lists (C++'s `[=]` and `[&]`,
Rust's `move`) are a lot to explain in a language without function values.

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

So the leaning is by reference: nested functions see and can assign the
enclosing function's variables, and luv60's tests above are rewritten.

### Implementing capture by reference

Every local is already a slot in the IL, and promoting slots to registers
happens after the whole function is emitted ([D26](D26-il.md)). `&x`
already relies on this: earlier uses of `x` don't need to know that its
address is taken later. A captured variable is one more use of a slot's
address, so emission is unaffected by when the capture is discovered.

**A static link** (Pascal's term). A nested function gets a hidden
parameter, the address of the enclosing function's frame. A captured
variable is the enclosing function's slot N, reached through the link, for
example with an IL operation that gives the address of the parent's slot N.
Each newly discovered capture only names its slot, so call sites don't
depend on what's captured: the enclosing function passes its frame, and a
nested function calling itself or a sibling passes the link that it was
given. Two levels up is the link stored in the parent's frame, as with
Pascal's static chains.

Slots don't have frame offsets in the IL. The backend assigns them when it
lays out the enclosing function's frame, which finishes after the nested
function has been emitted, so the nested function's offsets are resolved
then: either nested functions are finished after their parent, or their
offsets are fixed up. Slots that are reached through a link stay in memory
in the parent. Accesses through the link are loads and stores, which is
the cost of capture by reference.

**Lambda lifting** is the alternative: each captured variable becomes a
hidden `*T` parameter, and each call passes the slots' addresses. Call
sites then depend on the full set of captures, which isn't known while the
nested function's body is still being parsed. A recursive call in the body
can come before a capture that's found later, so its arguments would need
patching. The static link avoids that.

### Open questions

- By value or by reference.
- Whether assignment to captured variables is allowed (it's the main
  benefit of capture by reference).
- The IL operation for a parent's slot, and whether the backend finishes
  nested functions after their parent or fixes up offsets.
