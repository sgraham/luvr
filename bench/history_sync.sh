#!/bin/sh
# Copies the perf history log from the benchmark machine to bench/history.json,
# and writes out/history.html from it. With --now, first measures the commits
# on GitHub's main that the nightly run hasn't yet, and waits for them.
set -e
host=scott@192.168.1.146
dir=luvr/scratch/history
cd "$(dirname "$0")/.."
if [ "$1" = "--now" ]; then
    ssh "$host" "$dir/history_nightly.sh"
fi
scp -q "$host:$dir/history.json" bench/history.json
python3 bench/history_html.py
