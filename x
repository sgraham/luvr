#!/bin/sh
set -e
clear
./m r
echo -------------------------------------------------------------
if [ -n "$1" ]; then
  cp "$1" a.luv
fi
cat a.luv
echo -------------------------------------------------------------
out/mr/luv il a.luv | tee a.luvil
echo -------------------------------------------------------------
set +e
misc/interp.py a.luvil
echo $?
