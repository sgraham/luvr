# D25 Backend and execution model (open)

Investigated 2026-11-14, then revised the same day after the user's answers.
The survey is unchanged. The direction, the IL discussion and the
recommendation near the end reflect those answers. Not yet marked decided.

## What's being asked of a backend

- Native code for x64 and aarch64 on Windows, Linux and macOS. That's six
  OS/CPU pairs and four C ABIs (SysV x64, Win64, AAPCS64, and Apple's
  arm64 variant), plus Windows arm64 as a fifth if it's wanted.
- Compile speed is a feature. luv60's goal was to stay above 1M lines/s
  through to code. The front end here does lex and parse at about 22M
  lines/s (10M lines in 0.45 s), so the backend will be nearly all of the
  compile time whatever is chosen.
- luvr is plain C and is meant to end up as one header. A backend that
  needs C++, Rust, a generator step, or a copyleft licence works against
  that.
- The front end is single pass with no AST ([D1](D01-single-pass.md)). Code is produced in
  source order through an `expdesc` that's discharged late, behind a
  `luaK_*`-style interface. Functions are complete units, and generic
  instantiations are queued until the current top-level item ends ([D21](D21-generics.md)).
  So a function at a time is the natural unit to hand to a backend.

Language features that the backend has to deal with, and that most small
backends don't do for us:

- `foreign` functions and structs passed by value mean the C ABI rules for
  aggregates are needed for every target, including C calling back into luv.
- The current arena ([D23](D23-arrays-lists.md)) is a per-thread, dynamically scoped variable. Real
  TLS is different in every object format and is fiddly in a JIT. A pinned
  register or a hidden context argument avoids all of that.
- Constants that are evaluated at compile time already exist
  (`const_for_array_size.luv`). Anything beyond literal folding needs an
  evaluator inside the compiler.
- Nested functions capture by value ([D12](D12-nested-functions.md)), so there are no closures to
  allocate. That's only a hidden argument.

A budget, to make "fast" concrete: 1M lines/s on `dumbbench.luv` is about
8 µs per function (8 lines, about 35 tokens), or a few hundred cycles per
token. 5M lines/s is under 2 µs per function. Process creation is on a
different scale. Linking a 2 MB object with the system linker took 32 ms
here, and one `clang` invocation is tens of milliseconds before it reads any
input. Anything that starts another process can't be the fast path.

## Measurements

Laptop (noisy), best of 5, on the first 96k lines (12k functions) of
`dumbbench.c`, the C equivalent of `dumbbench.luv`:

| What | Time | Lines/s |
|---|---|---|
| `clang -fsyntax-only` | 82 ms | 1.2M |
| `clang -O0 -c` | 630 ms | 150k |
| `clang -O0 -S` | 807 ms | |
| `clang -c` on that `.s` (10.9 MB) | 909 ms | 105k |
| `clang c.o -o c.exe` (link only) | 32 ms | |

Two things follow. LLVM at `-O0` spends 85% of its time after the front end
and is 100 times slower than our parse. And assembling the text takes
longer than compiling the C did, so `.s` as an interchange format costs more
than the code generator that feeds it, before the linker runs.

luv60 is a prototype in an unknown state, so its numbers are weak evidence
and nothing below depends on them. For the record: its notes have dstogov/ir
at `-O1`, JIT in memory, doing 10M lines in 4.4 s on an M3 (2.2M lines/s
including the front end). Its current sqbe build did the 96k-line slice in
about 0.96 s including a `clang -g` link, and a sample showed about three
quarters of that in `strcmp` under sqbe's symbol handling, with QBE's own
passes (`filllive`, `spill`, `filluse`) a few percent. So that says nothing
about QBE's real speed. It would need the lookup fixed and a re-measure.

## Execution models

| Model | Needs | Compile cost | Gives | Problems |
|---|---|---|---|---|
| Interpret bytecode | Nothing | Lowest. Lua 4's whole code generator was 0.42 s per 10M lines | Runs on every host on day one, a reference for the others, compile-time evaluation | Slow at run time. `foreign` calls and callbacks from C still need per-ABI machine code, or a restricted set of signatures |
| JIT in process | Executable memory, `dlsym`/`GetProcAddress` for foreign symbols | Low. No files, no linker | Compile and run in one step, the base for luv60's "listener" and code reload ideas | No binary to ship. Debuggers see nothing without extra work (GDB's JIT interface, `RtlAddFunctionTable` for Windows unwinding). macOS arm64 needs `MAP_JIT` and write-protect toggling |
| Write an executable directly | A writer per format | Low. One file write | A self-contained toolchain with no linker installed, which matters most on Windows | Can't link static `.o`/`.a` files without becoming a linker. Debug info has to be written by us |
| Emit `.o`/`.obj` | A writer per format, and the system linker | Medium. One linker process | Static linking with C code, and the platform's debug info flow | Needs MSVC or lld on Windows and the Xcode tools on macOS. Linker start-up is the floor |
| Emit `.s` | Assembler and linker | High, see above | Readable output | Slowest of the native routes. Only useful as a debugging dump, which a disassembly printer also gives |
| Emit C | A C compiler | Highest: 150k lines/s at `-O0`, far less optimized | Optimized code from clang/gcc/MSVC without writing an optimizer, every platform a C compiler reaches, debug info through `#line` | Not a fast path. C's undefined behaviour has to be fenced off (`-fwrapv`, `-fno-strict-aliasing`, explicit conversions) |

How hard the direct executable writers are, for a program that only links
against shared libraries:

- **ELF** is the easy one. Program headers, `PT_INTERP`, a dynamic section
  with `DT_NEEDED`, and a GOT filled from relocations. A few hundred lines.
- **PE** is nearly as easy. Headers, sections, an import table per DLL, and
  optionally base relocations. `.pdata` unwind entries are needed for
  anything that unwinds through our frames (debuggers, SEH).
- **Mach-O** is the most work. An executable has to be dynamically linked
  against `libSystem` (there's no stable syscall ABI), needs dyld bind
  information or chained fixups, and on arm64 has to carry an ad-hoc code
  signature, which is SHA-256 over each page plus a small blob. tcc, Go and
  Zig all do this themselves, so it's known to be doable in small code.

A JIT and a direct executable writer are nearly the same thing. Both take
code and data in section buffers with relocation records against a symbol
table. One applies the relocations in memory and the other lays the
sections out behind headers and import tables. Writing `.o` files later is
a third consumer of the same records. So the choice between them doesn't
need to be made up front if the code generator emits into sections with
relocations rather than into "the JIT buffer".

Debug info is the largest hidden cost of every route except C. DWARF line
tables are a few hundred lines, and types and locals are much more. On
macOS the debug info normally stays in the `.o` files and is collected by
`dsymutil`, so a direct writer has to produce a dSYM or embed the sections
itself. On Windows the native format is CodeView in a PDB, and writing PDBs
is a project by itself. None of the small backends below solves this either:
QBE carries line numbers only, MIR and ir have nothing beyond JIT hooks, and
tcc has DWARF on ELF.

## Existing backends

| Backend | What it is | Lang, licence | Targets | In-process API | Output | Compile speed | Code quality |
|---|---|---|---|---|---|---|---|
| LLVM | Optimizing compiler framework | C++, Apache 2 | Everything | Yes | obj, JIT | Slow, see above | Best |
| Cranelift | Optimizing code generator from wasmtime | Rust, Apache 2 | x64, aarch64, riscv64, s390x. ELF, Mach-O, COFF through a Rust crate | Rust only, no maintained C API | obj, JIT | Faster than LLVM, by a small factor rather than an order of magnitude | Good |
| QBE 1.3 (2026-06) | SSA backend that reads a text IL | C, MIT | amd64 SysV, Apple and (new in 1.3) Windows, arm64, arm64 Apple, rv64. No Windows arm64 | No. It's a filter from IL text to assembly text | `.s` only | Limited by text in and text out | "70% of the performance of industrial optimizing compilers in 10% of the code" is its stated aim |
| sqbe | luv60's fork of QBE as a single header with a C API | C, MIT | As QBE | Yes | `.s`, Mach-O and COFF objects, JIT. No ELF object | Not known, see Measurements | As QBE |
| dstogov/ir | JIT framework, the base of PHP's JIT since 8.4 | C plus DynASM, MIT | x86, x64, aarch64. Exercised on all three OSes through PHP | Yes | Memory. Can also print C and LLVM text | Fast. Its README claims up to 40 times faster than `gcc -O2` | README claims about 5% slower than `gcc -O2` on its C benchmarks |
| MIR | JIT with its own IR, an IR interpreter, and a C11 front end | C, MIT | x64, aarch64, ppc64, s390x, riscv64 | Yes | Memory only | Fast | Around `-O1` |
| TinyCC (mob) | One-pass C compiler with its own assembler and linker | C, LGPL 2.1 | x86, x64, arm, arm64, riscv64. ELF, PE, Mach-O | `libtcc`, which takes C source | exe, obj, memory | The fastest C compiler by a wide margin | Poor, about `-O0` |
| xcc | C compiler, assembler, linker and libc | C, MIT | x64, aarch64, riscv64, wasm. README says ELF64 only | No, command-line tools | `.s` through its own `as` and `ld` | Not stated | No optimizer yet (it's on the TODO list) |
| rat | C99 compiler with a sea-of-nodes optimizer and its own linker | C, MIT | x86 only so far. ELF, with COFF and DWARF in progress | No, a `ratcc` driver | obj, exe | README: `-O1` builds `sqlite3.c` about 17 times faster than `clang -O1` | README: within 5% of `gcc -O1` |
| TPDE | Framework for single-pass back ends over an existing SSA IR | C++20, Apache 2 | x64 and aarch64, ELF only | C++ | obj, JIT | 8–24 times faster than LLVM `-O0` on SPECint | About LLVM `-O0`, within ±9% |
| sljit | Portable low-level assembler, used by PCRE2 | C, BSD | x86, x64, arm, arm64, riscv, ppc, mips, s390x, on all three OSes | Yes | Memory only | One pass, very fast | Whatever is asked for. It has no register allocator |
| Tilde (TB) | Sea-of-nodes backend library | C, MIT | Mostly x64 | Yes | obj, JIT | Fast | Aims at `-O1` |
| Copy and patch | A technique: stencils compiled ahead of time by clang, copied and patched at run time | n/a | Whatever clang targets | n/a | Memory | The paper reports about 100 times faster than LLVM `-O0` | Around `-O0`, sometimes better |

Notes on each, in terms of what this project needs.

**LLVM and Cranelift.** Ruled out as the main path for the reasons already
suspected. LLVM is two orders of magnitude larger than the rest of the
compiler would be, and even its fastest mode misses the speed goal by ten
times. Cranelift is a reasonable backend, but it brings a Rust toolchain
and a hand-written C shim, and its compile times are in LLVM's league
rather than tcc's. If optimized output is wanted, emitting C gets LLVM's or
MSVC's optimizer without linking either.

**QBE and sqbe.** This is the only candidate where every ABI except Windows
arm64 already works, and sqbe has already added an API, object output for
two formats, and a JIT. It also produces better code than anything we'd
write quickly. The doubts are speed and size. QBE runs a dozen passes over
a pointer-based SSA IR, and upstream has never had to care about throughput
because it's fed by a text parser. sqbe.h is 24k lines, which would be
several times the size of everything else in luvr. ELF object output and
debug info beyond line numbers would be ours to add. Whether its speed is
acceptable isn't known and is cheap to find out.

**dstogov/ir.** The fastest of the real optimizing backends by luv60's own
numbers, and it's in production on all three OSes. Against it: the
instruction selectors are DynASM `.dasc` files (11k lines for x86, 6.6k for
aarch64) that need a Lua preprocessing step, though the generated C could
be checked in. It has no object or executable output. Aggregates in the C
ABI are left to the caller. Its README says the API isn't stable. luv60
moved from ir to sqbe in June 2025, and the reason isn't recorded in the
notes.

**MIR.** Similar in shape to ir, with a wider set of CPUs and its own
interpreter for the IR. It's JIT only, and its author has said he has much
less time for it. luv60 tried and removed it in January 2025.

**TinyCC.** Its architecture is the closest match there is to this front
end. It's Lua 4 for C: one pass, a value stack, code emitted as the parser
goes, one `*-gen.c` of a couple of thousand lines per CPU, and its own ELF,
PE and Mach-O writers including the arm64 code signature. As a dependency
it fits badly. The supported interface is C source text, so the program
would be lexed and parsed twice. The internal code generator interface is
not an API. LGPL doesn't sit well in a single-header library. The last
release was in 2017, though the `mob` branch is active daily. It's the best
thing to read while writing our own, especially `tccelf.c`, `tccpe.c` and
`tccmacho.c`.

**xcc.** A clean, small, complete toolchain for three CPUs and wasm, but
it's a C compiler that drives its own assembler with text, has no Windows
or (per its README) Mach-O output, and has no library interface. Useful
reading for encoders and for a small linker. Not something to build on.

**rat.** The most interesting as an optimizer: sea of nodes, GVN, SCCP,
inlining, even SLP vectorization, with fast compile times claimed. It's
x86 only, one author, changing daily, and built as a C compiler rather than
a library. Worth watching as a possible optimizing tier in a few years. Not
usable now.

**TPDE.** Not usable (C++, ELF only), but it's the best evidence for a
design: one pass over each function that selects instructions and
allocates registers together, given liveness and loop information, gets
`-O0`-or-better code at 10–20 times LLVM's `-O0` speed. Baseline JITs for
wasm and JavaScript are built the same way.

**sljit.** If only a JIT were wanted, this would be the shortcut. It
supplies encoders, function entry and exit for each ABI, and executable
memory handling including Apple's rules. It can't produce objects or
executables, so it's a dead end for anything that ships a binary.

**Copy and patch.** luv60's `gen.c` "snip" experiments were in this family,
and its notes record the jump-patching trouble. It pays off when each
operation is a large piece of code, as in a dynamic language. For a typed
C-like language most operations are one or two instructions, so a stencil
saves little over a hand-written encoder, and register allocation ends up
fixed by the stencil set. It also puts clang into luvr's own build.

**Tilde.** Mostly x64, development has slowed, and Odin removed its Tilde
backend. Not a candidate.

What other small-team languages did, for calibration:

- Zig wrote its own x64 backend and linkers. It became the default for
  debug builds on Linux in 2025, isn't yet the default on Windows (COFF
  linker work), and aarch64 isn't ready. Release builds still use LLVM.
  Reported debug build times dropped several times over.
- Go has its own backends, assembler and linker for every target, and is
  the standing proof that it scales.
- Hare uses QBE and so doesn't target Windows or macOS.
- V and Nim emit C. V bundles tcc so that development builds are fast.
- Odin and C3 use LLVM, and both tried Tilde as a fast alternative.

The pattern among the ones that care about build speed is the same: an
in-house fast path for development, and someone else's optimizer for
release.

## Writing our own

Rough sizes for the plan recommended below, in lines of C. They're
estimates from the sizes of the same jobs in tcc, QBE and LuaJIT (from
memory), and could easily be off by half in either direction.

| Piece | Lines |
|---|---|
| IL: definition, builder, SSA construction, fold and CSE at emit, dump | 1.5–2k |
| ABI lowering: Win64, Apple arm64, SysV x64, with structs by value | 0.8–1k |
| Register allocation and the selection driver shared by both CPUs | 1–1.5k |
| aarch64 selection and encoding | 1.5–2k |
| x64 selection and encoding | 2–2.5k |
| JIT glue for three OSes (memory, symbols, Win64 unwind tables) | 0.4–0.6k |
| **Fast tier on all three targets** | **7–10k** |
| Optimizing passes, 0.3–0.8k each | 3–5k for a useful set |
| Executable writers: ELF 0.4k, PE 0.6k, Mach-O with signature 1k | 2k |
| Loader for external `.o`/`.obj`/`.a` files, three formats | 2–3k |
| Debug info | depends entirely on the form chosen |

For scale: the front end is about 3.5k lines today, sqbe.h is 24k, and ir
is about 35k plus its DynASM files. So the fast tier would be two to three
times the current front end and well under half of sqbe. It is the
largest single piece of the project.

It doesn't arrive all at once. A first milestone of aarch64 macOS only,
with integers, pointers, branches and calls to luv and to simple foreign
functions, is the IL, one classifier, the allocator and half an encoder:
about 3–4k lines, and it runs most of the existing tests. Floats, structs
by value, x64 and the other two ABIs are each separate steps after that.

Where the size and risk are, and what the fallbacks are:

- The backward pass that allocates and selects together is the least
  conventional part. If it gets complicated at joins and loops, the
  fallback is a liveness pass followed by an ordinary forward linear scan.
  That's about the same amount of code and one more walk over the function.
- SysV x64 struct classification is fiddly but small and completely
  specified, and it can be tested exhaustively against the C compiler.
- x64 encoding is the larger of the two encoders because of its
  irregularities. Restricting the fast tier to a fixed set of instruction
  forms keeps it down.
- If the whole thing proves too much, the IL lowers to QBE's, and sqbe
  becomes the backend with the front half kept.

## Direction from the user (2026-11-14)

- Running in process is fine to start with.
- Targets to begin with are x64 Windows, x64 Linux and aarch64 macOS only.
  Windows arm64 is out for now.
- Differences between backends should be avoided as much as possible. That
  means observable behaviour, and also capability: no "only this mode can
  use external `.o` files" and no "this backend only works on Windows". A
  user should be able to pick any tier and any way of running in whatever
  context suits their project. Two tiers are fine: a fast compile, and a
  slower compile that gives faster code.
- Linking with C objects is wanted on the fast path, but it can be done by
  the user building a host program ahead of time that contains the C code
  and runs the luv code. So the fast path needs no object writer or linker.
- Structs passed by value to and from C are required for `foreign`. This is
  why luv60 moved from ir to sqbe, so ir is out.
- Debug info is deferred unless it blocks something. It may be limited to
  one platform (raddbg only, or PDB with MSVC), or the effort may go into a
  luv debugger with its own simpler format. The user's `dyn_basic_pdb.h`
  writes PDBs for JIT code but only has file and line.
- A C backend is in doubt. Debugging luv through generated C with its
  temporaries and helper calls may be too strange.
- Compile-time evaluation is only basic constant folding.

These change the earlier draft of this section in three ways. The
interpreter loses most of its case: no compile-time evaluation needs it, the
three targets are all hosts that can JIT, it couldn't do struct-by-value
foreign calls without machine code anyway, and it would be one more backend
to differ. The C emitter is a second semantics (undefined behaviour,
evaluation order, float contraction) as well as awkward to debug, so it
moves to "only if a platform we can't generate code for ever matters". And
the wish for one behaviour across tiers means the tiers should be the same
backend with more or fewer passes, which makes the choice of IL the main
design question.

## Keeping the modes equal

The user's rule is that every combination should work: either tier, on
every target, run in process or (later) written out. The plan keeps to it
by making each choice independent of the others:

- A tier is a list of passes over the IL. It doesn't know which CPU or OS
  is being targeted or where the code will go.
- A target is an ABI classifier plus an encoder. Both tiers go through the
  same ones.
- A consumer (JIT, later an executable or object writer) takes sections
  and relocations. It doesn't know which tier produced them.
- Anything to do with foreign code belongs to the layer that resolves
  symbols, below all three. To begin with there's one rule everywhere: C
  code lives in the host program or a shared library. If loading `.o`
  files is added, it's added there, as a reader that turns an object file
  into the same sections and relocations, so the JIT and any executable
  writer get it together. An executable writer that could link objects when
  the JIT couldn't, or the reverse, is what to avoid.

The practical consequence for sequencing is that a feature isn't done until
it works on all three targets and both tiers, and the test runner should
run every test in every combination that exists.

## Where this is headed

The user's reference for where the project should end up is the Tomorrow
Corporation tech demo (https://www.youtube.com/watch?v=72y2EC5fkcE). From
memory of that talk, not re-checked: their own language, compiler and
debugger, a whole game rebuilt in a fraction of a second, code reloaded
into the running game, and the full run recorded so that execution can be
scrubbed and stepped backwards. luv60's README goals (saved inputs,
incremental code update, debugger integration) are the same picture.

What that asks of the backend, to keep in mind now though none of it is
built yet:

- Functions have to be replaceable one at a time in a running program. The
  per-function IL and per-function code with retained relocations already
  suit this. Calls between luv functions would go through something
  patchable (a table, or a jump at the old entry) in the development tier.
- Globals and types need a story for when their layout changes under a
  running program, which luv60's notes had started on. That's a language
  and runtime question more than a backend one.
- Recording and replay want all input and nondeterminism to come through
  the host, which fits the host-program model.
- A debugger of our own makes the debug info format ours to choose. That
  supports deferring PDB and DWARF, and it means the information the IL
  and the fast tier keep (source offsets, where locals live) is the real
  requirement.
- It favours the JIT and in-process model as the main one, with written
  executables as the shipping step.

## The IL

The single-pass front end doesn't force a push/pop code generator. The
front end produces the IL in one pass, a function at a time, and what
happens to a finished function is separate. Three levels:

1. **No IL: machine code from the parser** (tcc). Locals in stack slots, a
   value stack for expressions. Nothing can be improved afterwards, and a
   better tier would be a different backend.
2. **Register bytecode, not SSA** (Lua). Virtual registers are locals and
   expression temporaries. What's affordable without SSA: constant folding
   in `expdesc` before anything is emitted (Lua 4 does this), fusing
   compares with branches and folding address arithmetic into addressing
   modes at selection time, and giving the most used locals a register for
   the whole function from use counts weighted by loop depth. That's well
   ahead of push/pop code. It stops there, though: with no def-use
   information there's no CSE, no dead store removal, and nothing for a
   second tier to build on.
3. **Linear SSA in a flat array, built during the single pass.** This is
   the shape of LuaJIT's IR, which dstogov/ir also follows: fixed-size
   instructions in one array, operands are indices, constants in the same
   array growing the other way, reset per function. SSA can be built
   directly by a single-pass front end with no dominance computation
   (Braun et al., "Simple and Efficient Construction of Static Single
   Assignment Form", 2013, which is what Cranelift's front end uses): each
   block keeps a small map from local to current value, and a read that
   misses asks the predecessors and adds a phi where they disagree. Loop
   headers get provisional phis that are resolved when the loop ends. It's
   a few hundred lines.

What level 3 gives nearly for free, in the fast tier:

- Folding and algebraic simplification as each instruction is emitted.
- CSE by hashing each instruction as it's emitted, within the dominating
  code that's already been seen.
- Dead code removal from use counts.
- Register allocation, instruction selection and encoding in **one backward
  pass** over the array, as LuaJIT's assembler does. Going backwards, a
  value's last use is seen first, so liveness needs no separate analysis,
  and each instruction can see how its result is used (as an address, as a
  branch condition) when it's selected. Control flow joins and loops need
  more care than in a trace compiler, which is the part TPDE's design
  shows how to do.

What the slower tier adds, on the same IL and through the same selection,
allocation and ABI code: inlining, moving loop-invariant code, global value
numbering, load and store forwarding, a better allocator. These are
ordinary passes that can be added one at a time, and each can be turned off
to find a miscompile. Both tiers share everything that defines behaviour,
so they differ only in which rewrites ran.

Level 3 is recommended. It costs more than level 2 up front (the SSA
construction, phi handling in the allocator) and compiles somewhat slower.
The budget has room: ir, which has this shape plus a real pass pipeline,
was the fastest backend luv60 tried. It's also the level at which "fast
tier" and "optimized tier" are one backend.

Struct passing is handled the way QBE does it, which is known to work for
this language. Aggregates are memory in the IL. A call or parameter that
takes a struct by value says so with the struct's layout, and a per-target
ABI step rewrites that into registers, stack slots and hidden pointers
before selection. Three classifiers are needed for the first targets: Win64
(simple, by size), Apple arm64 (homogeneous float aggregates, 16-byte
rule), and SysV x64 (the eightbyte classification, the hard one). A
generated test that passes many struct shapes both ways across a C boundary
and compares the bytes would be written with it. sqbe stays useful here as
something to compare against.

For debug info, whichever way it goes later, two things are worth keeping
from the start because they're cheap now and awkward to add: every IL
instruction carries a source offset, and the fast tier can report where each
named local lives.

## Recommendation (revised 2026-11-14)

1. **A linear SSA IL per function**, with folding and CSE as it's built.
   [D26](D26-il.md) has the design, and explains why SSA is made by one pass at the end
   of each function and not while parsing. Expression typing produces this. A text
   dump of it is the first testable output.
2. **One backend of our own: ABI lowering, then a backward pass that
   allocates registers, selects and encodes**, for x64 and aarch64, emitting
   into sections with relocation records.
3. **A JIT as the only consumer to begin with.** The library compiles and
   runs in process. Foreign symbols are resolved by the host, through a
   callback on Ctx in the same style as [D24](D24-modules.md)'s file loader, with `dlsym` and
   `GetProcAddress` as the default. C code is linked into the host program
   by the user. Order of bring-up: aarch64 macOS (the laptop), x64 Linux
   (the desktop), then x64 Windows, which adds only Win64 and unwind tables
   to the x64 work.
4. **The optimized tier is passes on the same IL**, added when there's
   real code that's too slow. Nothing needs to be designed for it now
   beyond choosing SSA.
5. **Later, as needed:** executable or object writers as further consumers
   of the same sections, debug info in whatever form is chosen, and a C
   emitter only for platforms without a native generator.

Not planned: an interpreter, `.s` output, any third-party backend as a
dependency.

sqbe is the fallback and the comparison. If the backward-pass generator
turns out harder than expected, lowering this IL to QBE's is mechanical,
since both are SSA with the same view of aggregates. It also remains the
quickest way to check our ABI lowering and code quality against something
independent.

What this gives up:

- The first native "hello world" is further away than with sqbe, since the
  IL, one ABI, one encoder and the allocator all have to exist first.
- Until the optimizing passes exist, code is somewhere between tcc and
  `-O1`.
- Two instruction selectors and three ABIs are ours to get right. With no
  interpreter, the checks are the tests' expected output, the two CPUs
  against each other, the fast tier against the optimized tier, and the
  struct-passing test against C.
- Standard debuggers see nothing in JIT code until debug info is done.

## Still open

1. The exact IL. See [D26](D26-il.md) (provisional).
2. Whether the current arena ([D23](D23-arrays-lists.md)) is a pinned register or a hidden
   argument. [D26](D26-il.md) proposes a pinned register that holds the context, with
   the arena as a field of it.
3. How good the fast tier's code needs to be for interactive use, which
   decides how early the optimizing passes are wanted.
4. Debug info: platform and format, or our own debugger. Deferred.
5. Whether a shipped executable without a host program is ever needed, and
   so whether the executable writers get written.
6. Whether loading external `.o` files is wanted, beyond C code in the host
   program or a shared library. If so it's a loader below every consumer.

## Sources

- QBE: https://c9x.me/compile/ and https://c9x.me/compile/releases.html
- TinyCC: https://bellard.org/tcc/ and https://repo.or.cz/tinycc.git
- xcc: https://github.com/tyfkda/xcc
- rat: https://github.com/hexratcc/rat
- ir: https://github.com/dstogov/ir and https://wiki.php.net/rfc/jit-ir
- MIR: https://github.com/vnmakarov/mir
- TPDE: https://arxiv.org/abs/2505.22610 and https://github.com/tpde2/tpde
- Copy and patch: https://arxiv.org/abs/2011.13127
- SSA construction: Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau, "Simple and
  Efficient Construction of Static Single Assignment Form", CC 2013
- Tilde: https://github.com/RealNeGate/Cuik and
  https://forum.odin-lang.org/t/tilde-backend-got-removed/1697
- Zig's x86 backend: https://ziggit.dev/t/self-hosted-x86-backend-is-now-default-in-debug-mode/10447
  and https://mitchellh.com/writing/zig-builds-getting-faster

Not checked against a primary source, from memory: Cranelift's lack of a C
API and its relative compile speed, sljit's and MIR's target lists, tcc's
per-file sizes and its Mach-O arm64 support, what Go, Hare, V, Nim, Odin and
C3 use, and the descriptions of LuaJIT's IR and backward assembler pass and
of Cranelift's use of Braun's algorithm, and the contents of the Tomorrow
Corporation demo.
