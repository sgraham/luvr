#!/usr/bin/env python3

import argparse
import glob
import os
import re
import subprocess
import sys

PLATFORM = {"win32": "win", "linux": "linux", "darwin": "mac"}[sys.platform]
EXE = ".exe" if PLATFORM == "win" else ""


# Each platform and configuration builds into its own directory, e.g. out/wd
# for Windows debug or out/lr for Linux release.
def out_dir(args):
    d = f"out/{PLATFORM[0]}{args.config}"
    if args.msvc:
        d += "-msvc"
    return d


# build.ninja has a rule to regenerate itself when the generator changes, so
# it only has to be written here the first time.
def build(args, out, targets):
    if not os.path.exists(os.path.join(out, "build.ninja")):
        gen = [sys.executable, "misc/gen_ninja.py", "--out", out, "--platform", PLATFORM]
        if args.config == "d":
            gen.append("--debug")
        if args.msvc:
            gen.append("--msvc")
        subprocess.check_call(gen)
    ninja = os.path.join("third_party", "ninja", PLATFORM, "ninja" + EXE)
    rc = subprocess.call([ninja, "-f", os.path.join(out, "build.ninja")] + targets)
    if rc:
        sys.exit(rc)


# The expected stderr is the CERR lines, with {self} replaced by the test's
# path, and {ssss} by spaces of the same length, so that the ^ lines up with
# the source line above it both in the header and in the output. A test can
# be renamed without changing its header. Trailing whitespace is ignored,
# since editors tend to strip it from the headers.
def expected_result(path):
    text = open(path, encoding="utf-8").read()
    ret = re.search(r"^# CRET: (\d+)$", text, re.M)
    lines = re.findall(r"^# CERR: (.*)$", text, re.M)
    if not ret or not lines:
        return None
    lines = [l.replace("{self}", path).replace("{ssss}", " " * len(path)).rstrip() for l in lines]
    return int(ret.group(1)), "\n".join(lines)


# Prefixes the first line with `label`, and the rest with spaces to match.
def indent(text, label):
    pad = "\n" + " " * len(label)
    return label + pad.join(text.split("\n"))


def is_imported(path):
    with open(path, encoding="utf-8") as f:
        return f.readline().startswith("# IMPORTED")


def is_error_test(path):
    return path.startswith("test/errors/") and not path.startswith("test/errors/todo/")


def all_tests():
    paths = glob.glob("test/*.luv") + glob.glob("test/errors/*.luv") + glob.glob("test/errors/todo/*.luv")
    return sorted(p.replace("\\", "/") for p in paths if not is_imported(p))


# A test can be named by its path, or by its file name without .luv when that
# is unique. Returns None if it doesn't name a test, so that it can be passed
# on to ninja.
def find_test(name, ap):
    if name.endswith(".luv"):
        path = os.path.normpath(name).replace("\\", "/")
        if not os.path.exists(path):
            ap.error(f"{name}: no such test")
        matches = [path]
    else:
        matches = sorted(p.replace("\\", "/") for p in glob.glob(f"test/**/{name}.luv", recursive=True))
    if not matches:
        return None
    if len(matches) > 1:
        ap.error(f"{name}: ambiguous, matches {', '.join(matches)}")
    if is_imported(matches[0]):
        ap.error(f"{matches[0]}: only imported by other tests")
    return matches[0]


# test/*.luv and test/errors/todo/*.luv must parse. The todo/ tests are semantic
# errors that the parser doesn't detect yet. test/errors/*.luv must fail with
# the error given in their CERR header.
# Files that start with '# IMPORTED' are only imported by other tests, so they
# aren't run on their own.
def check_test(exe, path):
    r = subprocess.run([exe, "parse", "-q", path], capture_output=True, text=True)
    if not is_error_test(path):
        if r.returncode != 0:
            return f"{path}: expected to parse\n  got: {r.stderr.strip()}"
        return None
    expected = expected_result(path)
    if expected is None:
        return f"{path}: missing CRET or CERR header"
    want_ret, want = expected
    got = "\n".join(l.rstrip() for l in r.stderr.strip("\n").split("\n"))
    if r.returncode != want_ret:
        return f"{path}: expected exit status {want_ret}, got {r.returncode}\n{indent(want, '  want: ')}"
    if got != want:
        return f"{path}: wrong error\n{indent(want, '  want: ')}\n{indent(got, '   got: ')}"
    return None


def run_tests(out, paths, rewind, unittest):
    exe = os.path.join(out, "luv" + EXE)
    failures = [f for f in (check_test(exe, p) for p in paths) if f]
    # Rewinding the lexer and lexing again must reach the same state. All of the
    # test files lex without error, including those in todo/.
    lexdump = os.path.join(out, "lexdump" + EXE)
    r = subprocess.run([lexdump, "-r"] + rewind, capture_output=True, text=True)
    if r.returncode != 0:
        failures.append(f"lexdump -r: rewind check failed\n  got: {r.stderr.strip()}")
    if unittest:
        r = subprocess.run([os.path.join(out, "unittest" + EXE)], capture_output=True, text=True)
        if r.returncode != 0:
            failures.append(f"unittest failed\n  got: {(r.stdout + r.stderr).strip()}")
    for f in failures:
        print(f)
    print(f"{len(paths) - len(failures)}/{len(paths)} tests passed")
    return not failures


def main():
    ap = argparse.ArgumentParser(description="Build into out/<platform><config>/ and run tests.")
    ap.add_argument("config", choices=["d", "r"], help="d: debug build, r: release build")
    ap.add_argument("--msvc", action="store_true", help="on Windows, use cl instead of clang-cl")
    ap.add_argument(
        "targets",
        nargs="*",
        help="test: run all tests. A test path or name (e.g. add_int): run that test. "
        "Anything else is a ninja target, e.g. luv. With no targets, all binaries are built.",
    )
    args = ap.parse_args()

    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    if args.msvc and PLATFORM != "win":
        ap.error("--msvc is only supported on Windows")
    out = out_dir(args)
    ninja_targets = []
    tests = []
    all = False
    for t in args.targets:
        if t == "test":
            all = True
            continue
        path = find_test(t, ap)
        if path:
            tests.append(path)
        else:
            ninja_targets.append(t)
    if all:
        tests = all_tests()
        ninja_targets += ["unittest"]
    if tests:
        ninja_targets += ["luv", "lexdump"]
    build(args, out, ninja_targets)
    if tests:
        rewind = sorted(glob.glob("test/**/*.luv", recursive=True)) if all else tests
        if not run_tests(out, tests, rewind, all):
            sys.exit(1)


if __name__ == "__main__":
    main()
