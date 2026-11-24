@echo off
rem m <d|r> [--msvc] [ninja arguments...]
rem
rem See m. gen_ninja.py prints the arguments for ninja, or nothing if the
rem arguments are wrong.
setlocal
cd /d "%~dp0"
set ninja_args=
for /f "delims=" %%a in ('python misc\gen_ninja.py %*') do set ninja_args=%%a
if not defined ninja_args exit /b 1
third_party\ninja\win\ninja.exe %ninja_args%
