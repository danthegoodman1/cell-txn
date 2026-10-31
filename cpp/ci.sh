#!/usr/bin/env bash
# Builds and tests cpp/ in every preset. The debug, asan and tsan presets
# run every test binary; release checks the simulator against Go's traces
# for seeds 0–9,999 and runs the 30,000-seed sweeps; simbugs runs the
# self-test, which must find every deliberate bug.
# Usage: cpp/ci.sh [quick]   quick stops after the golden comparison.
set -euo pipefail
cd "$(dirname "$0")"
store/lionturtle/fetch.sh >/dev/null

for preset in debug asan tsan; do
	echo "== $preset"
	cmake --preset "$preset" >/dev/null
	cmake --build --preset "$preset"
	ctest --test-dir "build/$preset" --output-on-failure
done

echo "== golden"
sim/golden.sh 10000
[ "${1:-}" = quick ] && exit 0

echo "== sweeps"
cmake --build --preset release --target sim >/dev/null
for w in random users tpcc; do
	build/release/sim -seeds 30000 -workload "$w" | tail -2
done
build/release/sim -seeds 30000 -refresh -1 | tail -2

echo "== self-test"
cmake --preset simbugs >/dev/null
cmake --build --preset simbugs --target sim
for bug in $(grep -o '{"[a-z-]*"' core/bugs.cc | tr -d '{"'); do
	build/simbugs/sim -bug "$bug" -seeds 30000 -refresh -1 -det 0 | tail -1
done
