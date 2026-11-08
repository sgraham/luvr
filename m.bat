@echo off
setlocal
mkdir out >nul 2>nul
set CFLAGS=/Os -DBUILD_DEBUG=0 /Zi /W4 /WX
cl %CFLAGS% src/lexdump.c src/lex.c -o out/lexdump
cl %CFLAGS% bench/lexfloor.c src/lex.c -o out/lexfloor
cl %CFLAGS% src/parsecheck.c src/parser.c src/lex.c -o out/parsecheck
