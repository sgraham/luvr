# D30 Emission

Written 2026-11-16 as the plan for the vertical slice, and revised
2026-11-23 to describe what was built. It covers how the parser produces
[D26](D26-il.md)'s raw IL, in the style of Lua's `lcode.c`. [D26](D26-il.md)
is the IL itself, and [D31](D31-aggregates.md) adds aggregates to it.

Anything to do with the SSA pass is left until it's written: predecessor
lists, each loop's stored slots, block order invariants.

The slice was paused on 2026-11-20 before `for` over `range`, `break` and
`continue`. Real `str` and `range` both needed small aggregates, so
aggregates came first. `str` is done ([D32](D32-prelude.md)). `range` needs
the iteration protocol, and so method calls.

## The driver and the two builds

- `luv parse` checks files, and is the parser benchmark. `luv il` prints
  the dump.
- `parser.c` is compiled twice, by `parser_check.c` and `parser_emit.c`,
  which define `EMIT` as 0 or 1 and include it. Each subcommand calls the
  matching copy, so choosing between checking and emitting costs nothing
  while parsing. The entry point's name comes from the `PARSE_FILE` macro,
  so both copies link into one binary.
- Emission is written as `if (EMIT) { ... }` rather than `#if` wherever it
  compiles in both builds, so both builds type check every line, and the
  checking build's optimizer removes it.
- In the emitting build, a construct that can't be emitted yet is an error,
  "X isn't supported yet.", at the construct. The checking build doesn't
  emit at all, so every test still parses and type checks there. The test
  runner counts a test whose `luv il` fails this way as not run yet.

## What runs

`misc/interp.py` is a Python interpreter that reads `luv il` output. It's
scaffolding for developing emission, and [D25](D25-backend.md)'s "no
interpreter" still stands. The runtime's print functions are written in it
for now, and the C runtime is written when there's native code to link it
into. Every test in `test/` whose functions all emit is run through it, and
its `# OUT:` and `# RET:` headers are checked ([D29](D29-test-targets.md)).

## Functions

- The Parser has one `IlFunc`, since nested functions and methods aren't
  emitted yet. When a function ends, it's passed to `Ctx.on_func`, which
  for `luv il` prints it, and the buffers are reset.
- Each top-level function gets a symbol when its signature is parsed, and
  calls refer to it with a `KSYM` constant. Symbols are in a table on `Ctx`
  because modules share it. Calls are define-before-use until the
  [D11](D11-forward-refs.md) skim exists. A function's symbol is in
  `Parser.global_funcs`, indexed from its `GlobalInfo`, rather than in its
  `TypeInfo`, because function `TypeId`s will be interned once there are
  function pointers.
- The runtime's functions are symbols too, added when the `Ctx` is set up.

## ExpDesc

`ExpDesc.kind` says where an expression's value is:

| kind | value |
|---|---|
| `EK_REF` | an instruction's or constant's result |
| `EK_CONST` | a constant in `cv` |
| `EK_LOCAL` | in a slot, not loaded yet |
| `EK_ADDR` | in memory at `base + offset`, not loaded yet |
| `EK_CONST_DATA` | constant data, by its index in `Ctx.data` (only string literals so far) |
| `EK_FUNC` | a function, by its symbol |

Delaying the load of `EK_LOCAL` and `EK_ADDR` lets `x = y` be a `LOAD` and
a `STORE`, lets `&x` use the address without a load, and lets a field of a
field add its offset without any instruction. A string literal
(`EK_CONST_DATA`) stays unemitted until it's used. `print` of a literal
uses its data directly, and any other use builds a `str` in a temporary.
A `Ref` of 0 is never valid, so an expression that wasn't emitted
(`exp_unemitted`) is an error if it's used, rather than reusing a stale
value.

The emission helpers are in `parser.c`, named `emit_*`, such as
`emit_to_ref` (make `e` one value of type `t`), `emit_agg_addr`,
`emit_assign_to_local`, `emit_assign_to_addr` and `emit_infix` (called
after a binary operator is read, so that a non-constant left operand is
evaluated before the right one, while a constant is kept for folding).
Everything that writes to the buffer is in `il.c`, named `il_*`.

Types: `bool` and integers up to 32 bits are `i32`, kept extended to 32
bits. `i64`, `u64` and pointers are `i64`, and floats are themselves.
Narrow arithmetic will be extended after each operation (`SEXT8` after an
`i8` `ADD`). That's more instructions than extending where the result is
observed, but it's one rule, and the SSA pass's folding removes the extras.

## Locals and parameters

- A local is a slot, an aggregate slot if its type is an aggregate. Locals
  are function scoped ([D6](D06-assignment-declares.md)), so a slot is
  never reused within a function, and neither are temporaries.
- A scalar parameter is a value, not a slot ([D26](D26-il.md), "Slots").
  `PARAM n` is emitted at the start of the entry block, and the parameter's
  `Local` holds its `Ref`. An aggregate parameter is `PARAMAGG`, an
  address.

## Blocks and statements

A block number is handed out when something first needs to refer to it
(`il_block_declare`), and the block is started later (`il_block_start`).
Blocks are then not numbered in source order, which only matters to the SSA
pass, and can be fixed there or by switching to Lua-style pending jump
lists. `il_branch` is `BR`, which ends its block, so it also starts the
fall-through block.

- `if`/`elif`/`else`: a `BR` to the next branch when the condition is
  false, and a `JMP` to the end after each suite unless it returns. The end
  block is only declared if some branch reaches it.
- `and` and `or` store each operand to a temporary slot on its own path and
  load it after the join ([D26](D26-il.md)). A condition is then a bool
  value and a `BR`. Lua instead keeps a condition's jumps in true and false
  lists, so `if a and b:` branches straight to the else block. The SSA pass
  can recover the same branches (below).
- `return` is `RET` or `RET v`, and a void function that falls off the end
  gets a `RET`.

Not done yet:

- `for i in range(a, b)`: a call to `range` as the whole loop expression
  would be emitted as a counting loop, with `b` stored to a hidden slot so
  it's evaluated once: `STORE i, a`, a header block with
  `BR (LT i end), exit`, the body, a step block (where `continue` goes)
  with `STORE i, ADD i 1` and `JMP header`, and the exit (where `break`
  goes). With the iteration protocol this may be a special case of it
  instead.
- `break` and `continue` are `FLOW_ENDS`, which end their block like
  `return`, so nothing is emitted into an unreachable block.

## Recovering branches for `and` and `or` (for the SSA pass)

The jump lists aren't the only way to get the better branches back. The
SSA pass can recover them with a local rewrite, jump threading, without
changing the parser.

After the temporary slot is promoted, `if a and b:` is:

```
:0  BR a, :J              (a is false: the phi gets 0)
:B  %b = ...              (continues into :J)
:J  %t = PHI [:0: 0], [:B: %b]
    BR %t, :else
:then ...
```

The rewrite applies to a block that holds only a phi and a `BR` of it, when
nothing else uses the phi. For each predecessor whose incoming value is a
constant, where the `BR` goes is already known, so that predecessor's edge
is changed to go there instead. Here `BR a, :J` becomes `BR a, :else`.
Then `:B` is the only predecessor left, the phi is just `%b`, and `:J` is
merged into `:B` as `BR %b, :else`. That's the same code the jump lists
give: `BR a, :else` and `BR b, :else`.

- It needs predecessor lists and phis, which the SSA pass builds anyway,
  and a way to change a branch's target in place, which is one operand.
- Every edge that's changed goes to a later block, so the
  [D26](D26-il.md) invariants (source order, predecessors before their
  block) still hold.
- The shape can be recognized when the forward walk reaches `:J`, so it
  doesn't need to be a pass of its own.
- For `or`, the constant edge is the true path, an empty block with
  `JMP :J`, and the phi gets 1. Its `JMP` is changed to go to `:then`. A
  chain like `a and b and c` is threaded one join at a time.
- When the value is used as a value (`x = a and b`), the phi has another
  use, so nothing changes and the bool is kept.
- `not` is the awkward case. `if not (a and b)` is a `BR` of `XOR %t, 1`.
  Removing the `XOR` means swapping the true and false targets, but `BR`
  only branches on zero and otherwise continues into the next block, so a
  swap means reordering blocks. A flag on `BR` that inverts the test is
  probably simplest, since the encoder chooses the jump's condition anyway.

## Calls and print

- A call evaluates its arguments to refs left to right, then appends `CALL`
  and its `EXT` slots. The refs are kept in an array on the C stack while
  the arguments are parsed, with a limit of `MAX_CALL_ARGS`. An aggregate
  argument is an `ARGAGG` of its address, and an aggregate result is a
  `RESULTAGG` of a new temporary as the first argument
  ([D31](D31-aggregates.md)).
- `print` calls the runtime's printer for the value's type, then
  `luv_print_end`. An interpolated string in `print` is one call per piece,
  so nothing is allocated.
- String literal bytes go into `Ctx.data`. A constant of type `IT_DATA`
  refers to them by index. Like a `KSYM`, it's an address, and it becomes a
  relocation. It isn't a symbol only so that data doesn't need an interned
  name.

## The dump format

A fixed line grammar, close to QBE's and LLVM's text forms. For example:

```
def int add(int a, int b):
    return a + b

def int main():
    int x = add(1, 2)
    if x > 2:
        print "big \(x)"
    return 0
```

```
data @.dat0 "big "
data @.dat1 align 8 @.dat0 04 00 00 00 00 00 00 00

func i64 @add
:0
  %1 = PARAM i64 0
  %2 = PARAM i64 1
  ;     return a + b
  %3 = ADD i64 %1, %2
  RET i64 %3

func i64 @main
  slot $0 i64 x
:0
  ;     int x = add(1, 2)
  %1 = SLOT $0
  %2 = CALL i64 @add, 1, 2
  STORE i64 %2, %1
  ;     if x > 2:
  %5 = SLOT $0
  %6 = LOAD i64 %5
  %7 = GT i64 %6, 2
  BR %7, :2
:3
  ;         print "big \(x)"
  CALL @luv_print_str, @.dat1
  ...
```

- Top level lines are `agg`, `data` and `func`. An `agg` line is printed
  when the layout is added, and `data` lines before the next function.
  Inside a function, `slot` lines come first, then a label line `:n` for
  each block, followed by its instructions.
- `agg #N size align [sse0] [sse1] [hfa_f32|hfa_f64]` is an aggregate
  layout ([D31](D31-aggregates.md)). Elsewhere `#N` refers to it: an
  aggregate slot is `slot $n #N`, a function that returns one is
  `func #N @name`, and `COPY`, `ZERO`, `ARGAGG`, `PARAMAGG` and
  `RESULTAGG` have it as their last operand.
- A `data` line is `data @.datN [align N] "bytes"` when there are no
  relocations and every byte is printable, `\n` or `\t`. Otherwise it's
  `data @.datN [align N]` followed by each byte as two hex digits, with
  `@.datM` in place of the 8 bytes of a relocation to that data
  ([D34](D34-constant-data.md)). A string literal is its bytes, then a
  `str` image that refers to them. The alignment is only printed when it's
  more than 1. Data only refers to data before it.
- An instruction is `[%n =] OP [type] operand, operand, ...`. The type is
  left out when it's `void`, for `SLOT`, which is always an `i64` address,
  and for the aggregate operations. Types are a fixed set of words that no
  operand can look like.
- A compare's type is its operands' type, which the unsigned compares need
  to know the width. Its result is always an `i32` 0 or 1.
- The first character of an operand says what it is: `%n` an instruction,
  `@name` or `@name+8` a `KSYM`, `:n` a block, `$n` a slot, `#n` an
  aggregate. A `KINT` starts with a digit or `-`, and is printed as signed
  decimal and read at the instruction's type. A `KFLT` is a hex float
  (`0x1.8p+0`), which is exact and is read by Python's `float.fromhex`.
- `%n` is the instruction's index in the buffer, so there are gaps where
  `BLOCK` and `EXT` are.
- A call's arguments are all on its line, wherever they're stored. The
  `EXT` slots aren't printed.
- A comment is a whole line starting with `;`, so the reader can skip them
  without looking inside other lines. Comments give the text of each
  statement's source line, with its indentation, from the source position
  table ([D26](D26-il.md)), and above an instruction with float constants,
  their values in decimal (`; 0x1.8p+0 = 1.5`). Line numbers aren't
  printed, because the lexer only keeps offsets, and finding a line number
  means counting newlines from the start of the file.
- In `data @name "bytes"`, printable ASCII is written as itself except for
  `"` and `\`, which are `\"` and `\\`. Newline and tab are `\n` and `\t`.
- Symbol names can't contain whitespace. Data is `@.datN`, which can't
  collide with a function's name because identifiers can't start with `.`.

## Not emitted yet

Globals, `mod.func` calls, nested functions, methods, lists, dicts,
comprehensions, `with`, `check`, `for`, `break`, `continue`, floats,
`foreign` calls, and `str` operations other than `print`.
Comprehensions need extra care, since [D10](D10-comprehensions.md)'s rewind
re-lexes the element expression.
