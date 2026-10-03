#!/usr/bin/env bash
# Sweeps contention for both workloads in every mode and appends to a CSV.
# Usage: bench/sweep.sh [out.csv] [duration]
set -euo pipefail
out=${1:-bench.csv}
dur=${2:-5s}
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
bin=$tmp/bench
go build -o "$bin" ./cmd/bench

for wh in 1 2 4 8; do
	"$bin" -workload tpcc -warehouses "$wh" -duration "$dur" -csv "$out"
done
for theta in 0 0.5 0.9 0.99; do
	"$bin" -workload users -theta "$theta" -duration "$dur" -csv "$out"
done
# Disjoint-column share: profile and pay touch different columns of the
# same hot rows; same-column transactions conflict in every mode.
for mix in 2,2,4,0,0,0 2,2,0,0,4,0 0,0,4,0,4,0; do
	"$bin" -workload users -theta 0.9 -mix "$mix" -duration "$dur" -csv "$out"
done
