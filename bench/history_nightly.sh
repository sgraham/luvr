#!/bin/sh
# Measures the commits on GitHub's main that aren't in the history log yet. Runs
# on the benchmark machine from cron, from a copy in ~/luvr/scratch/history:
#
#   30 23 * * * $HOME/luvr/scratch/history/history_nightly.sh >> $HOME/luvr/scratch/history/nightly.log 2>&1
#
# history.py is taken from the newest main each time, so only this script has
# to be copied again when it changes. bench/history_sync.sh copies the log back.
set -e
dir="$HOME/luvr/scratch/history"
inputs="$HOME/luvr/scratch/inputs"
cd "$dir"
# A run that's still going when the next one starts, finishes for both.
exec 9> "$dir/nightly.lock"
if ! flock -n 9; then
    echo "$(date -Is) already running"
    exit 0
fi
echo "$(date -Is) start"
if [ ! -d repo.git ]; then
    git init -q --bare repo.git
fi
git -C repo.git fetch -q https://github.com/sgraham/luvr.git +main:main
git -C repo.git show main:bench/history.py > history.py
python3 history.py --repo repo.git --log history.json \
    --input betterbench="$inputs/betterbench.luv" --input dumbbench="$inputs/dumbbench.luv"
echo "$(date -Is) done"
