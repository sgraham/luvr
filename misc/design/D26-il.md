# D26 The IL (provisional)

Written 2026-11-14, before expression typing existed. The raw stage, the
dump and the parser's side of it exist ([D30](D30-emission.md)), and
aggregates are in [D31](D31-aggregates.md). The SSA pass and everything
after it are still a plan. The parts most likely to change are listed at
the end.

The two references are LuaJIT's IR and dstogov/ir, which follows it. Both
keep a function's instructions in one flat array, refer to operands by
index, and keep constants in the same index space. LuaJIT's instructions
are 8 bytes with 16-bit operands, which limits a trace to 64k entries.
ir's are 16 bytes with 32-bit operands and room for a third operand.

## Summary

- One function at a time. All of it is reset when the function is finished.
- Instructions are 8 bytes: an opcode, a type, and two 24-bit operands.
- Operands are references, either to an instruction or to a constant.
- Value types are `i32`, `i64`, `f32` and `f64`. Narrower integers exist
  only in memory and in extension operations.
- Aggregates are memory. The IL has no struct values
  ([D31](D31-aggregates.md)).
- The front end never emits a phi. It reads and writes locals as frame
  slots. One forward pass at the end of the function turns slots whose
  address was never taken into SSA values, and folds and removes common
  subexpressions as it goes.
- Blocks are in source order, and control flow is structured.
- The luv context pointer is in a pinned register.

## Two stages

The earlier plan was to build SSA while parsing. That doesn't work for one
reason: whether a local can be an SSA value depends on whether its address
is ever taken, and `&x` can come after `x` has been read and written many
times. Switching `x` to memory at the `&x` is unsound in a loop. A pointer
from an earlier iteration can write the slot before the `&x` is reached
again, and the code between the loop header and the `&x` was already
emitted reading the SSA value.

So there are two stages, which is also what ir does with its `VAR`,
`VLOAD` and `VSTORE` and a later pass:

1. **Raw.** The parser appends instructions to a buffer. Every local is a
   frame slot, read with `LOAD` and written with `STORE`. Nothing is folded
   or searched for, so emitting is an append. Constant expressions are
   already folded in `expdesc` before anything is emitted, as Lua 4 does,
   and that's also what evaluates `[BLAH]int`.
2. **SSA.** When the function ends, one forward pass copies the raw buffer
   into the final one. A slot that was only ever loaded and stored directly
   is promoted: its stores define values, its loads are replaced by the
   current value, and phis are placed where control flow joins. Each copied
   instruction goes through folding and CSE. Slots that weren't promoted
   keep their loads and stores.

The parser is still single pass. The cost is one more linear walk and a
second buffer of the same size. Both buffers are reused for every function.

The front end not emitting phis keeps it simple. A value that depends on
control flow, like the result of `a and b` or a comprehension's
accumulator, is a compiler temporary slot that's stored on each path and
loaded after the join. The SSA pass turns that into a phi like any other
local.

## Could the SSA pass be removed?

Considered 2026-11-15, and not done. Disallowing `&x` on scalar and pointer
locals would make it possible to build SSA while parsing, since that's the
only way a local's escape is found late. But:

- It doesn't make the backend one pass. The backward code generation pass
  needs the whole function, so the walks per function go from three to
  two, not to one.
- The SSA pass knows each loop's stored list before it reaches the loop
  header, so its phis are exact. Building SSA while parsing needs a
  provisional phi for every local read in a loop, as in Braun et al., and
  code already emitted in the body can't fold or CSE through them.
- The parser would have to emit phis for `and`, `or` and comprehension
  accumulators itself, and the folding, CSE and phi lookup would move into
  it rather than going away.
- The language cost is C interop: `strtol(s, &end, 10)`,
  `sqlite3_open(path, &db)` and other out-parameters.

The expected cost of two stages is minor, though not measured. Both
buffers are walked sequentially, 8 bytes per instruction, and the IL is
about the size of its source, so the pass streams about as many bytes as
the lexer did (a streaming read is 0.05 ns/byte against 1.81 ns/byte for
the lexer, [section 5](lexer.md)). Both buffers are reused per function, so
only generated functions near 1M instructions reach memory. The random
accesses (the CSE chain, per-block slot definitions, the raw-to-final
reference map) are what to watch in long functions, and the number of
passes doesn't change those. A bench input that's one function of a few
hundred thousand statements should be added once the pass exists.

## Instruction format

```
typedef u64 Ins;   // op:8 | ty:8 | a:24 | b:24
typedef u32 Ref;   // 24 bits used
```

One `u64` with shifts, not bit-fields, so the layout doesn't depend on the
compiler.

Bit 23 of a `Ref` (`REF_INS`) says that it's an instruction, and the other
23 bits are its index in `ins`. Otherwise, it's the constant at
`consts[ref - 1]`, and reference 0 means no operand. Instructions and
constants are in separate arrays. That gives 8M instructions and 8M
constants per function. Running out is an error, "function too large".
Every constant's reference is below every instruction's, which the CSE
search relies on (see below).

Smaller was considered. LuaJIT's 16-bit operands would cap a function at
32k instructions, which generated code would hit. 16 bytes as in ir would
double the memory that every pass walks, for the sake of a third operand
that few operations need.

Operations with more than two operands (calls, phis, block copies) are
followed by extension slots. Each is an `Ins` with opcode `EXT` holding
two more references. The count comes from the instruction itself (the call
descriptor, the block's predecessor count). Passes that walk the array skip
`EXT` by its opcode.

Nothing else is stored in the instruction. Per-instruction data that only
some stages need is in parallel arrays indexed by reference: the CSE chain
during the SSA pass, and the register and spill slot during code
generation. LuaJIT keeps these in the instruction's fourth 16-bit field.
We've spent those bits on operand range instead.

To revisit, when the SSA pass and a code generator exist and the
long-function bench can measure them:

- (2026-11-15) Whether a third field in the instruction, for the CSE link
  or the register and spill slot, is worth a smaller operand range, so
  that a chain step or an operand lookup touches one cache line instead of
  two. LuaJIT's layout (8/8/16/16/16) allows 32k instructions, which the
  raw stage would reach in a function of a couple of thousand lines.
  6/4/18/18/18 allows about 131k, but caps the opcodes at 64 and the types
  at 16. Either way, the chain walk should be bounded as LuaJIT's is:
  chains per opcode, stopping below the larger operand reference.
- (2026-11-17) One array for both, with constants as instructions, as
  LuaJIT does: constants grow downwards from a bias and instructions
  upwards. Reserving 128 MB of address space per buffer and committing
  pages as they're used would avoid moving anything. Then `ins_op()` and
  `ins_type()` work on any operand without checking which kind it is, and
  the separate type array goes away, which matters for folding and the
  code generator. `Ref` is opaque outside `il.c`, so only `il.c` changes.

## Types

| Type | Used for |
|---|---|
| `i32` | `bool`, `i8`–`i32`, `u8`–`u32` once loaded |
| `i64` | `i64`, `u64`, `int`, pointers |
| `f32`, `f64` | floats |
| `i8 u8 i16 u16` | only as the type of `LOAD`, `STORE`, and the extensions |
| `void` | instructions with no result |

This follows QBE. Signedness belongs to the operation (`DIV` and `UDIV`,
`LT` and `ULT`, `SHR` and `SAR`), not the type. Arithmetic on a narrow type
is done in `i32`, and the front end emits a `SEXT8`/`ZEXT8` or similar
where the narrow result is observed. Folding removes the redundant ones.
The encoders then only need 32-bit and 64-bit forms.

A pointer isn't a separate type from `i64`. All three targets are 64-bit.

## Constants

A constant is 64 bits of payload in `consts`, with its `IlType` in a
parallel byte array. The type says what kind it is:

- an integer type: `KINT`, an integer of either width.
- a float type: `KFLT`, a float of either width, as its bits.
- `IT_SYM`: `KSYM`, the address of a symbol (a function, later a global),
  by its index in `Ctx.syms`. These become relocations.
- `IT_DATA`: the address of data in `Ctx.data`, such as string literal
  bytes or a struct's default image. Also a relocation, but it doesn't need
  an interned name.

Constants are interned per function through a small hash, so equal
constants have equal references and comparing references is enough for CSE.

## Slots

A slot is a piece of the function's frame. The slot table holds size,
alignment, a scalar type if it has one, and an `escaped` flag.

- `SLOT k` is an instruction whose value is the slot's address.
- A named scalar local is a slot. Reading it is `LOAD ty, SLOT k`, and
  writing is `STORE ty, value, SLOT k`.
- Parameters and captured values are not slots. They're immutable and their
  address can't be taken, so that they can always be values: `PARAM n` for
  a parameter, used directly wherever it's read.
- When the front end uses a slot's address for anything other than the
  address operand of such a `LOAD` or `STORE` (`&x`, passing it, indexing
  into it), it sets `escaped`. Aggregate slots were planned to be escaped
  from the start. [D31](D31-aggregates.md) proposes splitting unescaped
  ones into scalars instead.
- A slot that isn't escaped when the function ends is promoted.

The front end also keeps, for each loop, the list of slots stored inside
it. A nested loop's list is added to its parent's when it closes. The SSA
pass uses these to place phis at loop headers without guessing.

A read of a promoted slot before any write produces zero, which matches the
language's zero initialization.

## Blocks and control flow

`BLOCK` starts a block and is the only way one starts. Its operand is an
index into the block table, which holds the block's first instruction, its
predecessor list, its loop depth, and a loop header flag. A block ends at
the next `BLOCK`.

Terminators:

- `JMP b`: go to block `b`.
- `BR c, b`: if `c` is zero go to block `b`, otherwise continue into the
  next block. `BR` is always the last instruction of its block.
- `RET` and `RET v`.
- A block with no terminator continues into the next one.

`BR` with one target and a fall-through matches both how the parser emits
(jump over the body if the condition is false) and what the encoder
produces (a compare and one conditional jump).

Two invariants, which hold because luv only has structured control flow
(`if`, `for`, `break`, `continue`, `return`, `and`, `or`, `with`):

1. Blocks are in source order.
2. Every predecessor of a block comes before it, except for the back edges
   of a loop, which go to a block flagged as a loop header.

A lot of the design leans on these. The SSA pass can be one forward walk.
The immediate dominator of each block can be found in the same walk, since
for a graph like this one visit in this order is exact. The code generator
can use loop depth for spill decisions without finding loops. If the
language ever gets `goto`, these need revisiting.

## The SSA pass

For each block in order, and each instruction in it:

- `STORE` to a promoted slot records the value as the slot's current
  definition in this block and emits nothing.
- `LOAD` from a promoted slot is replaced by the slot's current definition.
  If this block has none, it's looked up through the predecessors, as in
  Braun et al.'s algorithm: one predecessor means asking it, and several
  mean asking each and creating a `PHI` in this block if they differ. The
  answer is recorded in the block so it's only found once.
- At a loop header, a `PHI` is created up front for each promoted slot in
  the loop's stored list. Its back-edge operands are filled in when the
  loop's last block has been processed. Because the stored list is known,
  there are no provisional phis to remove afterwards.
- Anything else is copied with its operands mapped to their new references,
  through the folder and then the CSE lookup.

Folding covers constant operands, identities (`x + 0`, `x * 1`, `x & x`),
and extensions of values that are already in range. CSE applies to pure
operations only. A match found through the hash chain is used if the block
that defines it dominates the current block. Loads are not matched in the
fast tier, since that needs to know what a store might alias.

`PHI`'s operands are in the order of its block's predecessor list, in
extension slots when there are more than two.

Dead code isn't removed in this pass. The code generator walks backwards,
so it reaches a value's uses before its definition, and a pure instruction
that nothing has asked for by then is skipped. Its operands are then never
asked for either.

## Operations

Provisional. The exact set of conversions and comparisons will follow from
expression typing.

| Group | Operations |
|---|---|
| Integer arithmetic | `ADD SUB MUL DIV UDIV REM UREM NEG` |
| Bitwise | `AND OR XOR NOT SHL SHR SAR` |
| Float arithmetic | `FADD FSUB FMUL FDIV FNEG` |
| Integer compare, result `i32` 0 or 1 | `EQ NE LT LE GT GE ULT ULE UGT UGE` |
| Float compare | `FEQ FNE FLT FLE FGT FGE` |
| Width | `SEXT8 SEXT16 SEXT32 ZEXT8 ZEXT16 ZEXT32 TRUNC` |
| Int and float | `ITOF UTOF FTOI FTOU FEXT FTRUNC BITCAST` |
| Memory | `LOAD STORE SLOT COPY ZERO` |
| Control | `BLOCK JMP BR RET PHI` |
| Calls | `PARAM PARAMAGG CALL ARGAGG RESULTAGG` |
| Other | `CTX EXT NOP` |

The operand type is the instruction's type for arithmetic. For compares
and conversions, where the result type differs from the operand type, the
opcode says which is meant, so some of these will split by width (`EQ` and
`EQ64`, or a type on the operand side). That's to be settled with the first
encoder.

Address arithmetic has no operations of its own. A field access is `ADD
ptr, KINT offset`, and an index is a `MUL` and an `ADD`. The code generator
sees these when it selects the `LOAD` or `STORE` that uses them and turns
them into an addressing mode.

`COPY dst, src` and `ZERO dst` take an `AggId`, which gives the size and
alignment ([D31](D31-aggregates.md)). They're how aggregates are assigned
and initialized.

A static table gives each opcode its operand count, whether it's pure,
whether it's commutative (for CSE to order operands), and whether it ends a
block.

## Calls and the calling convention

Calls between luv functions use the platform's C calling convention. That
means one convention to implement and test per target, and it means C can
call a luv function with the signature it appears to have.

`CALL callee, n` is followed by its `n` arguments in extension slots. The
callee is a `KSYM` or a computed value, and the instruction's type is the
scalar result type, or `void`. Variadic calls will need to say where the
variadic arguments start, since they're passed differently on Apple arm64
and Win64.

Aggregate arguments, parameters and results are `ARGAGG`, `PARAMAGG` and
`RESULTAGG` ([D31](D31-aggregates.md)). Before code generation, an ABI step
for the target rewrites them, and narrow integer extensions, into what the
convention says: which registers, which stack offsets, which aggregates go
by hidden pointer.

A nested function that captures ([D12](D12-nested-functions.md)) takes a pointer to its captured
values as an extra first argument. That's part of its type, so a call to it
through a function value passes it too.

## The context register

Confirmed 2026-11-23 ([D32](D32-prelude.md)), over a thread-local that the
runtime owns or an implicit context parameter.

One callee-saved register is reserved in all luv code for a pointer to the
thread's luv context: `r15` on x64 and `x28` on aarch64 (provisional
choices, both callee-saved in every ABI we target). `CTX` is the
instruction whose value is that register. The current arena ([D23](D23-arrays-lists.md)) is a
field of the context, so finding it is one load, and `with Arena()` and
`into a:` store that field and restore it on the way out.

The register holds the context, not the arena, so that it never changes
while a thread runs luv code. That's what makes C interop work:

- **luv calling C.** The register is callee-saved, so C preserves it.
  Nothing is done at the call.
- **C calling luv**, as the host entering luv code or as a callback through
  a function pointer. C doesn't have the context in that register, and by
  the C convention the luv function has to preserve whatever was there. So
  a luv function that C can reach has a second entry point that saves the
  register, loads the context, and continues into the body, with the
  register restored on return. The context comes from a function in the
  runtime that returns the calling thread's context. The runtime is C and
  is compiled into the host, so thread-local storage is the C compiler's
  job and our code generator never emits it.
- **Which functions get the second entry.** A function whose address is
  taken, or that's exported to the host. Direct calls between luv
  functions use the normal entry and pay nothing.

A hidden context argument on every function was the alternative. It keeps
all registers available, but every luv function then has a signature that
differs from what it appears to be, so every function pointer handed to C
would need a thunk with the same problem of finding the context, and every
luv-to-luv call would pass one more argument. The pinned register costs
one of fifteen usable registers on x64 and one of about twenty-eight on
aarch64.

## Source positions

A side table of (reference, source offset) pairs, with an entry each time
the front end starts a statement. The SSA pass carries it across. That's
enough for line tables and for reporting where a fault happened, whatever
form debug info takes later ([D25](D25-backend.md)). The slot table keeps each named local's
name, so the code generator can report where it lives.

## The dump

A text form of both stages, one instruction per line.
[D30](D30-emission.md) gives the format, which `misc/interp.py` reads.

## What's likely to change

1. The operation list, especially conversions and how operand width is
   expressed for compares.
2. Whether strings and lists ([D23](D23-arrays-lists.md)) are ordinary memory operations or get
   operations of their own, for bounds checks for example.
3. Whether the SSA pass should rewrite in place, with a forwarding array,
   instead of copying to a second buffer. To be measured.
4. How `check` and other statements with runtime support are expressed.
   Probably as calls into the runtime, as `print` is ([D23](D23-arrays-lists.md), "For the
   vertical slice").
5. Overflow and division behaviour. The IL operations wrap, and anything
   stricter would be added operations or explicit checks.
6. The register choices for `CTX`, once the allocators exist.
7. Phi handling in the backward code generation pass, which isn't designed
   yet and may ask for something different from the SSA pass.
