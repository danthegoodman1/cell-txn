#!/usr/bin/env bash
# Compares this tree's simulator with a git revision's, seed by seed: both
# write each seed's trace, steps, crashes, failure and parameters, and the
# files must be identical. A change that must keep every trace, such as
# performance work, passes this against the revision before it. Prints the
# first differing lines otherwise. The revision builds in a temporary
# worktree, removed on exit.
# Usage: sim/golden.sh [rev] [seeds] [start]   rev defaults to HEAD.
set -euo pipefail
rev=${1:-HEAD}
seeds=${2:-10000}
start=${3:-0}
cd "$(dirname "$0")/.."
root=$(pwd)
out=$(mktemp -d)
cleanup() {
	git -C "$root" worktree remove --force "$out/ref" 2>/dev/null || true
	rm -rf "$out"
}
trap cleanup EXIT
git worktree add -q --detach "$out/ref" "$rev"
build() { # dir
	(cd "$1" && cmake --preset release >/dev/null && cmake --build --preset release --target sim >/dev/null)
}
build "$out/ref"
build "$root"
"$out/ref/build/release/sim" -seeds "$seeds" -start "$start" -det 0 -golden "$out/ref.txt" >/dev/null || true
"$root/build/release/sim" -seeds "$seeds" -start "$start" -det 0 -golden "$out/tree.txt" >/dev/null || true
if cmp -s "$out/ref.txt" "$out/tree.txt"; then
	echo "golden: $seeds seeds from $start match $rev"
	exit 0
fi
echo "golden: mismatch; first differences (< $rev, > this tree):"
diff "$out/ref.txt" "$out/tree.txt" | head -20
exit 1
