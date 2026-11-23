# D11 ★ Forward references (functions and types used before they are defined)

A typed compiler needs the callee's return type to type-check
`x = helper()`, and a struct's layout to use it by value. Lua 4 doesn't care
because globals are dynamic.

**What the luv60 tests actually need (scanned all of `test/`, `test/errors/`,
`stdlib/`):**
- **Zero** uses before definition. No top-level `def`/`on`/`struct`/
  `foreign` appears after `main` in any file. `memfn_self_references.luv`
  even defines `memfn_2` *before* `memfn_1`, which calls it. The suite is
  written define-before-use.
- **Zero** recursive or mutually recursive functions. (Two hits from the scan
  aren't recursion: `manual_list_int.luv` calls `int`'s `__repr__` from
  `ListData_int.__repr__`, and `memfn_shadow_foreign_name.luv` calls the
  foreign `free` from `Stuff.free`.)
- Imports aren't affected, because an imported module is compiled whole
  before its importer.

So requiring definition before use breaks no existing test. It's a question
of what the language should allow, not of compatibility.

**Recursion under define-before-use:**
- *Self*-recursion is free in a single pass. The return type comes before
  the name (`def int fact(int n):`), so the full signature is known at `)`
  and the symbol can be bound before the body is parsed.
- *Mutual* recursion and mutually referencing structs need a forward
  declaration. No new keyword is required: a header that ends at NEWLINE
  instead of `:` is a declaration (`def bool is_odd(int n)`, `struct Node`).
  After `)` (or after the struct name), `:` vs. NEWLINE is one token, so it's
  LL(1). luv already has the same shape in `foreign int f()`. Pointers to a
  declared-only struct work; using it by value needs the full definition.

Options:

- **(a) Define before use**, plus the forward-declaration headers above.
  Zero cost. C/Pascal-like discipline, and somewhat un-Pythonic for files
  that put `main` first.
- **(b) Eager top-level skim pass.** Before the real parse, lex only
  indent-0 lines (`def [type] name(params)`, `on T def ...`, `struct` with
  its field lines, `foreign`, `import`, top-level variables) and skip bodies
  by fast-forwarding while lines start with a space (a byte compare after
  each `\n`). Every file pays for it. On dumbbench that's re-lexing 1 in 7
  non-comment lines, plus a line-start scan of the whole 207 MB (which
  exceeds cache, so it's a second trip to DRAM). Rough estimate: 5–15% of
  parse time.
- **(c) Lazy skim on first miss.** Parse in order as in (a). The *first*
  time a global name or TYPENAME fails to resolve, run the (b) skim once
  over the rest of the file to build a `name → offset` index of top-level
  headers, then parse the needed signature (and, for a struct, its fields)
  on demand. Files written define-before-use, which is the whole test suite
  and dumbbench, pay **nothing**. Files that use forward references pay one
  skim. A truly undefined name pays the skim and then errors. Recursion and
  mutual recursion work without forward declarations, though the
  header-only declaration can still be allowed.
- **(d) Late patching**: emit unresolved calls and patch them afterward.
  Can't type-check `helper().field` before `helper` is seen. Rejected.

**Limits on the skim (so lazy lookup can't diverge from or hide errors in
the normal path):**
- *The skim never reports errors.* An indent-0 header that doesn't parse is
  recorded as "malformed at offset X" and skipped. Before the main parse
  reports "undefined name", it checks for a recorded malformed header. If one
  exists, it parses that header for real and reports *its* error instead,
  which is the root cause and matches what the in-order file would report.
  Duplicate definitions are indexed silently. The main parse reports them
  when it reaches the second one, as usual.
- *One header parser.* The skim calls the same `funcsig`/`struct`-field code
  as the main parse, at a different offset, so a symbol is identical no
  matter which path created it. When the main parse reaches a header the
  skim already turned into a symbol, it reuses that symbol and parses only
  the body. It never defines it twice.
- *Declarations only, never values.* The skim finds only items whose type is
  fully written in the header: `def`, `on T def` (keyed `(T, name)`),
  `struct`, `foreign`, typed globals (`int x = 55`: address and type only).
  Untyped globals and constants (`x = 1`, `BLAH = 3`) get their type or
  value from evaluating an expression, so using them before their
  definition stays an error.

**Recommendation: (c).** It has the speed of (a) for code that's already in
order and the convenience of (b) for code that isn't. It keeps the main
parse single-pass. The cost is a resumable "parse just this header at
offset X" entry point, which (b) needs anyway. A struct's field types may
trigger further on-demand lookups, so the parser needs a cycle check for
by-value self-containment. If (c)'s bookkeeping turns out to be fiddly, (a)
is the fallback: no tests need changing. The incremental "top-level
chunker" from `misc/notes.txt` (sha per top-level item) is the same
header index as (b)/(c), so this work could feed it later.
