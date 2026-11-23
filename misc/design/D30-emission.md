# D30 Emission for the vertical slice (proposal)

Written 2026-11-16, for review before any code. It covers how the parser
produces D26's raw IL for the slice: int, bool and str literals, locals,
arithmetic and comparisons, `if`, `for` over `range`, `print`, functions
with parameters, calls and `return`. D26 is the IL itself. This is the
interface between `ExpDesc` and the IL buffer, in the style of Lua's
`lcode.c`.

Anything to do with the SSA pass is ignored for the slice: predecessor
lists, each loop's stored slots, block order invariants. Those are added
when the SSA pass is written.

## The driver and the two builds

- `parsecheck` becomes `luv`, with subcommands: `luv parse` (parsecheck's
  current job), `luv il` (prints the dump) and later `luv run`.
- `parser.c` is compiled twice, from two small files that define `EMIT` as
  0 or 1 and then include it. Each subcommand calls the matching copy, so
  choosing between checking and emitting costs nothing while parsing. The
  entry point's name comes from a macro, so that both copies can be linked
  into one binary.
- In the parser, emission is written as `if (EMIT) { ... }` rather than
  `#if`, so both builds compile and type check every line, and the checking
  build's optimizer removes it. The `exp_*` helpers return a value that's
  unused in the checking build, such as a `Ref` of 0.
- `luv parse` stays the parser benchmark. Emission's cost can't be
  measured on dumbbench until enough of the language emits, so a
  benchmark input restricted to the slice is generated when emission is far
  enough along to matter.

## What runs

- The slice is run by `misc/interp.py`, a Python interpreter that reads
  `luv il` output. It's scaffolding for developing emission. D25's "no
  interpreter" still stands, and making it a separate script makes that
  obvious. Python is used because it reads the text dump easily and the
  build script already requires it.
- The runtime's print functions (D23) are written in the script for now.
  The C runtime is written when there's native code to link it into.
- The dump is the interface between the two, so it has to be easy to read
  back. It's also what's used to diagnose the front end later, so it has to
  be easy for a person to read too. See "The dump format".
- The test runner pipes `luv il` into the interpreter for each test whose
  functions all emit, and checks `# OUT:` and `# RET:`. The number of tests
  that pass is the slice's progress.

## Functions and the sink

- A `FuncState` per function being parsed holds the instruction buffer,
  constants, slot table and block table. Parsing a nested `def` uses the
  next `FuncState`, so the buffers are kept per nesting depth and reused.
- When a function ends, it's passed to a callback on `Ctx`, as files are
  loaded through `load_file`. For `luv il` the callback prints it, and the
  buffers are reset.
- In the emitting build, a construct that the slice can't emit yet is an
  error, "X isn't supported yet.", at the construct. The checking build
  doesn't emit at all, so every test still parses and type checks there.
- Each function gets a symbol, and calls refer to it with a `KSYM`
  constant. Symbols are in a table on `Ctx` because modules share it. Calls
  are define-before-use until the D11 skim exists, so a symbol always
  exists before a call refers to it. The symbol is found through a
  separate table indexed from `GlobalInfo`, like `global_consts`, rather
  than from the function's `TypeInfo`, because function `TypeId`s will be
  interned once there are function pointers.

## ExpDesc

`ExpDesc` gets a kind that says where its value is, replacing `is_const`:

| kind | value | info |
|---|---|---|
| `EK_CONST` | in `v`, as now | |
| `EK_LOCAL` | in a slot, not loaded yet | slot index |
| `EK_REF` | an instruction's or constant's result | `Ref` |
| `EK_STR` | a string literal | the literal's index in a table of string data |
| `EK_VOID` | nothing, the result of a void call | |

Delaying the load of `EK_LOCAL` lets `x = y` be a `LOAD` and a `STORE`,
and lets `&x` use the slot's address without a load. `EK_STR` stays
unemitted until it's used, because it becomes two values.

Emission helpers are in `parser.c` next to `ExpDesc`, named `emit_*`:

- `emit_to_ref(p, e, t)`: emits whatever is needed to make `e` one value of
  type `t`, and returns its `Ref`. Untyped constants get their type from
  `t`, which has already been checked by `check_assign` or
  `type_for_arith`.
- `emit_to_str_refs(p, e, refs)`: the two values (data and len) of a `str`.
- `emit_store_local(p, local, e)`: an assignment or declaration.
- `emit_infix(p, e)`: called after a binary operator is read and before
  its right operand is parsed. A non-constant left operand is turned into a
  `Ref`, so that it's evaluated before the right operand. A constant is
  kept so that folding still works.

Everything that writes to the buffer is in a separate `il.c`, named
`il_*`: appending an instruction, interning a constant, adding a slot,
blocks and jumps, and the dump.

Mapping types: `bool` and integers up to 32 bits are `i32`, `i64`, `u64`
and pointers are `i64`, floats are themselves. Narrow arithmetic results
are extended immediately after each operation (`SEXT8` after an `i8`
`ADD`). That's more instructions than "where the narrow result is
observed", but it's one rule and the SSA pass's folding removes the extra
ones later.

## Locals and parameters

- A local is a slot. `Local` gets the slot index, set when it's declared.
  Locals are function scoped (D6), so a slot is never reused within a
  function.
- A parameter is a value, not a slot (D26, "Slots"). `PARAM n` is emitted
  at the start of the entry block, and the parameter's `Local` holds its
  `Ref`. Reading the parameter gives an `EK_REF`.
- A `str` local is two slots, and a `str` parameter is two `PARAM`s, until
  structs exist (D23, "For the vertical slice").

## Blocks and jumps

A block number is handed out when something first needs to refer to it,
and the block is started later. The block table holds where each one starts.
Blocks are then not numbered in source order, which only matters to the SSA
pass, and can be fixed there or by switching to Lua-style pending jump
lists when it's written.

- `il_block_declare(f)` returns a block number that isn't started yet.
- `il_block_start(f, b)` starts block `b` at the current position.
- `il_jump(f, b)` and `il_branch(f, cond, b)` are `JMP` and `BR`. `BR`
  ends its block, so `il_branch` also starts the fall-through block.

Statements:

- `if c: A elif d: B else: C`: a new block for the end, a new block for
  each next branch, `BR` to the next branch when the condition is false,
  and a `JMP` to the end after each suite unless it returns.
- `for i in range(a, b)` (and `range(b)`): a call to the builtin `range`
  as the whole loop expression is recognized and emitted as a counting
  loop. `b` is stored to a hidden slot so that it's evaluated once.

  ```
  STORE i, a
  BLOCK header
  BR (LT (LOAD i) (LOAD end)), exit
  BLOCK body
  ...                 (continue jumps to step)
  BLOCK step
  STORE i, ADD (LOAD i) 1
  JMP header
  BLOCK exit          (break jumps here)
  ```

  Any other loop expression isn't supported yet.
- A loop stack in `FuncState` holds each loop's step and exit blocks for
  `continue` and `break`. As planned with `Flow`, `break` and `continue`
  become `FLOW_JUMPS` and have to end their block like `return`, so that
  nothing is emitted into an unreachable block.
- `return` is `RET` or `RET v`. A void function that falls off the end gets
  a `RET`.
- `and` and `or` store each operand to a temporary slot on its own path and
  load it after the join (D26). A condition is then a bool value and a
  `BR`. Lua instead keeps a condition's jumps unresolved in two lists, one
  for where to go when it's true and one for false, so `if a and b:` is two
  branches straight to the else block without making a bool. That can come
  later, if the generated code shows it's worth it.

## Recovering branches for `and` and `or` (for the SSA pass)

Written 2026-11-19, to read when the SSA pass is written. The jump lists
aren't the only way to get the better branches back. The SSA pass can
recover them with a local rewrite, jump threading, without changing the
parser.

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
- Every edge that's changed goes to a later block, so the D26 invariants
  (source order, predecessors before their block) still hold.
- The shape can be recognized when the forward walk reaches `:J`, so it
  doesn't need to be a pass of its own.
- For `or`, the constant edge is the true path, an empty block with
  `JMP :J`, and the phi gets 1. Its `JMP` is changed to go to `:then`. A
  chain like `a and b and c` is threaded one join at a time.
- When the value is used as a value (`x = a and b`), the phi has another
  use, so nothing changes and the bool is kept.
- A `BR` of a compare still becomes a compare and a conditional jump in the
  encoder, as it would have.
- `not` is the awkward case. `if not (a and b)` is a `BR` of `XOR %t, 1`.
  Removing the `XOR` means swapping the true and false targets, but `BR`
  only branches on zero and otherwise continues into the next block, so a
  swap means reordering blocks. A flag on `BR` that inverts the test is
  probably simplest, since the encoder chooses the jump's condition anyway.

## Calls and print

- A call evaluates its arguments to refs left to right, then appends `CALL`
  and its `EXT` slots. The refs are kept in a fixed array on the C stack
  while the arguments are parsed, with a limit on the argument count. A
  `str` argument is two refs.
- `print` uses D23's runtime calls, one per kind, then `luv_print_end`. An
  interpolated string in `print` is one call per piece. The runtime
  functions are symbols too, created when the `Ctx` is set up.
- String literal bytes go into a table of data on `Ctx`. A constant of
  type `IT_DATA` refers to the bytes by their index in the table, and the
  length is a `KINT`. Like a `KSYM`, it's an address, and it becomes a
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
  CALL @luv_print_str, @.dat0, 4
  ...
```

- Top level lines are `data` and `func`. Inside a function, `slot` lines
  come first, then a label line `:n` for each block, followed by its
  instructions.
- A `data` line is `data @.datN [align N] "bytes"`. The alignment is only
  printed when it's more than 1, as for a struct's default image.
- An instruction is `[%n =] OP [type] operand, operand, ...`. The type is
  left out when it's `void`, and for `SLOT`, which is always an `i64`
  address. Types are a fixed set of words that no operand can look like.
- A compare's type is its operands' type, which the unsigned compares need
  to know the width. Its result is always an `i32` 0 or 1.
- The first character of an operand says what it is: `%n` an instruction,
  `@name` or `@name+8` a `KSYM`, `:n` a block, `$n` a slot. A `KINT` starts
  with a digit or `-`, and is printed as signed decimal and read at the
  instruction's type. A `KFLT` is a hex float (`0x1.8p+0`), which is
  exact and is read by Python's `float.fromhex`.
- `%n` is the instruction's index in the buffer, so there are gaps where
  `BLOCK` and `EXT` are.
- A call's arguments are all on its line, wherever they're stored. The
  `EXT` slots aren't printed.
- A comment is a whole line starting with `;`, so the reader can skip
  them without looking inside other lines. Comments give the text of each statement's source line, with its indentation, from
  the source position table (D26), and above an instruction with float
  constants, their values in decimal (`; 0x1.8p+0 = 1.5`). Line numbers
  aren't printed, because the lexer only keeps offsets, and finding a line
  number means counting newlines from the start of the file.
- In `data @name "bytes"`, printable ASCII is written as itself except for
  `"` and `\`. Everything else is `\n`, `\t`, `\\`, `\"` or `\xNN`.
- Symbol names can't contain whitespace. Data is `@.datN`, which
  can't collide with a function's name because identifiers can't start
  with `.`.

## Out of the slice

Globals, `mod.func` calls, nested functions, structs, lists, dicts,
comprehensions, `with`, `check`, `cast` of non-constants beyond numeric
conversions, pointers, `foreign` calls, and `str` results from functions.
Comprehensions need extra care later, since D10's rewind re-lexes the
element expression.

## Steps

Each step is split into pieces small enough to review, driven by the tests
that start to pass.

1. `il.c`: the buffer, constants, slots, blocks and jumps, the source
   position table, and the dump, tested from `unittest.c`. No parser
   change.
2. `ExpDesc.kind` replaces `is_const`. No change in behaviour.
3. `parsecheck` becomes `luv parse`, with `parser.c` built twice. Then
   `FuncState`, the callback and `luv il`: `def int main(): return 0`.
4. The Python interpreter and the test runner running supported tests:
   `basic_int.luv`-style `# RET:` tests.
5. Locals, assignment, arithmetic and comparisons.
6. `print` and string literals.
7. `if`/`elif`/`else`, `and`, `or`, `not`.
8. `for` over `range`, `break` and `continue`, `FLOW_JUMPS`.
9. Parameters and calls.

## Decided in review (2026-11-16)

- The driver is `luv`, and `luv parse` replaces parsecheck, but only if it
  costs nothing in parse speed, hence the two builds.
- The interpreter is a Python script reading the dump.
- Parameters (and later captured values) are values, not slots.
- Unsupported constructs are errors in the emitting build.
- SSA concerns are ignored for the slice, and blocks are numbered in
  whichever way is easiest for the front end.
- A function's symbol is in a separate table, not its `TypeInfo`.
- The interpreter is `misc/interp.py`.

## Still open

1. A better name than `FuncState`, if one comes up.
