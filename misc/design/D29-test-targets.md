# D29 Tests as ninja targets

Deferred 2026-11-16, done 2026-11-23.

`m <config> [--msvc] [args...]` runs `misc/gen_ninja.py` and then ninja,
building into `out/<platform><config>/` (e.g. `out/wd`, `out/lr`). `m` is a
shell script and `m.bat` a batch file, and neither parses its arguments:
`gen_ninja.py` handles the configuration and prints the arguments for ninja.
Test runs are ninja targets, so there's no test logic outside of them.

- The generator runs on every `m`, since ninja can't glob and new or renamed
  tests have to be picked up. It takes about 30 ms, and only writes
  `build.ninja` and `gen/prelude.h` when their contents change, so an
  unchanged run doesn't rebuild anything. `build.ninja` has no rule to
  regenerate itself, so running ninja directly doesn't pick up changes to the
  generator or the prelude.
- Every file in `test/` has an edge that runs `misc/check_test.py one` on it,
  and a phony target with the same name as the file, so `m d test/foo.luv`
  runs one test and the shell can complete the name. That's possible because
  no edge uses a test file as an input.
- Test edges depend on a phony `FORCE`, so they always run. Running the same
  tests repeatedly is useful for profiling and for finding flaky tests, and a
  test's result also depends on the modules it imports, which ninja doesn't
  track. Each edge writes a result file under `results/` when the test
  passes, with whether it ran with `luv il`.
- `test` runs every test edge and `unittest`, and then a summary edge that
  prints the pass counts from the result files. If any test fails, the
  summary doesn't run, and the failures are ninja's `FAILED:` blocks. The
  arguments include `-k 0` so that every failure is shown.
- One rule covers every kind of test. `check_test.py` runs `lexdump -r` on
  each file, `luv parse` and the `CRET`/`CERR` check on tests, and `luv il`
  on tests in `test/`. The interpreter runs in the same process, since
  starting a Python per test is most of the time a test takes. Files that
  are only imported by other tests only get the rewind check.
- Test output isn't on the console pool, so ninja buffers it and each
  failure is printed in one piece.
