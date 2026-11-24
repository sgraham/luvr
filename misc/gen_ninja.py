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

# Sources that include the generated header with the prelude.
PRELUDE_USERS = {"src/parser_check.c", "src/parser_emit.c"}


# Writes a C header with the bytes of src as prelude_src, followed by
# LEX_PADDING zeros so that it can be lexed in place.
def write_embed(src, dst):
    data = open(src, "rb").read()
    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join(str(b) for b in data[i:i + 16]) + ",")
    text = f"// Generated from {src} by misc/gen_ninja.py.\n"
    text += f"#define PRELUDE_LEN {len(data)}\n"
    text += "static const u8 prelude_src[PRELUDE_LEN + LEX_PADDING] = {\n"
    text += "\n".join(lines) + "\n};\n"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", newline="\n") as f:
        f.write(text)


def quote(arg):
    return f'"{arg}"' if " " in arg else arg


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
        "-g", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wimplicit-fallthrough", f"-I{out}/gen"
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="output directory, relative to the repository root")
    ap.add_argument("--platform", choices=["win", "linux", "mac"])
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--msvc", action="store_true")
    ap.add_argument("--embed",
                    nargs=2,
                    metavar=("SRC", "DST"),
                    help="write the prelude header DST from SRC, instead of build.ninja")
    args = ap.parse_args()
    if args.embed:
        write_embed(*args.embed)
        return
    if not args.out or not args.platform:
        ap.error("--out and --platform are required")

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
    text += windows_rules(out, args.debug, args.msvc) if win else posix_rules(out, args.debug)
    text += f"""
rule regen
  command = {" ".join(quote(a) for a in regen)}
  generator = 1
  description = REGEN $out

build {out}/build.ninja: regen misc/gen_ninja.py

rule embed
  command = {quote(sys.executable)} misc/gen_ninja.py --embed $in $out
  description = EMBED $in

build {out}/gen/prelude.h: embed src/prelude.luv | misc/gen_ninja.py
"""

    srcs = sorted({s for ss in TARGETS.values() for s in ss})
    obj = {s: f"{out}/obj/{os.path.splitext(s)[0]}{obj_ext}" for s in srcs}
    text += "\n"
    for s in srcs:
        # The header is order-only, since the depfile has it once the object
        # has been built.
        dep = f" || {out}/gen/prelude.h" if s in PRELUDE_USERS else ""
        text += f"build {obj[s]}: cc {s}{dep}\n"
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
