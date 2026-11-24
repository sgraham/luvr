#!/usr/bin/env python3

# Runs one test for ninja, or prints the summary of a run. build.ninja has an
# edge per file in test/ that runs `check_test.py one`, and the summary edge
# runs `check_test.py summary` once every test has passed.
#
# A test prints nothing if it passes, and the reason and exits with 1 if it
# fails. It writes its result file only when it passes, with whether it ran
# with luv il, so that the summary can count them.

import io
import os
import re
import subprocess
import sys
import traceback

import interp


# Prefixes the first line with `label`, and the rest with spaces to match.
def indent(text, label):
    pad = "\n" + " " * len(label)
    return label + pad.join(text.split("\n"))


# The expected stderr is the CERR lines, with {self} replaced by the test's
# path, and {ssss} by spaces of the same length, so that the ^ lines up with
# the source line above it both in the header and in the output. A test can
# be renamed without changing its header. Trailing whitespace is ignored,
# since editors tend to strip it from the headers.
def expected_result(path, text):
    ret = re.search(r"^# CRET: (\d+)$", text, re.M)
    lines = re.findall(r"^# CERR: (.*)$", text, re.M)
    if not ret or not lines:
        return None
    lines = [l.replace("{self}", path).replace("{ssss}", " " * len(path)).rstrip() for l in lines]
    return int(ret.group(1)), "\n".join(lines)


def is_error_test(path):
    return path.startswith("test/errors/") and not path.startswith("test/errors/todo/")


# test/*.luv and test/errors/todo/*.luv must parse. The todo/ tests are semantic
# errors that the parser doesn't detect yet. test/errors/*.luv must fail with
# the error given in their CERR header.
def check_parse(luv, path, text):
    r = subprocess.run([luv, "parse", "-q", path], capture_output=True, text=True)
    if not is_error_test(path):
        if r.returncode != 0:
            return f"{path}: expected to parse\n  got: {r.stderr.strip()}"
        return None
    expected = expected_result(path, text)
    if expected is None:
        return f"{path}: missing CRET or CERR header"
    want_ret, want = expected
    got = "\n".join(l.rstrip() for l in r.stderr.strip("\n").split("\n"))
    if r.returncode != want_ret:
        return f"{path}: expected exit status {want_ret}, got {r.returncode}\n{indent(want, '  want: ')}"
    if got != want:
        return f"{path}: wrong error\n{indent(want, '  want: ')}\n{indent(got, '   got: ')}"
    return None


# Runs the dump with misc/interp.py in this process, since starting another
# Python for each test would be most of the time a test takes. Returns main's
# exit status, the output, and an error message, or None if there wasn't one.
def interpret(dump):
    buf = io.BytesIO()
    out = io.TextIOWrapper(buf, encoding="utf-8", newline="")
    saved = sys.stdout
    sys.stdout = out
    try:
        prog = interp.parse(dump)
        if "main" not in prog.funcs:
            raise interp.Error("no main")
        ret = interp.run(prog, prog.funcs["main"], [], None)
        error = None
    except interp.Error as e:
        ret, error = 1, f"interp: {e}"
    except Exception:
        ret, error = 1, traceback.format_exc()
    finally:
        sys.stdout = saved
    out.flush()
    return (ret or 0) & 0xFF, buf.getvalue().decode("utf-8", "replace"), error


# Tests in test/ whose functions all emit are also run by interpreting the
# output of `luv il`. main's return value and the output are checked against
# the RET and OUT headers, which default to 0 and nothing. Returns whether the
# test emitted, and the failure if there was one.
def check_il(luv, path, text):
    r = subprocess.run([luv, "il", path], capture_output=True, text=True)
    if r.returncode != 0:
        if re.search(r"(isn't|aren't) supported yet\.$", r.stderr.rstrip()):
            return False, None
        return True, f"{path}: luv il failed\n{indent(r.stderr.strip(), '  got: ')}"
    got_ret, output, error = interpret(r.stdout)
    ret = re.search(r"^# RET: (\d+)$", text, re.M)
    want_ret = int(ret.group(1)) if ret else 0
    want = "\n".join(l.rstrip() for l in re.findall(r"^# OUT: ?(.*)$", text, re.M))
    got = "\n".join(l.rstrip() for l in output.splitlines())
    if error:
        return True, f"{path}: interp failed\n{indent(error.strip(), '  got: ')}"
    if got_ret != want_ret:
        return True, f"{path}: interp returned {got_ret}, expected {want_ret}"
    if got != want:
        return True, f"{path}: wrong output\n{indent(want, '  want: ')}\n{indent(got, '   got: ')}"
    return True, None


# Rewinding the lexer and lexing again must reach the same state. Every test
# file lexes without error, including those in todo/ and those that are only
# imported, except for the lexer error tests in test/errors/lex_*.luv.
def check_rewind(lexdump, path):
    if path.startswith("test/errors/lex_"):
        return None
    r = subprocess.run([lexdump, "-r", path], capture_output=True, text=True)
    if r.returncode != 0:
        return f"{path}: lexdump -r: rewind check failed\n  got: {r.stderr.strip()}"
    return None


# Files that start with '# IMPORTED' are only imported by other tests, so only
# the rewind check is run on them, and their result is "imported". Otherwise
# it's "il ok" or "il todo" for the tests that are run with luv il, and empty
# for the rest.
def run_one(out, result, path):
    # The result is only ever written under the build directory, so that a
    # mistake in the arguments can't overwrite a test.
    if not os.path.abspath(result).startswith(os.path.abspath(out) + os.sep):
        sys.exit(f"check_test.py: result {result} isn't under {out}")
    exe = ".exe" if sys.platform == "win32" else ""
    luv = os.path.join(out, "luv" + exe)
    text = open(path, encoding="utf-8").read()
    failure = check_rewind(os.path.join(out, "lexdump" + exe), path)
    status = "imported" if text.startswith("# IMPORTED") else ""
    if not failure and not status:
        failure = check_parse(luv, path, text)
        if not failure and os.path.dirname(path) == "test":
            emitted, failure = check_il(luv, path, text)
            status = "il ok" if emitted else "il todo"
    if failure:
        print(failure)
        return False
    os.makedirs(os.path.dirname(result), exist_ok=True)
    with open(result, "w", encoding="utf-8") as f:
        f.write(status)
    return True


def summary(results):
    statuses = [open(r, encoding="utf-8").read() for r in results]
    tests = [s for s in statuses if s != "imported"]
    print(f"{len(tests)}/{len(tests)} tests passed")
    il = [s for s in tests if s]
    print(f"{il.count('il ok')}/{len(il)} tests run with luv il")


# check_test.py one <out> <result> <path>
# check_test.py summary <result>...
def main():
    if len(sys.argv) == 5 and sys.argv[1] == "one":
        sys.exit(0 if run_one(*sys.argv[2:]) else 1)
    if len(sys.argv) > 2 and sys.argv[1] == "summary":
        summary(sys.argv[2:])
        return
    sys.exit("usage: check_test.py one <out> <result> <path>\n"
             "       check_test.py summary <result>...")


if __name__ == "__main__":
    main()
