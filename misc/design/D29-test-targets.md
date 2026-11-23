# D29 Tests as ninja targets (deferred)

`m <config> [targets...]` builds with ninja into `out/<platform><config>/`
(e.g. `out/wd`, `out/lr`), from a `build.ninja` written by
`misc/gen_ninja.py`. Ninja only builds binaries. `m` handles `test` and test
names itself, and passes anything else on to ninja.

Instead, the generator could write the runs as ninja targets too, so that `m`
passes every argument through and `ninja -f out/mr/build.ninja test` works
without `m`:

- A `test` phony that depends on an edge per test, and a phony per test named
  after its file (e.g. `add_int`), so that a single test is just a target.
- One generic rule that runs the compiler on one input and compares against
  its `CRET`/`CERR` header. This is only worth doing once there's a single
  compiler to run on every input. Until then, parsecheck, lexdump and
  unittest each need a separate kind of edge.
- Ninja can't glob, so `m` reruns the generator at the start of every run to
  pick up added or renamed tests. Writing the file only when its contents
  change keeps this from triggering ninja's regeneration.
- Test edges have no outputs or stamp files, so they always run. Running the
  same tests repeatedly is useful for profiling and for finding flaky tests.
  A test's result also depends on the modules it imports, which a stamp
  wouldn't track.
- Test edges use `pool = console` so their output isn't buffered, at the
  cost of ninja's own status and `FAILED:` lines in between.
