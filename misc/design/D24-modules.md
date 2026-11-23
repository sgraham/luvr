# D24 Modules and imports

Decided 2026-11-13, implemented 2026-11-14 (steps 1–5 below).

**Canonical names.** Each source file is a module. Its canonical name is
its path relative to the root (the main file's directory), normalized
lexically, without `.luv`. An import path is relative to the importing
file's directory, so joining the two and collapsing `.` and `..` gives the
canonical name. `import "x"` in a root file and `import "../../x"` in
`sub/dir/y.luv` are both the module `x`. A path that goes above the root
normalizes to `../x`, which is still a consistent key. Normalization doesn't
touch the filesystem, so a file reached through two symlinked paths is two
modules.

**Loading.** Ctx has a dict from canonical name to `Module`. Importing a
module that's already in the dict only binds the name (or `as` alias) to it,
and nothing is parsed again. Otherwise the module is loaded and parsed
completely before the importer continues past its import list. Imports come
first in a file (D17), so at that point the importer has no locals, and no
globals other than the module names it has already imported. Importing a
module that's still being loaded is an import cycle, and an error.

Files are read through a callback on Ctx that returns a buffer followed by
`LEX_PADDING` zero bytes, or fails ("Couldn't open import, looking for
'test/errors/nopes.luv'."). The library itself has no stdio (D16).
`parsecheck` supplies a callback built from its `load_file()`.

**Module and Parser.** A `Module` lasts as long as the Ctx. It has the
canonical name, the source, a loading/done state, and the table of
globals. The source is kept so that generic bodies can be re-lexed. A
`Parser` is only the state of parsing, and points at the Module it's
currently in: the lexer, locals, `func_depth` and `type_params`.

The source is a `FileBuf`, which Module owns. `LexState` has a copy of the
Module's `FileBuf`, which is only changed when the Parser switches modules.
The lexer reads `buf` and `end` on every token, so it uses its copy rather
than going through a Module pointer, and `lex.c` doesn't need to know about
Modules.

```c
struct FileBuf {
  const char* filename;  // for errors
  const u8* buf;         // followed by LEX_PADDING zero bytes
  const u8* end;         // buf + len
};

struct Module {
  const char* path;  // canonical name, also the key in Ctx's dict
  FileBuf src;       // kept so generic bodies can be re-lexed
  u8 state;          // loading or done, to detect import cycles
  // table: NameId -> Global
};
```

**One active Parser.** `parse_file()` allocates a single Parser, and it's
used for the main file, every imported module, and every instantiation.
Each of those starts at a point where the Parser's state is empty apart
from the lexer and its Module: imports come first in a file (D17), and
instantiations are queued to the end of a top-level item (D21). So there's
no locals, `func_depth` and `type_params` are 0, and the lexer isn't inside
an interpolation. Switching to another module (or another place in the same
one) saves the Module pointer, the `FileBuf` and `LexPos`, points them at
the new source and offset, and restores them afterwards. That's less than
64 bytes, so an import nests by saving it on the C stack. Instantiations
found during an instantiation are queued rather than nested.

The switch asserts that the state really is empty (`num_locals`,
`func_depth`, `type_params` and `interp_sp` are 0). A new Parser field that
isn't empty at a top-level boundary fails that assert at the switch,
rather than needing a full snapshot of the Parser.

**Globals.** The module's table, keyed by NameId, is where globals live. It's
needed for `mod.name` anyway, which looks a name up in another module's
globals. An entry is a Global record. For now it's only a `GlobalKind`, and
it's where the type, the declaration offset and the codegen symbol will go.
`Name.global`, `Parser.globals` (the per-file list of NameIds) and
`pop_globals()` are removed.

Unqualified names are looked up in the table too. That's a hash probe for
every non-local name, in a table that can be large (dumbbench has 1.25M
top-level functions) and so often not in cache, where `Name.global` is a
load from a Name the lexer has just touched. If the probe is measurably
slower, a cache can be added afterwards: `Name.binding` points at the active
module's Global record. Locals already save and restore `binding`, so
shadowing works without changes. The costs are rebinding names when the
active module changes, and a tag to tell a Local from a Global. Adding the
cache doesn't change anything else, so it's fine to start without it. The
table's own performance (swisstable or other layouts) probably needs a
separate investigation once it exists.

**Generic instantiation across modules (D21).** A body is instantiated by
switching the Parser to the module that defines it, with the lexer at the
body's offset. Names in the body resolve against that module's globals, so
its private helpers work, and a name that both modules define binds to the
right one. Within one module it's the same switch without changing the
Module. Instantiations are queued to the end of the current top-level item,
because locals are bound through `Name.binding`, and in the middle of a
function the body would see that function's locals. If the `Name.binding`
cache for globals is added, switching modules also has to rebind them.

**Open: can a local reuse an import's name?** (2026-11-14) Inside a
function, a name that's bound by an import has to be followed by `.`, so
`someimp = 5` is an error ("Expecting '.' after module name.") rather than
declaring a local. It's nice that module names become semi-reserved, but
adding an import to old code could make an unrelated local invalid. To be
revisited: either keep it, or let an assignment declare a local that shadows
the import, as a local already shadows any other global.

**Steps.**

1. Split `Module` out of `Parser`, with `parse_file()` creating it, and
   `FileBuf` out of `LexState`. Move globals into the module's table, with
   all lookups through it, and remove `Name.global`. Bench against HEAD on
   the desktop.
   - **Measured 2026-11-14** (desktop, `parsecheck`, dumbbench): with the
     NameId multiplied by 2^64 / phi as the hash, 778 ms vs. 680 ms at HEAD
     (14% slower), with 2.3 more cache misses and 25 more page faults per
     function. Every global in dumbbench is a declaration, so it's the
     duplicate check and insert into a table with 1.25M entries spread out at
     random. Using the NameId itself as the hash puts consecutive
     declarations in consecutive slots: 702 ms (3% slower), and almost all
     of the difference is system time from growing the table. locals64 is
     1% slower (83.8 vs. 83.1 ms).
2. Probably: improve the table's performance (swisstable or other layouts),
   depending on what step 1 measures.
   - **Measured 2026-11-14** (desktop, ms). `bench/gen_globals.py` makes
     files that reference globals: gref_rand (300k functions, 4 lines each,
     references to uniformly random earlier globals), gref_near (the same,
     but 90% to the last 64 definitions) and gref_small (2000 globals, 800
     lines each, so the table stays in cache).

     | | dumbbench | gref_rand | gref_near | gref_small |
     |---|---|---|---|---|
     | HEAD (`Name.global`) | 695.7 | 388.3 | 286.5 | 216.8 |
     | NameId as hash, 64 slots | 709.3 | 407.0 | 296.7 | 221.8 |
     | NameId × 2^64 / phi | 787.4 | 441.8 | 321.3 | 228.4 |
     | NameId as hash, presized | 695.0 | 404.9 | 290.1 | 220.3 |
     | Verstable, default hash | 779.4 | 434.9 | 326.1 | 234.3 |
     | Verstable, NameId as bucket | 710.0 | 411.6 | 305.2 | 226.5 |

     The memory access pattern decides it, much more than the table's
     code. Any hash that spreads NameIds out costs 8–13% on the large
     files, and Verstable is 1–3% slower than the simple table even with
     NameId buckets. Its strengths (deletion without tombstones, high load
     factors, chains that stay short) don't matter for a table that's
     insert-only, at most half full, and keyed by dense integers.
     Presizing to the next power of 2 that's at least file length / 32
     slots avoids regrowing, which was mostly page faults, and makes
     dumbbench level with HEAD. What's left is about 1.5 ns per reference
     (gref_small), where `Name.global` was a load from a Name the lexer had
     just touched. That isn't worth the `Name.binding` cache for now.
     Decided: keep the simple table, presized.
3. The loader callback, the dict of modules by canonical name, and loading
   imports. Driven by `test/errors/todo/import_does_not_exist.luv`. The test
   runner skips files marked `# IMPORTED`.
4. Look up `mod.name` in the imported module's table. Driven by
   `import_wrong_func`, `import_wrong_type` and
   `import_treating_module_as_variable` in `test/errors/todo/`.
5. Report import cycles as an error.
