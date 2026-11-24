#!/bin/sh

# m <d|r> [ninja arguments...]
#
# Writes build.ninja with misc/gen_ninja.py, and runs ninja with the rest of
# the arguments. For example, `m d` builds everything, `m d test` runs all of
# the tests, and `m d test/add_int.luv` runs one.

cd "$(dirname "$0")" || exit 1
case "$(uname -s)" in
Darwin) ninja=third_party/ninja/mac/ninja ;;
*) ninja=third_party/ninja/linux/ninja ;;
esac
args=$(python3 misc/gen_ninja.py "$@") || exit 1
# args is split into words on purpose.
exec $ninja $args
