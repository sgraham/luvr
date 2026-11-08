@echo off
setlocal
mkdir out >nul 2>nul
::set CFLAGS=/nologo /Ox /GL -D_CRT_SECURE_NO_WARNINGS=0 -DBUILD_DEBUG=0 /Zi /W4 /WX
::cl %CFLAGS% src/lexdump.c src/lex.c /link /LTCG /out:out\lexdump.exe || exit /b
::cl %CFLAGS% bench/lexfloor.c src/lex.c /link /LTCG /out:out\lexfloor.exe || exit /b
::cl %CFLAGS% src/parsecheck.c src/parser.c src/lex.c /link /LTCG /out:out\parsecheck.exe || exit /b
set CFLAGS=/nologo /Ox -D_CRT_SECURE_NO_WARNINGS=0 -DBUILD_DEBUG=0 /Zi /W4 /WX
clang-cl %CFLAGS% src/lexdump.c src/lex.c /link /out:out\lexdump.exe || exit /b
clang-cl %CFLAGS% bench/lexfloor.c src/lex.c /link /out:out\lexfloor.exe || exit /b
clang-cl %CFLAGS% src/parsecheck.c src/parser.c src/lex.c /link /out:out\parsecheck.exe || exit /b
