# D31 Aggregates (proposal)

Written 2026-11-20, for review before any code. It covers structs and
fixed-size arrays as values: their layout, what each target ABI needs to
know about them, how they appear in D26's IL, how they can still end up in
registers, and how the parser emits them. `str` is the first consumer.
`range` needs an iteration protocol, and so method calls, first, and both
are out of scope here.

## What exists now

Struct types aren't typed yet. There's no `TypeInfo` for a struct, fields
aren't recorded, and `sizeof` of a struct takes the non-constant path. The
first step is struct typing in the checking build: fields and their types,
field access, struct literals checked against fields, and the layout below,
which makes `sizeof` and `alignof` of every struct an untyped constant
(D28).

## Layout

Every struct uses C's layout. Fields are in declaration order, each at the
next multiple of its own alignment. A struct's alignment is the largest
alignment of its fields, and its size is rounded up to that alignment.
Fields are never reordered.

- Scalars are naturally aligned. `bool` is 1 byte and pointers are 8.
- `[N]T` is `N * sizeof(T)` bytes with `T`'s alignment.
- `str` is `{*u8 data; int len}`: 16 bytes, aligned to 8.
- A field of struct type needs the full definition (D11), and a struct
  that contains itself by value is an error at its definition.
- A struct with no fields is an error (open question 3).
- Decided (2026-11-20): field defaults (`int x = 5` in a struct body) must
  be constant expressions, like global initializers (D28). A struct with
  defaults then has a constant image, which goes in the data table, and
  creating one is a copy of that image instead of a `ZERO`. A struct
  without defaults is zeroed.
- Each generic instantiation (D21) gets its own layout, computed when it's
  instantiated.
- Nothing in luv is aligned to more than 8, and there's no syntax to ask
  for more. There are no packed structs, bitfields or unions for now
  (unions and packed structs are likely later, see "The summary record").

Since every struct already has C's layout, a struct needs no mark to be
passed to C. This replaces the tentative `foreign struct` from D26. That
mark's other job, rejecting `int` fields in structs passed to C, is left to
a lint.

## What each ABI needs

The three targets are Win64, SysV x64 (Linux) and AAPCS64 with Apple's
changes (macOS). Each section gives the rules for aggregates, then the
register and stack rules they interact with. Everything marked "checked"
was confirmed against clang 18's output for that target, either the LLVM IR
signature or the generated assembly. The rules were also compared with each
ABI's own document (2026-11-20): Microsoft's "x64 calling convention" and
"Stack usage" pages, the SysV psABI's "Parameter Passing" section
(gitlab.com/x86-psABIs, master), AAPCS64 release 2026Q3 (stages B and C,
"Result return"), and Apple's "Writing ARM64 code for Apple platforms".
Rule numbers like C.12 are AAPCS64's. Where a document and clang differ, or
a document doesn't say, the text says so.

### Win64

The first four arguments are passed by position. Argument 1 goes in `rcx`
or `xmm0`, argument 2 in `rdx` or `xmm1`, argument 3 in `r8` or `xmm2`,
and argument 4 in `r9` or `xmm3`, depending on whether it's an integer or a
float. Each position uses one of its pair and the other is unused, so
integer and float arguments aren't counted separately as on SysV and arm64
(checked: in `f(int, double, int, double, int, double)`, the arguments
went in `ecx`, `xmm1`, `r8d`, `xmm3`, then the stack). The fifth and later
arguments take 8-byte stack slots each, narrow integers included
(checked).

- The caller always reserves 32 bytes of shadow space just above the
  return address, for the callee to spill the four register arguments
  into. Stack arguments start after it, at `rsp + 32` at the call
  (checked). `rsp` is 16-byte aligned at the call.
- Integer results are in `rax`, and float results in `xmm0`.
- `rbx rbp rdi rsi r12-r15` and `xmm6-xmm15` are callee-saved (checked:
  a caller spilled `xmm6` around its calls). Unlike SysV, `rdi`, `rsi` and
  the upper `xmm` registers are callee-saved.
- There's no red zone. Microsoft's documentation doesn't use the term,
  but says all memory below `rsp` is volatile and may be overwritten by
  the OS, a debugger or an interrupt handler.
- In a variadic call, a float argument in the first four positions is
  passed in both the `xmm` register and the matching GPR, so that the
  callee can spill all four GPRs and walk the arguments in memory
  (checked: `2.5` as argument 2 was in both `xmm1` and `rdx`).

Aggregates:

1. An aggregate of size 1, 2, 4 or 8 is passed as an integer of that size,
   in the position's GPR or in a stack slot. This applies even when every
   field is a float (checked: `{f32, f32}` was in `r8` and `{f64}` in `r9`,
   not in `xmm` registers).
2. Any other aggregate, including sizes 3, 5, 6 and 7, is copied by the
   caller, and a pointer to the copy is passed in the position's GPR or
   stack slot (checked: `{u8, u8, u8}` and `str` were passed as pointers).
   The callee may modify the copy, and the caller can't assume the copy is
   unchanged after the call. That's how compilers treat it, but
   Microsoft's documentation doesn't say it either way. The documentation
   says the copy "must be 16-byte aligned". clang aligns it only to the
   type's alignment (checked: a `{u8, u8, u8}` copy was at offset 60), so
   aligning it to 16 satisfies both.
3. A result of size 1, 2, 4 or 8 is returned in `rax` as an integer, even
   when it's all floats (checked: `{f64}` came back in `rax`, where a plain
   `f64` comes back in `xmm0`). The documentation adds C++ conditions
   (roughly, a C++03 POD) that every C or luv struct meets. It doesn't
   mention all-float structs, which fall under the same rule.
4. Any other result is written through a hidden pointer that the caller
   passes as argument 1, in `rcx`, which moves every other argument one
   position along. The callee returns the same pointer in `rax` (checked).

There's no register exhaustion rule to speak of. Each argument has a fixed
position, so an aggregate either fits in one GPR or is a pointer, and in
both cases it takes exactly one position. The only thing the classifier
reads is the size.

### SysV x64

Integer arguments go in `rdi rsi rdx rcx r8 r9`, and float arguments go in
`xmm0` to `xmm7`. The two are counted separately, so a function taking an
int and a double uses `rdi` and `xmm0`. There's no shadow space. Stack
arguments take 8-byte slots each, including narrow integers (checked), in
order of increasing address. Integer results are in `rax` then `rdx`, and
float results in `xmm0` then `xmm1`. `rbx rbp r12-r15` are callee-saved,
and all `xmm` registers are caller-saved. Leaf functions may use 128 bytes
below `rsp` (the red zone). In a variadic call, `al` holds an upper bound
on the number of vector registers used.

Aggregates are classified by eightbytes, the 8-byte chunks of the
aggregate at offsets 0, 8, and so on:

1. An aggregate larger than 16 bytes is class MEMORY. The psABI's base
   rule says larger than 64 bytes, and a later rule sends anything over 16
   bytes to memory unless it's a vector type (`__m256`, `__m512`), which
   luv doesn't have. An aggregate with unaligned fields is MEMORY too,
   which can't happen in luv. An empty struct is NO_CLASS, and the psABI
   doesn't say how it's passed. QBE treats size 0 as MEMORY. GCC and clang
   ignore empty structs in C.
2. Otherwise each eightbyte gets a class from the fields that overlap it.
   If any such field is an integer or pointer, it's INTEGER. If all of them
   are floats, it's SSE. Padding doesn't count. With natural alignment no
   field crosses an eightbyte boundary, so each field is in exactly one
   eightbyte. Array elements count as fields.
3. An INTEGER eightbyte is passed in the next GPR. An SSE eightbyte is
   passed in the low 8 bytes of the next `xmm` register, so two `f32`s in
   the same eightbyte share one register (checked: `{f32, f32}` is one
   `<2 x float>` in `xmm0`).
4. If there aren't enough registers left for every eightbyte of an
   argument, the whole argument goes on the stack. The registers it would
   have used stay free for later arguments. The psABI says it as "If there
   are no registers available for any eightbyte of an argument, the whole
   argument is passed on the stack. If registers have already been
   assigned for some eightbytes of such an argument, the assignments get
   reverted." (checked: after five integer
   arguments, a `{i64, i64}` went to the stack and the next `i64` took
   `r9`. The same happened with `xmm7` and `{f64, f64}`).
5. A MEMORY argument is copied by the caller into the outgoing argument
   area on the stack, rounded up to 8 bytes, "at an address respecting the
   arguments alignment". It isn't a pointer to a copy as on Win64. The callee finds it at a fixed offset from its incoming
   `rsp` (checked). It uses no registers.
6. A MEMORY result is written through a hidden pointer that the caller
   passes as the first integer argument, in `rdi`. The callee returns the
   same pointer in `rax` (checked). Other results use the classes above
   with `rax`/`rdx` and `xmm0`/`xmm1`, so `{f64, i64}` comes back in `xmm0`
   and `rax`.

An eightbyte that's only partly filled (the last 4 bytes of a 12-byte
struct) is passed in the low bytes of the register. The rest of the
register is undefined, and the load that fills it must not read past the
end of the object. clang loads such pieces as `i32` or `i24`.

What the classifier reads: the size, and for aggregates of 16 bytes or
less, whether each of the two eightbytes is all floats.

### AAPCS64 and Apple arm64

Integer arguments go in `x0` to `x7`, and float arguments go in `v0` to
`v7` (`s` for `f32`, `d` for `f64`). The two are counted separately, as on
SysV. Results use the same registers as the first argument would: `x0`/
`x1`, or `v0` to `v3`. `x8` is the indirect result register. `x19-x29`
and the low 64 bits of `v8-v15` are callee-saved. `x18` is the platform
register, reserved on Apple. `x29` is the frame pointer, which Apple
requires to always address a valid frame record, and `x30` is the link
register. AAPCS64 has no red zone, but Apple has one of 128 bytes.

An HFA (homogeneous floating-point aggregate) is an aggregate whose fields,
with nested structs and arrays flattened, are all `f32` or all `f64`, and
of which there are 1 to 4.

1. An HFA is passed one field per `v` register, without packing (checked:
   `{f32, f32}` is `s0` and `s1`, where SysV packs it into `xmm0`). This
   includes 32-byte HFAs like `{f64, f64, f64, f64}`, in `d0` to `d3`
   (checked). If there aren't enough `v` registers left, the HFA goes on
   the stack and every remaining `v` register is marked used, so later
   float arguments go on the stack too (checked: after seven `f64`s, a
   `{f64, f64}` and the next `f64` both went on the stack and `d7` was
   unused). The GPRs aren't affected. These are C.2, C.3 and C.6. AAPCS64
   adds that an HFA that isn't in registers is "always passed on the stack
   (never in general-purpose registers)".
2. Any other aggregate larger than 16 bytes is copied by the caller and
   replaced with a pointer to the copy, which then counts as an integer
   argument (checked, B.4). Like Win64, but the cutoff is 16 bytes and the
   size doesn't have to be a power of two.
3. Any other aggregate is rounded up to 8 or 16 bytes and passed in one or
   two consecutive `x` registers as its memory image, first 8 bytes in the
   first register. Floats in a non-HFA go in `x` registers (checked:
   `{f64, i64}` and `{f32, f32, f64}` are both `x0`, `x1`). If there aren't
   enough `x` registers left, it goes on the stack and every remaining `x`
   register is marked used (checked: after seven `i64`s, a `{i64, i64}` and
   the next `i64` both went on the stack and `x7` was unused). These are
   B.5, C.12 and C.13.
4. An aggregate is never split between registers and the stack. On the
   stack it's at an 8-byte aligned address (C.14), and it takes its size
   rounded up to 8 (B.5).
5. A result that isn't an HFA and is larger than 16 bytes is written
   through a pointer that the caller passes in `x8`. `x0` stays the first
   argument, and the callee doesn't return the pointer (checked).
   AAPCS64 says "there is no requirement for the callee to preserve the
   value stored in x8".
6. An aggregate aligned to 16 starts at an even-numbered register (C.10),
   except on Apple, which allows an odd one. luv never has one.

Apple's changes to AAPCS64 that matter here:

- Scalar stack arguments take their natural size and alignment instead of
  8-byte slots (checked: `char, short, char, int` were at offsets 0, 2, 4
  and 8). Apple says "Function arguments may consume slots on the stack
  that are not multiples of 8 bytes", which drops C.16's widening to 8.
  B.5 and C.14 still apply to aggregates, so an aggregate on the stack
  still takes an 8-byte aligned slot rounded up to 8 bytes (checked: a
  `{u8, u8}` between two `char`s was at offset 8). Apple doesn't spell
  this out.
- Variadic arguments always go on the stack, in 8-byte slots, never in
  registers.
- The caller extends integer arguments narrower than 32 bits, where
  AAPCS64 has the callee do it (below).
- "Functions may ignore parameters that contain empty struct types."
  AAPCS64 itself has no empty structs ("all members of a Composite Type
  have nonzero size").

What the classifier reads: the size, and whether it's an HFA of `f32` or of
`f64`. The number of fields is the size divided by the width.

### Narrow integer arguments and results

Not about aggregates, but it's the same ABI step. The ABIs disagree about
who extends `i8`, `u8`, `i16`, `u16` and `bool` to 32 bits (checked, from
what clang's callers and callees assume):

| target | arguments | results |
|---|---|---|
| Win64 | callee extends | caller extends |
| SysV x64 | callee extends by the psABI. clang before 2026-11 relied on the caller having done it (open question 6) | caller extends |
| AAPCS64 (Linux) | callee extends | caller extends |
| Apple arm64 | caller extends | callee extends (clang only) |

What the documents say:

- Win64: "the callee can ignore the upper bits of the register", and "The
  state of unused bits in the value returned in RAX or XMM0 is undefined".
- SysV: bits beyond the type's size "are unspecified", and "the consumer
  side of those values needs to extend them". clang's callees assume the
  caller extended arguments to 32 bits anyway (checked), and clang and
  GCC both extend when calling. That's a known de facto rule that the
  psABI doesn't state.
- AAPCS64: "named integral values must be narrowed by the callee rather
  than the caller", and unused bits of a register are unspecified.
- Apple: "The caller of a function is responsible for signing or
  zero-extending any argument with fewer than 32 bits." Apple's page says
  nothing about results. clang's callees extend them (checked), so a luv
  caller shouldn't rely on it without a better source.

The IL keeps narrow integers extended in an `i32` (D30), so luv always
passes and returns extended values. A narrow parameter or result coming
from C needs a `SEXT`/`ZEXT` except where the table says the other side
did it.

### The checked cases

Arguments, as clang lowered them. Results use the same registers, except
that the "memory" cases become a hidden result pointer.

| fields | size | SysV x64 | Apple arm64 | Win64 |
|---|---|---|---|---|
| `str` (`*u8, i64`) | 16 | `rdi`, `rsi` | `x0`, `x1` | pointer to copy |
| `f32, f32` | 8 | `xmm0` (packed) | `s0`, `s1` | one GPR |
| `f32, f32, f32` | 12 | `xmm0` (packed), `xmm1` | `s0`-`s2` | pointer to copy |
| `f64, f64` | 16 | `xmm0`, `xmm1` | `d0`, `d1` | pointer to copy |
| `f64` x 4 | 32 | memory | `d0`-`d3` | pointer to copy |
| `[4]f32` | 16 | `xmm0`, `xmm1` (packed) | `s0`-`s3` | pointer to copy |
| `f32, i32` | 8 | one GPR | one GPR | one GPR |
| `f64, i64` | 16 | `xmm0`, `rdi` | `x0`, `x1` | pointer to copy |
| `f32, f32, f64` | 16 | `xmm0` (packed), `xmm1` | `x0`, `x1` | pointer to copy |
| `f64, f32` | 16 | `xmm0`, `xmm1` | `x0`, `x1` | pointer to copy |
| `i32, i32, i32` | 12 | `rdi`, `rsi` (low 4 bytes) | `x0`, `x1` | pointer to copy |
| `u8, u8, u8` | 3 | one GPR (low 3 bytes) | one GPR | pointer to copy |
| `i16, u8` | 4 | one GPR | one GPR | one GPR |
| `i64` x 3 | 24 | memory | pointer to copy | pointer to copy |

## The summary record

The three classifiers together need less than QBE's type records hold.
QBE keeps every field because its text IL is all that it sees. Here the
fields are known to typing, so the summary can be computed once, when the
struct's definition ends:

```c
typedef struct AggInfo {
  u32 size;
  u8 align;  // log2
  u8 abi;    // AGG_SSE0, AGG_SSE1, AGG_HFA_F32, AGG_HFA_F64
} AggInfo;
```

- `AGG_SSE0` and `AGG_SSE1` are set for an aggregate of 16 bytes or less
  when every field overlapping that eightbyte is a float, and there's at
  least one field in it.
- `AGG_HFA_F32` or `AGG_HFA_F64` is set when every field (flattened) is
  that float type and there are 1 to 4 of them.
- Win64 reads only `size`, SysV reads `size` and the SSE bits, and arm64
  reads `size` and the HFA bits.

QBE's `isdark`, its union handling, and the SysV classes for `long double`
and vectors (X87, SSEUP) aren't needed, since luv has no packed structs,
unions, `long double` or vector types.

Implemented (2026-11-22) as `il_agg_add` in il.c, which classifies from the
struct's scalars flattened into `AggLeaf {offset, IlType}`. A struct's
`TypeInfo` holds an `AggId`, which replaced its own size and alignment.
Leaves are only collected for structs of up to 32 bytes.

Win arm64 and Linux AAPCS64 need nothing more. Win arm64 differs only in
variadic calls, where floats and HFAs go in GPRs. RISC-V's LP64D (and
LoongArch's) would need a different kind of summary. An aggregate of at most
16 bytes that flattens to one float, two floats, or one float and one
integer is passed field by field in FPRs, or an FPR and a GPR (`{f32, i32}`
is `fa0` and `a0`). That needs each of up to two leaves' class, width and
offset. Since `il_agg_add` already gets the leaves, that's a local change.

C-style unsafe unions and an equivalent of `__attribute__((packed))` are
likely to be wanted eventually, though not soon. The summary record would
need to grow for them:

- Unions: the SysV bits are computed by merging every member's fields into
  each eightbyte, so an eightbyte is SSE only if every member's fields
  there are floats. A union is an HFA only if every member is an HFA of the
  same base type. Win64 still reads only the size.
- Packed structs: a field that isn't at its natural alignment makes the
  whole aggregate MEMORY on SysV, which needs a bit like QBE's `isdark`.
  AAPCS64 still classifies by size and HFA, and Win64 by size. Field loads
  and stores of a packed struct also need unaligned access, which the IL's
  `LOAD`/`STORE` would have to mark.

`[N]T` passed by value between luv functions is classified as a struct of
N `T` fields. C can't pass arrays by value, so `[N]T` in a `foreign`
signature is an error.

## Aggregates in the IL

This refines D26 ("Aggregates are memory").

- A module's `AggInfo`s are in a table on `Ctx`, and the IL refers to them
  by index. A struct's `TypeInfo` holds its index.
- There's no aggregate value type. An aggregate only appears as an address,
  an `i64`.
- `IlSlot` gets a size and alignment, or an `AggInfo` index, so that a
  slot can hold an aggregate.
- A field is `ADD addr, KINT offset`, then `LOAD` or `STORE` of the field's
  type. Nested fields (`a.b.c`) add their offsets in the front end, so
  they're still one `ADD`.
- `ZERO dst, agg` has the `AggInfo` index as its second operand, and
  `COPY dst, src` has it in an `EXT`, since both of its operands are
  addresses. The index gives the alignment as well as the size, so the
  code generator can choose wide moves. The size can't come from the
  instruction's type byte, which only holds an `IlType`, or from the
  destination slot, since the destination can be a field (`a.inner = b`)
  or a pointer (`*p = s`). `COPY` is much rarer than scalar operations,
  so the `EXT` costs little.
- `ARGAGG addr, agg` is an aggregate argument. `PARAMAGG n, agg` is the
  address of aggregate parameter `n`, which is the callee's own copy.
  Aggregate parameters are immutable like other parameters, so their
  fields and elements can't be assigned or have their address taken.
- Decided (2026-11-23): a call that returns an aggregate has
  `RESULTAGG dst, agg` as its first argument, where `dst` is a new
  temporary slot in the caller, and the call's value is that slot. A
  function that returns an aggregate is `func #N @name` (`IlFunc.ret` is
  `IT_AGG`, with `ret_agg`), and returns with `RET addr`, the address of
  the value. The ABI step turns `RESULTAGG` into the hidden pointer, or
  into stores of the result registers after the call, and `RET addr` into
  a copy through the hidden pointer, or loads into the result registers.
  QBE instead has a typed call (`%r =:t call`) whose result is a pointer to
  memory that its ABI pass allocates. That was rejected so that all memory
  is a slot before the ABI step, and so that `dst` could later be the final
  destination when nothing aliases it.
- Captured values (D12) need nothing extra. A nested function that
  captures gets a pointer to a block of its captured values as a scalar
  first parameter (D26). A captured aggregate is copied into that block
  when the nested function is defined, and reading it, or a field of it,
  is an address at a constant offset from that pointer, like any other
  `EK_ADDR`. `ARGAGG` and `PARAMAGG` are only for parameters and
  arguments the user wrote.

Up to the ABI step, all of this is the same for every target.

## Where the ABI step goes (open)

The step turns `ARGAGG`, `PARAMAGG`, aggregate results and narrow
extensions into what the target does. It looks at a whole call, or a whole
parameter list, at once, so it can apply the rules that depend on earlier
arguments (SysV rule 4, arm64 rules 1 and 3) and give every piece its
actual register or stack offset, as QBE's `abi1` does. Everything after it
sees "this value is in `rsi` here", and not the ABI's rules.

The decision is where it runs relative to the SSA pass that promotes
slots:

- **(a) Late, as QBE does.** QBE's `abi1` runs after promotion, load
  forwarding, slot coalescing, GVN and code motion, and nothing after it
  forwards stores to loads. The optimizer sees an aggregate argument or
  parameter as an opaque use of memory, so anything passed or received is
  in memory, and lowering adds stores and loads that are never removed.
- **(b) First, before promotion.** The parser's output is the same as for
  (a). At the end of the function, the ABI step runs first, then the SSA
  pass. Promotion then sees only `LOAD`s and `STORE`s at constant offsets,
  and with the splitting below, a `str`'s data and length can become SSA
  values that go straight into or come straight out of registers.
- **(c) In the parser.** As (b), but `exp_*` splits aggregates when the
  call is emitted. That saves a pass, but puts per-target code in the
  parser and makes `luv il`'s output depend on the target.

(c) is ruled out (2026-11-20). The parser should emit the same IL for
every target, so that the backends can be tested on identical input, and
target details stay out of the parser. The lean is (b).

An example, with notation that's only for illustration: `PARAM i64 rdi` is
the value that arrives in `rdi`, and `CALL @len, rdi=%5, rsi=%6` passes
values in those registers. The luv code is:

```
def int len(str s):
    return s.len

def int caller(*u8 d, int n):
    x = str(d, n)
    return len(x)
```

The parser emits the same IL for every target:

```
func i64 @len
:0
  %1 = PARAMAGG 0, str     # the address of s
  %2 = ADD i64 %1, 8
  %3 = LOAD i64 %2
  RET i64 %3

func i64 @caller
  slot $0 str               # x
:0
  %1 = PARAM i64 0          # d
  %2 = PARAM i64 1          # n
  %3 = SLOT $0
  STORE i64 %1, %3
  %4 = ADD i64 %3, 8
  STORE i64 %2, %4
  %5 = ARGAGG %3, str
  %6 = CALL i64 @len, %5
  RET i64 %6
```

**SysV under (a).** `x`'s address is used by `ARGAGG`, so its slot stays in
memory through the SSA pass. So does `s`, since `PARAMAGG` is opaque. Then
the ABI step runs:

```
len:                        # PARAMAGG became a slot and two stores
  STORE i64 (PARAM i64 rdi), $s
  STORE i64 (PARAM i64 rsi), $s+8
  %3 = LOAD i64 $s+8        # reloads what was just stored
  RET i64 %3
caller:
  STORE d, $0               # from the parser
  STORE n, $0+8
  %5 = LOAD i64 $0          # ARGAGG became two loads
  %6 = LOAD i64 $0+8
  CALL @len, rdi=%5, rsi=%6
```

This is exactly what QBE generates. Compiling the equivalent QBE IL with
`qbe -t amd64_sysv` gives:

```
len:    subq $16, %rsp           caller: ...
        movq %rdi, 0(%rsp)               movq %rdi, -16(%rbp)
        movq %rsi, 8(%rsp)               movq %rsi, -8(%rbp)
        movq 8(%rsp), %rax               movq -16(%rbp), %rdi
        addq $16, %rsp                   movq -8(%rbp), %rsi
        ret                              callq len
```

`-t arm64_apple` is the same, with an address computation for each access.

**SysV under (b).** The ABI step runs first and gives the same rewrite as
above. But then the SSA pass runs, and `$s` and `$0` are only used by
`LOAD`s and `STORE`s at offsets 0 and 8. Each is split into two scalar
slots, and both are promoted. `%3` becomes `PARAM i64 rsi`, and the
caller's `%5` and `%6` become `d` and `n`:

```
len:
  %3 = PARAM i64 rsi
  RET i64 %3                # mov rax, rsi; ret
caller:
  CALL @len, rdi=d, rsi=n   # d and n are already in rdi and rsi
```

**Win64, either way.** `str` is 16 bytes, so it goes by pointer. In `len`,
`PARAMAGG` becomes the incoming pointer, `PARAM i64 rcx`, with no copy (see
below), and the field is a load through it: `mov rax, [rcx+8]`. The caller
has to make a copy, and passes its address:

```
caller:
  slot $t str               # the copy
  COPY $t, $0, 16
  CALL @len, rcx=$t
```

Under (b), `$0` is only stored at constant offsets and copied, so it's
split and promoted. The `COPY` from it becomes two stores of `d` and `n`
into `$t`. `$t` is passed by address, so it stays in memory. That's what
Win64 requires anyway. QBE does the same here, except that `$0` stays in
memory, so `d` and `n` are stored to `$0` and then copied to `$t` through
`rax`.

**Where the pieces don't match the fields.** SysV passes `{i32, i32}` as
one `i64`. After the ABI step, the slot has an `i64` access at offset 0 as
well as the `i32` field accesses at 0 and 4. They overlap differently, so
the slot isn't split. It stays in memory, and the result is the same as
(a). That's correct, just no faster.

**Drawbacks of (b), and why QBE is late.** The ABI step puts physical
registers into the IL. In QBE, that means copies to and from registers
like `rdi`, which aren't SSA values: `rdi` is assigned again before every
call. QBE's passes between promotion and `abi1` (load forwarding, GVN,
global code motion, if-conversion) would all have to leave those copies
alone, or keep them next to their calls. Lowering late keeps physical
registers out of the optimizer entirely, and keeps QBE's optimizer
target-independent. QBE also prints its IL after each pass for debugging,
and before `abi1` it reads the same for every target.

For luv, the costs of (b) are smaller, but they're real:

- The SSA pass runs on target-specific IL, so its output differs per
  target and has to be tested on each. The parser's output, the input to
  every backend, doesn't change.
- Physical registers must not appear as ordinary instructions. The ABI
  step only writes them as attributes: the register of a `PARAM`, and the
  register or stack offset of each of a `CALL`'s arguments and results.
  There's no "copy to `rdi`" instruction until register allocation, so
  the SSA pass never sees a register assigned twice. With only one
  forward pass (promotion, folding and CSE, no code motion), that's
  enough. A later optimized tier that moves code would have to keep the
  same rule.
- The SysV MEMORY argument area, and stack arguments in general, only
  exist at the call, relative to `rsp`. The ABI step needs a way to say
  "store this at outgoing offset 16 for this call". That's needed under
  (a) too, so it's not a cost of (b) as such.
- Pieces that don't match fields (`{i32, i32}` on SysV) leave the slot in
  memory. The ABI step could build the `i64` from the two fields with a
  shift and an `OR`, as clang does, so that the slot is still split.
  That's an improvement for later.

The benefit is mostly for small aggregates passed by value in registers:
`str`, and later ranges, slices and small structs like points. Large
aggregates go through memory on every target either way.

**The callee's copy.** On Win64, and for an arm64 aggregate over 16 bytes,
the callee gets a pointer to the caller's copy. luv parameters are
immutable and their address can't be taken, so the callee can use that
copy directly, and `PARAMAGG` is the incoming pointer. A SysV MEMORY
parameter is the same, at its address in the caller's argument area. The
caller has to make the copy unless its source is a temporary that isn't
used again, like a call result or a literal.

## Promoting aggregate slots

D26 has aggregate slots escaped from the start, so they're never promoted.
With that rule, every `str` local stays in memory, and every read of
`s.len` is a load.

The proposal is to split unescaped aggregate slots into scalars:

- An aggregate slot is escaped only if its address is used for something
  other than a `LOAD` or `STORE` at a constant offset, `COPY`, or `ZERO`.
  With (b), the ABI pieces are `LOAD`s and `STORE`s, so they count as
  constant-offset uses.
- When the function ends, each distinct (offset, type) that an unescaped
  aggregate slot is accessed at becomes its own scalar slot. A `COPY`
  between two such slots becomes per-field loads and stores, and a `ZERO`
  becomes zero stores. Promotion then continues as it does now.
- If two accesses overlap differently (an `i64` at 0 and an `i32` at 4),
  or an array is indexed by a non-constant, the slot stays in memory.

QBE's alternative is load forwarding (`loadopt`). For each `LOAD`, it
searches backwards for the most recent `STORE` to the same address. If
alias analysis can show that nothing in between may write that address,
the load is replaced by the stored value. When the search reaches a join
in the control flow, it continues into each predecessor and inserts a phi
of the values found. Alias analysis is what makes it work: QBE tracks each
pointer's base (which slot or global) and constant offset, and whether the
slot has escaped, so it knows that a store through an unrelated pointer
can't change the value.

Load forwarding is more general than splitting. It works on slots whose
address has escaped, and on memory reached through pointers, as long as
nothing in between may alias. But it's a separate pass with its own alias
information and a search for every load, where splitting is promotion
keyed by (slot, offset) instead of by slot, inside the existing forward
pass. It also wouldn't help QBE with the ABI's stores and loads, which are
added after it runs.

The two don't exclude each other. Splitting covers locals, which are what
`str` and small structs mostly are. Load forwarding could be added to an
optimized tier later, if repeated loads through pointers (`p.x` in a loop
body) turn out to matter.

## The parser

- `ExpDesc` gets `EK_ADDR`: an aggregate, or a field of one, in memory at
  `ref` plus a constant `off`. A field of an `EK_LOCAL` aggregate is
  `EK_ADDR` with the slot's `SLOT` as the base, and a field of an
  `EK_ADDR` adds its offset. A field of an aggregate parameter has
  `PARAMAGG` as the base. A scalar field is only loaded when it's used,
  like `EK_LOCAL`.
- Assigning one aggregate to another is `COPY`. Assigning a field is
  `STORE`.
- A struct literal is built in a new temporary slot, with a `ZERO` (or a
  `COPY` of the struct's default image, if it has defaults) and a `STORE`
  for each field given, and then copied to wherever it goes. A
  call that returns an aggregate gets a new temporary slot as its
  destination.
- Building a literal straight into its destination could be added for
  declarations, but splitting may make it unnecessary: a `COPY` between two
  unescaped slots becomes scalar moves, which promotion removes.
- Temporary slots are never reused within a function (D30). Later, slot
  coalescing can reuse them for slots that stay in memory.

## The interpreter

`misc/interp.py` addresses slots by number now. With aggregates, each frame
gets a `bytearray`, slot addresses become byte offsets (with a base per
frame, so that addresses in different calls differ), and `LOAD`/`STORE`
use `struct.pack` for each type. `COPY`, `ZERO`, `ARGAGG` and `PARAMAGG`
are done directly. The interpreter can't check an ABI, so it only checks
layout, copies and field access.

## Checking the classifiers before there's a backend

D25's test, which passes many struct shapes across a C boundary in both
directions and compares the bytes, needs a code generator. Most mistakes
are in classification, though, and that can be checked sooner. A script
can generate C structs, run `clang -target <triple> -S -emit-llvm`, and
compare clang's lowered signatures (`<2 x float>`, `[2 x i64]`, `byval`,
`sret`, as in the table above) with what our classifier produces for the
same luv structs. The table above came from doing this by hand.

## Steps

1. Struct typing in the checking build, with layout and constant
   `sizeof`/`alignof`. Probably several pieces, driven by tests.
2. `AggInfo` and its bits, with unit tests taken from the checked cases.
3. Aggregate slots, `COPY`, `ZERO`, `EK_ADDR`, field loads and stores,
   struct literals, and a byte-addressed interpreter. The `struct_*` tests
   that don't pass structs to functions start to run.
4. `ARGAGG`, `PARAMAGG` and aggregate results, before any ABI step. The
   interpreter passes them by copying. `struct_small_by_value.luv` and
   `struct_large_by_value.luv` start to run.
5. `str` as `{*u8 data; int len}`: `str` locals, parameters and results,
   and `print` of a non-literal `str`. `print.luv` starts to run.

The classifiers, the clang comparison script, the ABI step and splitting
aggregate slots come with the backend.

## Open questions

1. Where the ABI step goes: (a) or (b). (c) is ruled out. Both lean (b),
   unless a cost comes up that's not listed above.
2. Splitting aggregate slots, or QBE-style load forwarding. The lean is
   splitting now, with load forwarding possible in an optimized tier.
3. Decided (2026-11-20): empty structs are an error. A struct that needs
   no data can have a `u8` field. That avoids the question of matching C,
   where GCC and clang make an empty struct size 0, C++ makes it size 1,
   MSVC's C rejects it, and Apple ignores empty-struct parameters.
4. Decided (2026-11-20): `str` is a builtin type with a fixed layout, at
   least at first. The compiler needs detailed knowledge of it either way,
   so whether its definition is written in the prelude doesn't change
   much.
5. When the caller can skip the copy for an aggregate passed by pointer
   (Win64, arm64 over 16 bytes) or in SysV's argument area. A luv callee
   never writes its parameters, so for luv-to-luv calls the copy is only
   needed if the original could change during the call. That can't
   happen to an unescaped local or a temporary. It can happen to a local
   whose address was taken, if the callee writes through that pointer
   while reading its parameter. A C callee owns the copy and may write
   it, so calls to `foreign` functions always copy.
   Decided (2026-11-20): for now, an aggregate passed or returned by
   value in a `foreign` signature is an error. Calling the basic C
   runtime doesn't need it. The rule doesn't depend on the target, so a
   program doesn't compile on SysV and fail on Win64, where the same
   `str` would go by pointer. Runtime helpers that take a `str` on
   Win64 may need it sooner. Until then they can take the data and length
   as two scalar arguments, as `luv_print_str` does.
6. Decided (2026-11-20): extend narrow integers on both sides, on every
   target. On SysV the psABI says the opposite of what clang does: the
   bits above the type are unspecified, so the callee extends. clang's
   callees historically don't, and rely on the caller having extended to
   32 bits, which clang and GCC callers do. GCC extends on both sides.
   clang changed on 2026-11-16 (llvm-project b1c38d9b, "Fix arg extension
   incompatibility with platform psABI", #236450, based on D124435). It
   added `-msmall-int-abi-compat={platform,clang,both}`, defaulting to
   `both`: the caller extends and the callee doesn't assume it. That
   applies to SysV x86-32 and x86-64 except Darwin and PlayStation, where
   clang's old convention is now the platform ABI. clang 18, used for the
   checks above, predates it. Doing both is safe with every
   compiler. luv callers always extend (they already do), and luv callees
   extend too, at the cost of a `movsx`/`movzx` per narrow parameter (and
   per narrow result returned from C). When it's convenient, the callee's
   extension can go only in D26's second entry point, which every
   function that C can call already has, so that calls between luv
   functions skip it.
