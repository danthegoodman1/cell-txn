#!/usr/bin/env bash
# Runs identical SQL against InnoDB (SERIALIZABLE and REPEATABLE READ) and
# the celltnx engine in every mode, each configuration in a fresh mysqld
# from bench/build-mysql.sh with the same flags, and appends every run to a
# CSV. bench/median.sh summarizes the CSV.
# Usage: bench/compare-mysql.sh out.csv [duration] [runs] [durable: 0|1]
# Durable runs sync every commit: InnoDB with innodb_flush_log_at_trx_commit
# at 1, celltnx with celltnx_sync. Otherwise neither syncs per commit, though
# both still write: InnoDB its redo log, celltnx its lionturtle store.
# ONLY=innodb or ONLY=celltnx runs one side; MODES picks celltnx modes
# (default "row cell cell+delta"). WORKLOADS picks rows of the matrix by
# name (default all; see matrix below). TPC-C and ledger runs end with
# sqlbench's -audit, so an inconsistent result stops the script. PREPARE=1 sends statements as
# server-side prepared statements and adds -prep to each label. EXTRA adds
# mysqld flags to every server, and LABEL a suffix to celltnx labels, e.g.
# EXTRA=--celltnx-lock-wait-ms=0 LABEL=-nowait. CLIENTS sets the number of
# clients (default 32).
# MYSQL_ROOT (default ~/.cache/celltnx) holds the build; the data directory
# goes under it and is removed on exit.
set -euo pipefail
out=$(realpath -m "${1:-compare-mysql.csv}")
dur=${2:-8s}
reps=${3:-3}
durable=${4:-0}
cd "$(dirname "$0")/.."
root=${MYSQL_ROOT:-$HOME/.cache/celltnx}
build=$root/build-release
work=$(mktemp -d "$root/bench.XXXXXX")
sock=$work/mysql.sock
port=3311
pid=
stop() {
	"$build/bin/mysqladmin" --no-defaults -uroot --socket="$sock" shutdown >/dev/null 2>&1 || kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	pid=
}
cleanup() {
	if [ -n "$pid" ]; then stop; fi
	rm -rf "$work"
}
trap cleanup EXIT
go build -o "$work/sqlbench" ./cmd/sqlbench
"$build/bin/mysqld" --no-defaults --initialize-insecure --datadir="$work/pristine" >"$work/init.log" 2>&1

start() { # extra mysqld flags...
	rm -rf "$work/data"
	cp -a "$work/pristine" "$work/data"
	# shellcheck disable=SC2086 # EXTRA holds several flags
	"$build/bin/mysqld" --no-defaults --datadir="$work/data" --port=$port --socket="$sock" \
		--bind-address=127.0.0.1 --mysqlx=OFF --disable-log-bin --max-connections=1000 \
		--innodb-buffer-pool-size=4G --innodb-lock-wait-timeout=10 --innodb-flush-log-at-trx-commit="$durable" \
		--plugin-dir="$build/plugin_output_directory" --plugin-load-add=ha_celltnx.so --celltnx-sync="$durable" \
		${EXTRA:-} "$@" >"$work/mysqld.log" 2>&1 &
	pid=$!
	for _ in $(seq 150); do
		if "$build/bin/mysqladmin" --no-defaults -uroot --socket="$sock" ping >/dev/null 2>&1; then return; fi
		sleep 0.2
	done
	cat "$work/mysqld.log" >&2
	exit 1
}

prep=()
suffix=
if [ "${PREPARE:-0}" = 1 ]; then
	prep=(-prepare)
	suffix=-prep
fi
dsn="root@tcp(127.0.0.1:$port)/"
want() { [ -z "${WORKLOADS:-}" ] || [[ " $WORKLOADS " == *" $1 "* ]]; }

matrix() { # label sqlbench-args...
	local label=$1
	shift
	b() {
		for _ in $(seq "$reps"); do
			"$work/sqlbench" -dsn "$dsn" -label "$label$suffix" -clients "${CLIENTS:-32}" -duration "$dur" -setup -csv "$out" "${prep[@]}" "$@"
		done
	}
	if want tpcc1; then b -workload tpcc -warehouses 1 -customers 100 -items 5000 -audit "$@"; fi
	if want tpcc4; then b -workload tpcc -warehouses 4 -customers 100 -items 5000 -audit "$@"; fi
	if want tpcc1-think; then b -workload tpcc -warehouses 1 -customers 100 -items 5000 -think 1ms -audit "$@"; fi
	for theta in 0.5 0.9 0.99; do
		if want "users$theta"; then b -workload users -rows 1000 -theta "$theta" -disjoint 0.5 "$@"; fi
	done
	for dj in 0 0.9; do
		if want "users0.99-$dj"; then b -workload users -rows 1000 -theta 0.99 -disjoint "$dj" "$@"; fi
	done
	# Transfers through hot ledger accounts, audited after each run.
	for hot in 1 10 0; do
		if want "ledger-hot$hot"; then b -workload ledger -rows 1000 -hot "$hot" -audit "$@"; fi
	done
	if want ledger-hot1-think; then b -workload ledger -rows 1000 -hot 1 -think 1ms -audit "$@"; fi
}

only=${ONLY:-all}
if [ "$only" != celltnx ]; then
	start
	matrix innodb-serializable -engine InnoDB -isolation SERIALIZABLE
	matrix innodb-repeatable-read -engine InnoDB -isolation REPEATABLE-READ
	stop
fi
if [ "$only" != innodb ]; then
	for mode in ${MODES:-row cell cell+delta}; do
		start --celltnx-mode="$mode"
		matrix "celltnx-$mode${LABEL:-}" -engine CELLTNX
		stop
	done
fi
