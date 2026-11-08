#/bin/sh
set -e
mkdir -p out
CFLAGS='-O2 -g -DBUILD_DEBUG=0 -std=c11 -Wall -Wextra -Wpedantic -Wimplicit-fallthrough'
clang $CFLAGS src/lexdump.c src/lex.c -o out/lexdump
clang $CFLAGS bench/lexfloor.c src/lex.c -o out/lexfloor
clang $CFLAGS src/parsecheck.c src/parser.c src/lex.c -o out/parsecheck
