#!/usr/bin/env python3

# Writes <out>/build.ninja for one platform and configuration. Ninja is run
# from the repository root with -f, so sources are named relative to the root
# and diagnostics have the same paths as when building by hand. Everything
# written by the build is under <out>, so configurations don't share any state.

import argparse
import os
import sys

TARGETS = {
    "lexdump": ["src/lexdump.c", "src/lex.c", "src/ctx.c"],
    "lexfloor": ["bench/lexfloor.c", "src/lex.c", "src/ctx.c"],
    "parsecheck": ["src/parsecheck.c", "src/parser.c", "src/path.c", "src/lex.c", "src/ctx.c"],
    "unittest": ["src/unittest.c", "src/path.c"],
}


def windows_rules(debug, msvc):
    cc = "cl" if msvc else "clang-cl"
    # /Z7 keeps debug info in each .obj rather than a shared compiler .pdb, so
    # that compiles can run in parallel.
    cflags = ["/nologo", "-D_CRT_SECURE_NO_WARNINGS=0", "/Z7", "/W4", "/WX"]
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


def posix_rules(debug):
    cflags = ["-g", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wimplicit-fallthrough"]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="output directory, relative to the repository root")
    ap.add_argument("--platform", required=True, choices=["win", "linux", "mac"])
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--msvc", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    out = args.out
    win = args.platform == "win"
    obj_ext = ".obj" if win else ".o"
    exe_ext = ".exe" if win else ""

    regen = [sys.executable, "misc/gen_ninja.py", "--out", out, "--platform", args.platform]
    if args.debug:
        regen.append("--debug")
    if args.msvc:
        regen.append("--msvc")

    text = f"ninja_required_version = 1.10\nbuilddir = {out}\n\n"
    text += windows_rules(args.debug, args.msvc) if win else posix_rules(args.debug)
    text += f"""
rule regen
  command = {" ".join(f'"{a}"' if " " in a else a for a in regen)}
  generator = 1
  description = REGEN $out

build {out}/build.ninja: regen misc/gen_ninja.py
"""

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
        exe = f"{out}/{name}{exe_ext}"
        exes.append(exe)
        text += f"build {name}: phony {exe}\n"
        text += f"build {exe}: link {' '.join(obj[s] for s in ss)}\n"
    text += f"\ndefault {' '.join(exes)}\n"

    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "build.ninja")
    with open(path, "w", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
