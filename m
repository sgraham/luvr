#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys

TARGETS = [
    ("lexdump", ["src/lexdump.c", "src/lex.c"]),
    ("lexfloor", ["bench/lexfloor.c", "src/lex.c"]),
    ("parsecheck", ["src/parsecheck.c", "src/parser.c", "src/lex.c"]),
]


def commands(args):
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
        for name, srcs in TARGETS:
            srcs = [os.path.abspath(s) for s in srcs]
            yield [cc] + flags + srcs + ["/link"] + link + [f"/out:{name}.exe"], "out"
    else:
        flags = ["-g", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wimplicit-fallthrough"]
        if args.debug:
            flags += ["-O0", "-DBUILD_DEBUG=1"]
        else:
            flags += ["-O2", "-DBUILD_DEBUG=0"]
        for name, srcs in TARGETS:
            yield ["clang"] + flags + srcs + ["-o", f"out/{name}"], None


def main():
    ap = argparse.ArgumentParser(description="Build everything into out/.")
    ap.add_argument("-d", "--debug", action="store_true", help="debug build (default is release)")
    ap.add_argument("--msvc", action="store_true", help="on Windows, use cl instead of clang-cl")
    args = ap.parse_args()

    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs("out", exist_ok=True)
    for cmd, cwd in commands(args):
        rc = subprocess.call(cmd, cwd=cwd)
        if rc:
            sys.exit(rc)


if __name__ == "__main__":
    main()
