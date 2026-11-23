# D15 Interning and symbol lookup

- One global intern table (open addressing, power-of-two size, keyed on
  `(hash, len, bytes)`) holds both keywords and identifiers, the same as
  Lua 4's `marked`. Each `Name` record has: `kind` (keyword id or `NAME` /
  `TYPENAME`), `len`, `hash`, a pointer to the bytes in the source, and a
  **`binding` pointer**.
- Name resolution via the `binding` pointer: each declaration pushes the
  previous `binding` onto a scope undo-stack and points `binding` at the new
  symbol. Leaving a function pops it. Lookup is one load, versus Lua 4's
  linear `actloc` scan or luv60's per-scope dicts (`notes.txt` profiling
  shows scope lookup and `str_intern` as hot).
- **Alternative to benchmark in step 3: Lua 4's fixed per-function locals
  table.** Lua's `FuncState` has fixed arrays (`int actloc[MAXLOCALS]`
  with 200 entries, `expdesc upvalues[MAXUPVALUES]` with 32), so starting
  a function allocates nothing and nothing grows. `search_local`
  (`lparser.c:187`) scans `actloc` backwards comparing interned pointers,
  then repeats in each enclosing function. Of Lua's limits, these two are
  the ones that help the front end. The others (131k functions per chunk
  from `OP_CLOSURE`'s 17-bit A field, `MAXSTACK 250` from the 9-bit B
  field, the constant-table limit from the 26-bit U field,
  `LFIELDS_PER_FLUSH`) exist for the fixed 32-bit instruction format and
  do nothing for parse speed, so we shouldn't copy them unless our output
  format forces it. `MAXVARSLH`/`MAXPARAMS` cap C-stack recursion in
  multiple assignment, which we don't have.
  - Our scoping makes the scan unusually good: scope is the whole function
    (D6) and captures are by value (D12), so a lookup searches only "this
    function's locals, then globals", never walking enclosing functions.
    The scan stays in a small hot array, while `Name.binding` costs a
    load from the `Name` entry (often still in cache, since the lexer just
    interned it) plus an undo-stack push per declaration and a pop pass at
    function end.
  - Cost of the scan: a hard per-function cap (e.g. 256 locals) with an
    error to match, and lookup that's linear in the local count.
  - Measure both. Dumbbench has three locals per function, which flatters
    the scan, so also bench something with many locals per function.
  - **Result (2026-11-08): chose `Name.binding`.** Both were implemented
    for locals only (binding step 1, with no globals, captures or errors
    yet), and they resolved the same names on every test file (3.75M
    local lookups on dumbbench). `scratch/gen_locals.py` generates the
    many-locals inputs (N locals per function, names drawn from a shared
    pool). Hyperfine means, measured on the laptop:

    | input      | HEAD (no lookup) | `Name.binding` | scan     |
    |------------|------------------|----------------|----------|
    | dumbbench  | 476 ms           | 484 ms         | 494 ms   |
    | locals4    | 82.3 ms          | 83.0 ms        | 90.8 ms  |
    | locals16   | 69.4 ms          | 69.5 ms        | 85.4 ms  |
    | locals64   | 66.1 ms          | 66.2 ms        | 97.4 ms  |
    | locals256  | 66.1 ms          | 66.1 ms        | 170.2 ms |

    The scan loses even at 4 locals, and its cost grows linearly as
    expected. `Name.binding` costs nothing measurable on the many-locals
    files, and about 2–3% on dumbbench (+8–15 ms over several runs).
    That's about the same size as the laptop's run-to-run noise (±10–17
    ms, possibly battery or thermal throttling), so it should be
    re-measured on the desktop. For reference, Lua 4.0.1 on dumbbench:
    1.584 s full `luac`, 1.182 s with code generation stubbed out, 0.821 s
    lex only (builds in `scratch/lua-4.0.1-nocode`).

    Re-measured on the desktop (Ryzen 9 5900XT, Linux, clang 18 `-O2`,
    pinned with `taskset -c 6`), comparing 8b968df with e61a77e. The scan
    wasn't rebuilt there.

    | input      | HEAD (no lookup) | `Name.binding` |
    |------------|------------------|----------------|
    | dumbbench  | 640–651 ms       | 649–653 ms     |
    | locals4    | 104.2 ms         | 106.1 ms       |
    | locals16   | 85.7 ms          | 87.9 ms        |
    | locals64   | 81.3 ms          | 82.9 ms        |
    | locals256  | 80.5 ms          | 82.4 ms        |

    σ is 4–14 ms on dumbbench and under 1 ms on most of the locals files.
    On dumbbench, binding costs 1–8 ms (0.2–1.3%) across four runs. On the
    locals files it costs a steady 1.5–2.4 ms (about 2%), which the
    laptop's noise was hiding. System time is about 30% of the total on
    Linux (about 195 ms on dumbbench, versus 37 ms on the Mac), so the user
    time comparison is the closer one.
  - As implemented: a single `Local locals[1024]` array in the `Parser`,
    shared by enclosing functions, with `func_base` marking the current
    function's first entry. Each `Local` saves the shadowed binding, so
    the array doubles as the undo stack. Name indices are a `NameId`
    struct so they can't be mixed up with offsets. Bindings are undone at
    the end of `parse_file()`, including after an error, because `Name`s
    are shared by every file parsed with the same `Ctx`.
- Hash: something cheap over the bytes already being scanned (FNV-1a,
  or wyhash on ≤16-byte loads given the padding).
- **Result (2026-11-17): the table's layout doesn't matter, so it's shared.**
  The IL needs to intern constants, and `NameMap` was already a second
  open-addressing table, so the intern table was measured to choose one
  layout for all three. Desktop, `lexdump` (the lexer alone, which spends
  about 35% of its time in `intern` on dumbbench), `-falign-functions=64`,
  user cycles averaged over interleaved `perf stat` runs:

  | | dumbbench | betterbench |
  |---|---|---|
  | A: HEAD. `u64` slots of hash tag and index, positioned by the hash's low bits, rehashed from `Name.hash` | 1720M | 2824M |
  | B: as A, but positioned by the tag, rehashed from the slots alone, no `Name.hash` | 1724–1756M | 2782–2841M |
  | D: `u32` slots of only the index, compared through `Name.hash` | 1694–1765M | 2810–2824M |
  | E: B with a ¾ load factor instead of ½ | 1619M | 2863–2903M |
  | F: Verstable, a set of name indices, at ½, ¾ and ⅞ load | 2229–2343M | 3073–3127M |

  - A, B and D are within noise of each other. The spread on dumbbench
    follows code layout: with `-falign-loops=64` too, B is within 0.3% of
    A. All three do the same 19.7M probes on dumbbench. The cost of
    interning is the cache miss on each new name, which none of these
    layouts avoids.
  - The tag in the slot isn't measurable. At half load, there are too few
    collisions on the probe path for skipping the `Name` load to matter.
  - B has 62% fewer dTLB misses than A on dumbbench, from the smaller
    `Name` (24 bytes without `hash`). It also has 1.8M more branch misses,
    from the rehash's test for empty slots, which is a coin flip at half
    load. They roughly cancel.
  - E is faster or slower depending on where a file's name count falls
    relative to a power of 2. dumbbench's 1.25M names fit in 2M slots
    instead of 4M, while betterbench's 206k end at the same size as B and
    probe for longer. Not a general win.
  - Verstable runs 15–23% more instructions. Part of that is its rehash
    calling the hash function, which reads a random `Name`, for every key.
    It also has deletion, high load factors and short chains, which an
    insert-only table at half load doesn't need.
  - Not tried: prefetching the slot when the hash is known (there's
    little lexer work between hashing and probing to overlap it with) and
    Swiss-style control bytes (probes are already about 1.05 slots).

  Decided: B's layout, as one table used for interning, `NameMap` and IL
  constants. A slot is `{u32 tag, u32 val}`, positioned by `tag & mask`, and
  tag 0 is empty. For `NameMap` the tag is the `NameId` + 1, so a matching
  tag is a matching key, and consecutive ids are still in consecutive slots.
  For interning, the tag is 32 bits of the hash and a match is confirmed
  against the key. Growing only needs the slots. Scratch builds are in
  `~/luvr/scratch/d15` on the desktop.
