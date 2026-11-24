#!/usr/bin/env python3

# Writes <out>/build.ninja for this platform and one configuration, and
# <out>/gen/prelude.h. m runs this before every build, so build.ninja has no
# rule to regenerate itself. Ninja is run from the repository root with -f, so
# sources are named relative to the root and diagnostics have the same paths
# as when building by hand. Everything written by the build is under <out>, so
# configurations don't share any state.
#
# gen_ninja.py <d|r> [--msvc] [ninja arguments...]
#
# Prints the arguments for ninja: the build file, -k 0, and the rest of the
# arguments. m and m.bat pass them to ninja as they are, so that the shell
# scripts don't have to parse anything. Arguments with spaces aren't
# supported, since they're split by the shell again.

import glob
import os
import sys

TARGETS = {
    "lexdump": ["src/lexdump.c", "src/lex.c", "src/ctx.c"],
    "lexfloor": ["bench/lexfloor.c", "src/lex.c", "src/ctx.c"],
    # lex.c is first so that changes to the parser don't move lex_next. Its
    # speed depends on its address by up to ~4% on dumbbench, even with
    # functions aligned to 64 bytes.
    "luv": [
        "src/lex.c",
        "src/luv.c",
        "src/parser_check.c",
        "src/parser_emit.c",
        "src/il.c",
        "src/path.c",
        "src/ctx.c",
    ],
    "unittest": ["src/unittest.c", "src/il.c", "src/path.c", "src/lex.c", "src/ctx.c"],
}


# Writes text to path only if it's different, so that an unchanged file
# keeps its timestamp and doesn't cause a rebuild. The new contents are
# written to a temporary file and renamed over path, so that path is never
# partially written.
def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


# The bytes of src as prelude_src, followed by LEX_PADDING zeros so that it can
# be lexed in place.
def prelude_header(src):
    data = open(src, "rb").read()
    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join(str(b) for b in data[i:i + 16]) + ",")
    text = f"// Generated from {src} by misc/gen_ninja.py.\n"
    text += f"#define PRELUDE_LEN {len(data)}\n"
    text += "static const u8 prelude_src[PRELUDE_LEN + LEX_PADDING] = {\n"
    text += "\n".join(lines) + "\n};\n"
    return text


def quote(arg):
    return f'"{arg}"' if " " in arg else arg


# An edge per file in test/, with a phony named after the file's path, so that
# `m d test/foo.luv` runs one test. The phony has the same name as the source
# file, which works because nothing else depends on the source file. The edges
# depend on FORCE, so they run every time, which is useful for profiling and
# for flaky tests, and because a test's result also depends on the modules it
# imports. `test` runs all of them, then prints a summary.
def test_edges(out):
    py = quote(sys.executable)
    luv = f"{out}/luv{EXE_EXT}"
    lexdump = f"{out}/lexdump{EXE_EXT}"
    unittest = f"{out}/unittest{EXE_EXT}"
    paths = sorted(p.replace("\\", "/") for p in glob.glob("test/**/*.luv", recursive=True))
    text = f"""
rule check_test
  command = {py} misc/check_test.py one {out} $out $test
  description = TEST $test

rule test_summary
  command = {py} misc/check_test.py summary $in
  description = SUMMARY

rule run
  command = $cmd
  description = RUN $cmd

build FORCE: phony
"""
    results = []
    for p in paths:
        result = f"{out}/results/{p}"
        results.append(result)
        text += f"build {result}: check_test | {luv} {lexdump} FORCE\n  test = {p}\n"
        text += f"build {p}: phony {result}\n"
    # The unittest edge's output is never written, so it always runs.
    text += f"build {out}/results/unittest: run | {unittest}\n  cmd = {unittest}\n"
    text += f"build {out}/results/summary: test_summary {' '.join(results)} | FORCE\n"
    text += f"build test: phony {out}/results/summary {out}/results/unittest\n"
    return text


def windows_rules(out, debug, msvc):
    cc = "cl" if msvc else "clang-cl"
    # /Z7 keeps debug info in each .obj rather than a shared compiler .pdb, so
    # that compiles can run in parallel.
    cflags = ["/nologo", "-D_CRT_SECURE_NO_WARNINGS=0", "/Z7", "/W4", "/WX", f"/I{out}/gen"]
    ldflags = ["/nologo", "/DEBUG"]
    if debug:
        cflags += ["/Od", "-DBUILD_DEBUG=1"]
    else:
        cflags += ["/Ox", "-DBUILD_DEBUG=0"]
        if msvc:
            cflags += ["/GL"]
            ldflags += ["/LTCG"]
    return f"""\
cflags = {" ".join(cflags)}
ldflags = {" ".join(ldflags)}

rule cc
  command = {cc} $cflags /showIncludes /c $in /Fo$out
  deps = msvc
  description = CC $in

rule link
  command = link $ldflags $in /out:$out
  description = LINK $out
"""


def posix_rules(out, debug):
    cflags = [
        "-g", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wimplicit-fallthrough", f"-I{out}/gen"
    ]
    ldflags = ["-g"]
    if debug:
        san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        cflags += ["-O0", "-DBUILD_DEBUG=1"] + san
        ldflags += san
    else:
        # Without the alignment, a size change in one file moves the
        # functions after it, and lex_next's speed varies by ~4% depending
        # on where it lands relative to 64 byte boundaries.
        cflags += ["-O2", "-DBUILD_DEBUG=0", "-falign-functions=64"]
    return f"""\
cflags = {" ".join(cflags)}
ldflags = {" ".join(ldflags)}

rule cc
  command = clang $cflags -MMD -MF $out.d -c $in -o $out
  depfile = $out.d
  deps = gcc
  description = CC $in

rule link
  command = clang $ldflags $in -o $out
  description = LINK $out
"""


PLATFORM = {"win32": "win", "linux": "linux", "darwin": "mac"}[sys.platform]
EXE_EXT = ".exe" if PLATFORM == "win" else ""


# Parses the configuration from the start of args and writes its files. Each
# platform and configuration builds into its own directory, e.g. out/wd for
# Windows debug or out/lr for Linux release. Returns the directory and the
# rest of args, or None if args don't start with a configuration.
def generate(args):
    if not args or args[0] not in ("d", "r"):
        return None
    debug = args[0] == "d"
    rest = args[1:]
    msvc = "--msvc" in rest
    if msvc:
        rest.remove("--msvc")
    win = PLATFORM == "win"
    if msvc and not win:
        sys.exit("--msvc is only supported on Windows")
    out = f"out/{PLATFORM[0]}{args[0]}" + ("-msvc" if msvc else "")
    obj_ext = ".obj" if win else ".o"

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    text = f"ninja_required_version = 1.10\nbuilddir = {out}\n\n"
    text += windows_rules(out, debug, msvc) if win else posix_rules(out, debug)
    srcs = sorted({s for ss in TARGETS.values() for s in ss})
    obj = {s: f"{out}/obj/{os.path.splitext(s)[0]}{obj_ext}" for s in srcs}
    text += "\n"
    for s in srcs:
        text += f"build {obj[s]}: cc {s}\n"
    text += "\n"
    # Objects are linked in the order listed in TARGETS, since that decides
    # where functions land in the executable.
    exes = []
    for name, ss in TARGETS.items():
        exe = f"{out}/{name}{EXE_EXT}"
        exes.append(exe)
        text += f"build {name}: phony {exe}\n"
        text += f"build {exe}: link {' '.join(obj[s] for s in ss)}\n"
    text += f"\ndefault {' '.join(exes)}\n"
    text += test_edges(out)

    write_if_changed(os.path.join(out, "build.ninja"), text)
    write_if_changed(os.path.join(out, "gen", "prelude.h"), prelude_header("src/prelude.luv"))
    return out, rest


def main():
    generated = generate(sys.argv[1:])
    if not generated:
        sys.exit("usage: m <d|r> [--msvc] [ninja arguments...]")
    out, rest = generated
    # -k 0 keeps going after a failure, so that every failing test is shown.
    print(" ".join([f"-f {out}/build.ninja -k 0"] + rest))


if __name__ == "__main__":
    main()
