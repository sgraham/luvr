#!/usr/bin/env python3

import argparse
import glob
import os
import re
import subprocess
import sys

TARGETS = {
    "lexdump": ["src/lexdump.c", "src/lex.c", "src/ctx.c"],
    "lexfloor": ["bench/lexfloor.c", "src/lex.c", "src/ctx.c"],
    "parsecheck": ["src/parsecheck.c", "src/parser.c", "src/path.c", "src/lex.c", "src/ctx.c"],
    "unittest": ["src/unittest.c", "src/path.c"],
}


def commands(args, name):
    srcs = TARGETS[name]
    if sys.platform == "win32":
        cc = "cl" if args.msvc else "clang-cl"
        flags = ["/nologo", "-D_CRT_SECURE_NO_WARNINGS=0", "/Zi", "/W4", "/WX"]
        link = []
        if args.debug:
            flags += ["/Od", "-DBUILD_DEBUG=1"]
        else:
            flags += ["/Ox", "-DBUILD_DEBUG=0"]
            if args.msvc:
                flags += ["/GL"]
                link += ["/LTCG"]
        # Run from out/ so that .obj and .pdb files are written there. Sources
        # are absolute so that diagnostics have paths that work from anywhere.
        srcs = [os.path.abspath(s) for s in srcs]
        return [cc] + flags + srcs + ["/link"] + link + [f"/out:{name}.exe"], "out"
    else:
        flags = ["-g", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wimplicit-fallthrough"]
        if args.debug:
            flags += ["-O0", "-DBUILD_DEBUG=1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        else:
            # Without the alignment, a size change in one file moves the
            # functions after it, and lex_next's speed varies by ~4% depending
            # on where it lands relative to 64 byte boundaries.
            flags += ["-O2", "-DBUILD_DEBUG=0", "-falign-functions=64"]
        return ["clang"] + flags + srcs + ["-o", f"out/{name}"], None


# The location is either {self}, or {dir}/file for an error in another file
# in the test's directory, such as a module that it imports.
def expected_error(path):
    text = open(path, encoding="utf-8").read()
    loc = re.search(r"^# CERR: (\{self\}|\{dir\}/[^:]+):(\d+):(\d+):", text, re.M)
    msg = re.search(r"^# CERR: \{ssss\} *\^ (error: .*)$", text, re.M)
    if not loc or not msg:
        return None
    where = path if loc.group(1) == "{self}" else os.path.dirname(path) + loc.group(1)[len("{dir}") :]
    return f"{where}:{loc.group(2)}:{loc.group(3)}: {msg.group(1)}"


def is_imported(path):
    with open(path, encoding="utf-8") as f:
        return f.readline().startswith("# IMPORTED")


# test/*.luv and test/errors/todo/*.luv must parse. The todo/ tests are semantic
# errors that the parser doesn't detect yet. test/errors/*.luv must fail with
# the error given in their CERR header.
# Files that start with '# IMPORTED' are only imported by other tests, so they
# aren't run on their own.
def run_tests():
    exe = os.path.join("out", "parsecheck.exe" if sys.platform == "win32" else "parsecheck")
    failures = []
    paths = sorted(glob.glob("test/*.luv") + glob.glob("test/errors/todo/*.luv"))
    paths = [p for p in paths if not is_imported(p)]
    for path in paths:
        r = subprocess.run([exe, "-q", path], capture_output=True, text=True)
        if r.returncode != 0:
            failures.append(f"{path}: expected to parse\n  got: {r.stderr.strip()}")
    errors = sorted(glob.glob("test/errors/*.luv"))
    errors = [p for p in errors if not is_imported(p)]
    for path in errors:
        want = expected_error(path)
        if want is None:
            failures.append(f"{path}: missing CERR header")
            continue
        r = subprocess.run([exe, "-q", path], capture_output=True, text=True)
        got = r.stderr.strip()
        if r.returncode == 0:
            failures.append(f"{path}: expected an error, but it parsed\n  want: {want}")
        elif got != want:
            failures.append(f"{path}: wrong error\n  want: {want}\n   got: {got}")
    # Rewinding the lexer and lexing again must reach the same state. All of the
    # test files lex without error, including those in todo/.
    lexdump = os.path.join("out", "lexdump.exe" if sys.platform == "win32" else "lexdump")
    rewind = sorted(glob.glob("test/**/*.luv", recursive=True))
    r = subprocess.run([lexdump, "-r"] + rewind, capture_output=True, text=True)
    if r.returncode != 0:
        failures.append(f"lexdump -r: rewind check failed\n  got: {r.stderr.strip()}")
    unittest = os.path.join("out", "unittest.exe" if sys.platform == "win32" else "unittest")
    r = subprocess.run([unittest], capture_output=True, text=True)
    if r.returncode != 0:
        failures.append(f"unittest failed\n  got: {(r.stdout + r.stderr).strip()}")
    for f in failures:
        print(f)
    total = len(paths) + len(errors)
    print(f"{total - len(failures)}/{total} tests passed")
    return not failures


def main():
    ap = argparse.ArgumentParser(description="Build parsecheck, or the given tool, into out/.")
    ap.add_argument("-d", "--debug", action="store_true", help="debug build (default is release)")
    ap.add_argument("--msvc", action="store_true", help="on Windows, use cl instead of clang-cl")
    ap.add_argument(
        "action",
        nargs="?",
        choices=["test", "lexdump", "lexfloor", "unittest"],
        help="test: build parsecheck and run the parser tests and unit tests. lexdump, lexfloor, unittest: build that tool instead of parsecheck",
    )
    args = ap.parse_args()

    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs("out", exist_ok=True)
    name = args.action if args.action in TARGETS else "parsecheck"
    cmd, cwd = commands(args, name)
    rc = subprocess.call(cmd, cwd=cwd)
    if rc:
        sys.exit(rc)
    if args.action == "test":
        for tool in ["lexdump", "unittest"]:
            cmd, cwd = commands(args, tool)
            rc = subprocess.call(cmd, cwd=cwd)
            if rc:
                sys.exit(rc)
    if args.action == "test" and not run_tests():
        sys.exit(1)


if __name__ == "__main__":
    main()
