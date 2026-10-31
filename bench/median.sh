#!/usr/bin/env bash
# Summarizes a sqlbench CSV: one line per system and configuration, with
# the median commits/s over its runs, the median run's retries per
# transaction and p99 latency, and the spread of commits/s as
# (max - min) / median.
# Usage: bench/median.sh results.csv
set -euo pipefail
# A ledger run's hot-account count is the 13th field, absent from older files.
tail -n +2 "$1" | awk -F, '{ print $0 (NF < 13 ? "," : "") }' | sort -t, -k1,7 -k13,13 -k8,8n | awk -F, '
function flush() {
	if (n == 0) return
	m = n % 2 ? r[(n + 1) / 2] : (r[n / 2] + r[n / 2 + 1]) / 2
	shape = wl == "tpcc" ? "wh=" wh : wl == "users" ? "theta=" th " disjoint=" dj : "hot=" hot
	printf "%-32s %-6s %-26s think=%s clients=%-3s %8.0f commits/s  retries %.2f  p99 %6.2f ms  spread %.0f%%  (n=%d)\n",
		sys, wl, shape, tk, cl, m, t[int((n + 1) / 2)], p99[int((n + 1) / 2)], (m > 0 ? 100 * (r[n] - r[1]) / m : 0), n
	n = 0
}
{
	key = $1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7 FS $13
	if (key != last) { flush(); last = key; sys = $1; wl = $2; wh = $3; th = $4; dj = $5; cl = $6; tk = $7; hot = $13 }
	n++; r[n] = $8; t[n] = $9; p99[n] = $11
}
END { flush() }'
