#!/usr/bin/env bash
# Checks lionturtle against the engine's use of it: API edge cases,
# engine-shaped load with and without syncs, kill -9 recovery, and injected
# sync failures, each recovery checked row by row against a model. DIR must
# sit on the disk under test; it is removed.
# Usage: store/lionturtle/check/run.sh DIR
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
lt=$("$here/../fetch.sh")
dir=$1
bin=$(mktemp -d)
trap 'rm -rf "$bin" "$dir"' EXIT
native="-lutil -lrt -lpthread -lm -ldl"
c++ -std=c++20 -O2 -Wall -Wextra -Werror -I"$lt/lionturtle-c/include" "$here/usecase.cc" \
	"$lt/target/release/liblionturtle_c.a" $native -o "$bin/usecase"
cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared "$here/failsync.c" -ldl -o "$bin/failsync.so"
c++ -std=c++20 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden -shared -I"$lt/lionturtle-c/include" \
	"$here/plugin.cc" "$lt/target/release/liblionturtle_c.a" -Wl,--exclude-libs,ALL -Wl,-z,defs $native \
	-o "$bin/plugin.so"
c++ -std=c++20 -O2 -Wall -Wextra -Werror "$here/host.cc" -ldl -o "$bin/host"

exports=$(nm -D --defined-only "$bin/plugin.so" | awk '{print $3}' | grep -cv '^st_')
[ "$exports" = 0 ] || { echo "plugin.so exports $exports symbols beyond its own" >&2; exit 1; }
echo "plugin: links as a shared object exporting only its own symbols"

export LT_CACHE_MB=64 LT_CHI=8
mkdir -p "$dir"
"$bin/usecase" edges "$dir/edges"
"$bin/usecase" load "$dir/nosync" 20 0
"$bin/usecase" verify "$dir/nosync"
"$bin/usecase" load "$dir/sync" 15 32
"$bin/usecase" verify "$dir/sync"
"$bin/host" "$bin/plugin.so" crash "$dir/plugin" 10
"$bin/usecase" crash "$dir/crash" 10 16
for n in 100 3000 40000; do
	"$bin/usecase" fault "$dir/fault$n" 16 "$n" "$bin/failsync.so"
done
