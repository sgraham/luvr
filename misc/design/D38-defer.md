# D38 Scoped cleanup: `with` or `defer` (open, waiting on D12)

Written 2026-11-27, at the start of implementing `with`. Nothing here is
decided. luv60 has Python's `with`, and luvr parses it, but without
exceptions it's only used to run something at the end of a block. That's
what `defer` is for in other languages, so it's worth deciding which fits
luv before emitting either.

## What luv60's tests need

Eight tests in `test/with_*.luv` (and `comment_valid_after_block_start.luv`).
Each has a struct with `__enter__` and `__exit__` that print, and checks
that `__exit__` runs on every way out of the block: falling off the end,
`return` (after the return value is evaluated), and `break` and `continue`,
including through several nested `with`s inside nested loops. No test uses
`as`, and no `__enter__` returns a value.

So the behaviour that matters is "run this when the block is left, however
it's left, innermost first". Every option below needs that, and it's the
hard part of the emission either way.

## Options

### A. `with` (Python)

    with make_it():
        body

The manager is a struct with `__enter__(self)` and `__exit__(self)`.
`as name` binds `__enter__`'s result.

- The cleanup is tied to the value through a protocol, so it can't be
  forgotten, and it's one method call to emit at each exit.
- Any ad hoc cleanup needs a struct with two methods.
- Each manager is another level of indentation.
- Python designed it around exceptions (`__exit__` gets the exception and
  can suppress it). None of that applies.

### B. `defer` (Go, Zig, Odin, Swift, Jai, C2y)

    f = open(path)
    defer f.close()

The deferred code runs when the enclosing block is left, after anything
deferred later in the same block. Go runs defers when the function returns,
so a `defer` in a loop adds to a list at run time. Everything else listed
runs them at the end of the enclosing block, which is fully static: the
code is emitted at each exit, and nothing is kept at run time. For luv,
block scope is the natural choice, where a block is an indented suite.

- No protocol, and any call can be deferred.
- Acquire and release are next to each other, and nesting stays flat.
- Nothing ties the cleanup to the value, so it can be forgotten.

### C. A cleanup declaration

    with f = open(path)
    # f.__exit__() runs when this block is left

A declaration, not a block statement. The variable's type has `__exit__`,
which runs with the variable's address when the enclosing block is left.
It keeps A's protocol and B's flat nesting. This is the shape of C#'s
`using var`, Lua 5.4's `<close>` variables, and GCC and Clang's
`__attribute__((cleanup))` (all below).

### D. Both

`defer` as the primitive, with `with e:` (or C) as sugar for "call
`__enter__`, then defer `__exit__`", if structured forms turn out to be
wanted.

## Implicit or explicit

The options differ in who decides when the cleanup happens and how:

| | Who decides when | Who decides how | Examples |
|---|---|---|---|
| Fully implicit | nobody: when the owner dies | the type's destructor | C++ and Rust destructors |
| Opt-in | you, by marking the declaration | the type's method | C (`with f = open(p)`), C#'s `using var`, Lua's `<close>` |
| Explicit | you | you, by writing the call | B (`defer f.close()`), Go, Zig, C2y |

luv has no destructors, and memory is released in bulk by arenas
([D23](D23-arrays-lists.md)), so the question is only about other resources
(files, locks, the current pool), and only between the last two rows.
Opt-in means the "how" can't be got wrong, and the declaration reads as
"this one is cleaned up". Explicit shows what happens at the cost of one
more line, and works for things that aren't a type with a protocol, such as
restoring a saved value, or unlocking something that the block didn't
create. In both, the "when" is chosen, by placing it in a block.

## Precedents in more detail

### C#'s `using`

C# has two forms. The older one is a block, the same as Python's `with`:

    using (var f = File.OpenRead(path)) {
        Use(f);
    }   // f.Dispose() runs here, on every exit, including exceptions

C# 8 added `using` declarations, which drop the block:

    void Load(string path) {
        using var f = File.OpenRead(path);
        using var log = OpenLog();
        Use(f, log);
    }   // log.Dispose(), then f.Dispose(), on every exit from the scope

The type has to implement `IDisposable`, which is one method, `Dispose()`.
The variable is read-only, so it can't be reassigned, and `Dispose()` is
always called on the object that was declared. Disposal happens at the end
of the enclosing block, in reverse order of declaration. In practice the
declaration form replaced most uses of the block form, because it doesn't
indent.

Lua 5.4's `local f <close> = io.open(path)` is the same idea: the `__close`
metamethod runs when the variable goes out of scope, in reverse order, and
the variable is constant.

### GCC and Clang's `__attribute__((cleanup))`

    static void close_file(FILE** f) { if (*f) fclose(*f); }

    void load(const char* path) {
        __attribute__((cleanup(close_file))) FILE* f = fopen(path, "r");
        ...
    }   // close_file(&f) runs on every exit from the scope

The cleanup function gets the *address* of the variable, so it sees the
variable's value at exit, and reassigning it (or setting it to NULL to
transfer ownership) works. Linux's `cleanup.h` builds `__free(kfree)` and
`guard(mutex)(&lock)` on it. It's C's version of option C, without a
protocol: the function is named at each declaration.

### C2y's `defer` technical specification

Proposed for C as ISO/DIS 25755, described in
<https://thephd.dev/c2y-the-defer-technical-specification-its-time-go-go-go>:

    void f(void) {
        char* s = malloc(10);
        defer free(s);            // or defer { ... }
        ...
    }

- Block scope: it runs at the end of the block that contains it, so a
  `defer` in a loop body runs on each iteration.
- Nothing is captured. The deferred code refers to the variables directly,
  so it sees their values at exit. The article argues that capturing values
  at the `defer` would be "catastrophic", for example when `s` is
  reallocated after the `defer`, since the old pointer would be freed.
- `return` evaluates its value first, then runs the deferred code, then
  returns. The deferred code can't change the result. `break`, `continue`,
  and `goto` out of the block run it too.
- `break`, `continue`, `return`, and `goto` can't appear inside deferred
  code, and `goto` can't jump over a `defer` into its scope.
- Multiple defers run in reverse order.
- It's implemented by emitting the deferred code at each exit of the
  scope, with no storage or flags.

## When operands are evaluated

Go evaluates a deferred call's arguments and receiver at the `defer`, and
calls at exit. C2y, Zig, and Swift evaluate everything at exit. Option C and
the cleanup attribute pass the variable's address, which is evaluation at
exit for the variable.

Evaluating at exit is what the cleanup almost always wants. The value to
release is the one the variable holds when the block ends, after any
reallocation or ownership transfer. Go's rule is a known trap
(`defer fmt.Println(x)` prints the value at the `defer`), and the one case
it's better for, a loop that changes the variable before the end of the
block, is rare.

So the leaning is evaluation at exit, whichever form is chosen.

## Emitting it

All options need the same machinery: the parser keeps a stack of pending
cleanups for the function. `return` emits every pending cleanup, innermost
first, after the return value is evaluated. `break` and `continue` emit the
cleanups of the blocks between them and their loop (a `Loop` records the
stack's height when its body starts). Falling off the end of a block emits
that block's cleanups. What differs is what's emitted at each exit.

### 1. One call

At each exit, emit a call whose operands can be emitted again without
parsing: the address of a variable, a variable's current value, a field of
a local, or a constant. These are `ExpDesc`s that only refer to slots,
constants, and parameters, so they can be kept on the stack and emitted at
each exit. That gives evaluation at exit, and it's single pass: nothing is
parsed twice, and each exit adds a few loads and a call.

- Option C fits exactly: the only operand is the variable's address, as
  with the cleanup attribute.
- Options A and B fit if the deferred call's operands are restricted to
  those forms: `defer f.close()`, `defer free(p)`, `defer p.pop()` work,
  and `defer log("done \(n)")` doesn't. Its error would say that deferred
  arguments have to be variables, fields, or constants.

`defer free(p)` keeps "load the slot for `p`", and each exit emits that
load and the call. `"done \(n)"` isn't a place but a computation:
formatting `n` and building a new `str`, which is several instructions and
eventually an allocation. Evaluating it at exit means emitting that code
again at each exit, which needs approach 2 or 3. It could instead be
evaluated once at the `defer` into a hidden slot, and loaded at exit, which
is Go's rule. But then `n` would be read at the `defer` while `p` in
`free(p)` is read at exit, two rules in one statement depending on how
complicated an argument is. So with approach 1, deferred arguments are
restricted, and anything else is either a helper function
(`defer log_done(n)`, which reads `n` at exit) or waits for approach 3. Deferring
a nested function that makes the call depends on how nested functions
capture. With luv60's capture by value, `n` would be copied when the
function is made, at the `defer`, which is Go's rule again. With capture by
reference, which [D12](D12-nested-functions.md) is reconsidering,
`defer log_done()` with a nested `log_done` reads `n` at exit, which makes
nested functions the general way to defer anything that isn't a simple
call.

### 2. Parse the deferred statement again at each exit

Comprehensions already rewind the lexer ([D10](D10-comprehensions.md)) to
parse an element after the clauses that bind its names. A `defer` could
record its `LexPos`, and each exit could rewind and parse it again. This
gives C2y's semantics with any statement. But it's the parser reading the
same text twice to produce code, rather than to settle a syntax question
as comprehensions do, so it goes against the single-pass goal.

### 3. Emit it once, copy the IL

Parse and emit the deferred statement once into a side buffer, and copy
its instructions at each exit, renumbering the refs and blocks inside the
copy. A self-contained statement only refers to its own instructions,
constants, slots, and parameters, so the copy is valid at every later
exit. It's single pass, allows any statement, and evaluates at exit. It's
an IL facility that nothing else needs yet.

A cleanup block emitted once, with exits jumping to it and a slot saying
where to go afterwards, is what C compilers often do. It would break
[D26](D26-il.md)'s invariant that only loop headers have predecessors after
them, so it isn't an option here.

## Examples with block scope and function-scoped names

Names are scoped to the function ([D6](D06-assignment-declares.md)), but
cleanup is scoped to the block. In each example, the cleanup runs where the
comment says.

A loop that opens a file on each pass:

    def int count_lines(*Files fs):
        total = 0
        for name in fs.names():
            f = open(name)
            defer f.close()        # or: with f = open(name)
            if f.is_empty():
                continue           # f.close() runs here
            total += f.count_lines()
                                   # and here, at the end of each pass
        return total

A cleanup inside an `if`, which runs at the end of the `if`'s suite rather
than the function. This is where block scope and function-scoped names can
surprise:

    def run(bool verbose):
        if verbose:
            log = open_log()
            defer log.close()
            log.write("starting")
                                   # log.close() runs here
        work()
        log.write("done")          # log is still a name here, but closed

Python has the same shape: after `with open(p) as f:`, `f` is still bound,
and closed. A Go programmer would expect the `defer` to run at the end of
the function. The fix is to open the log unconditionally, or to give the
`if` and the rest of the work their own function.

Early returns with nested cleanups:

    def int load(str path):
        f = open(path)
        defer f.close()
        hdr = f.read_header()
        if not hdr.valid:
            return -1              # f.close() runs after -1 is evaluated
        buf = alloc(hdr.size)
        defer free(buf)
        if f.read_into(buf) != hdr.size:
            return -2              # free(buf), then f.close()
        return checksum(buf)       # checksum(buf) is evaluated, then
                                   # free(buf), then f.close()

The same with option C, where the protocol replaces each `defer` line:

    def int load(str path):
        with f = open(path)
        hdr = f.read_header()
        if not hdr.valid:
            return -1
        with buf = Buffer(hdr.size)
        ...

And with option A, for comparison:

    def int load(str path):
        with open(path) as f:
            hdr = f.read_header()
            if not hdr.valid:
                return -1
            with Buffer(hdr.size) as buf:
                ...

## Pools

D23's sketch is `with Pool() as p:` (make a pool, make it current, free it
at the end) and `into p:` (make an existing pool current). `into p:` is an
enter and exit, limited to one thing: it saves the current pool, makes `p`
current, and restores the saved one when the block is left. Allocations in
calls made inside the block go to `p`.

So pools need the same exit machinery as everything above, with a builtin
action (restore the saved pool) in place of a call. Whatever's chosen here,
a pool statement can be a compiler-known cleanup on the same stack. That
makes it reasonable to give pools their own syntax later, rather than
shaping `with` or `defer` around them, once pools have been used in real
code.

## Leaning

- Block scope, and evaluation at exit.
- Emission approach 1, which is single pass.
- Between B and C: B (`defer` of a call whose operands are variables,
  fields, or constants) is more general. C (`with f = open(path)`) can't be
  forgotten, has no operand rules, and needs a protocol method. Both fit the
  same emission, so both could exist later, and the tests can be rewritten
  for either.

## Open questions

- Waiting on [D12](D12-nested-functions.md)'s capture by value or by
  reference, which decides how much a restricted `defer` can do through
  nested functions.

- B, C, or both. Whether A stays at all.
- For B: whether the operand restriction is acceptable, or whether
  arbitrary statements are worth approach 3.
- For C: the name of the method (`__exit__`, `__close__`), whether the
  variable is read-only (C#, Lua), and whether the form is `with f = ...`
  or a new keyword.
- Whether a cleanup can appear in any block (the leaning), or only at the
  top level of a function body, which would avoid the `if` surprise above.
- Pools: their own statement, sharing the cleanup stack.
