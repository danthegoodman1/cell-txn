#!/usr/bin/env bash
# Compares the C++ simulator with the Go one seed by seed: both write each
# seed's trace, steps, crashes, failure and parameters, and the files must
# be identical. Prints the first differing lines otherwise.
# Usage: cpp/sim/golden.sh [seeds] [start]
set -euo pipefail
seeds=${1:-10000}
start=${2:-0}
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
go run ./cmd/sim -seeds "$seeds" -start "$start" -det 0 -golden "$out/go.txt" >/dev/null
(cd cpp && cmake --preset release >/dev/null && cmake --build --preset release --target sim >/dev/null)
cpp/build/release/sim -seeds "$seeds" -start "$start" -det 0 -golden "$out/cpp.txt" >/dev/null || true
if cmp -s "$out/go.txt" "$out/cpp.txt"; then
	echo "golden: $seeds seeds from $start match"
	exit 0
fi
echo "golden: mismatch; first differences (< go, > cpp):"
diff "$out/go.txt" "$out/cpp.txt" | head -20
exit 1
