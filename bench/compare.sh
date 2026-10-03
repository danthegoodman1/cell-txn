#!/usr/bin/env bash
# Runs identical SQL against MySQL (SERIALIZABLE and REPEATABLE READ) and
# cell-tnx in every mode, appending to a CSV.
# Usage: bench/compare.sh out.csv [duration] [durable: 0|1]
# ONLY=mysql or ONLY=ours runs one side. PREPARE=1 sends statements as
# server-side prepared statements and adds -prep to each label. Durable
# runs keep cell-tnx's data under $DATA_DIR (default .benchdata), which
# must share a disk with MySQL's data: on tmpfs an fsync costs nothing.
# Expects MySQL 8 at $MYSQL_DSN (default root@tcp(127.0.0.1:3308)/), on the
# host network so both servers sit behind the same loopback interface:
#   docker run -d --name celltnx-mysql --network host \
#     -e MYSQL_ALLOW_EMPTY_PASSWORD=yes mysql:8.0 --port=3308 \
#     --bind-address=127.0.0.1 --mysqlx=OFF --skip-log-bin \
#     --max-connections=1000 --innodb-buffer-pool-size=4G \
#     --innodb-lock-wait-timeout=10
set -euo pipefail
out=${1:-compare.csv}
dur=${2:-8s}
durable=${3:-0}
cd "$(dirname "$0")/.."
bin=$(mktemp -d)
trap 'rm -rf "$bin"' EXIT
go build -o "$bin/server" ./cmd/server
go build -o "$bin/sqlbench" ./cmd/sqlbench
mysql_dsn=${MYSQL_DSN:-root@tcp(127.0.0.1:3308)/}
only=${ONLY:-all}
prep=()
suffix=
if [ "${PREPARE:-0}" = 1 ]; then
	prep=(-prepare)
	suffix=-prep
fi
data_dir=${DATA_DIR:-$PWD/.benchdata}
ours_dsn='root@tcp(127.0.0.1:3307)/'
flush=0
[ "$durable" = 1 ] && flush=1
docker exec celltnx-mysql mysql -uroot -e "SET GLOBAL innodb_flush_log_at_trx_commit=$flush"

runs() { # dsn label extra-args...
	local dsn=$1 label=$2
	shift 2
	b() { "$bin/sqlbench" -dsn "$dsn" -label "$label$suffix" -clients 32 -duration "$dur" -setup -csv "$out" "${prep[@]}" "$@"; }
	for wh in 1 4; do
		b -workload tpcc -warehouses "$wh" -customers 100 -items 5000 "$@"
	done
	b -workload tpcc -warehouses 1 -customers 100 -items 5000 -think 1ms "$@"
	for theta in 0.5 0.9 0.99; do
		b -workload users -rows 1000 -theta "$theta" -disjoint 0.5 "$@"
	done
	for dj in 0 0.9; do
		b -workload users -rows 1000 -theta 0.99 -disjoint "$dj" "$@"
	done
}

if [ "$only" != ours ]; then
	runs "$mysql_dsn" mysql-serializable -isolation SERIALIZABLE
	runs "$mysql_dsn" mysql-repeatable-read -isolation REPEATABLE-READ
fi
[ "$only" = mysql ] && exit 0
for mode in row cell cell+delta; do
	args=(-addr 127.0.0.1:3307 -mode "$mode")
	if [ "$durable" = 1 ]; then
		rm -rf "$data_dir/$mode"
		mkdir -p "$data_dir/$mode"
		args+=(-dir "$data_dir/$mode")
	fi
	"$bin/server" "${args[@]}" >/dev/null 2>&1 &
	pid=$!
	sleep 1
	runs "$ours_dsn" "cell-tnx-$mode"
	kill "$pid"
	wait "$pid" || true
	rm -rf "${data_dir:?}/$mode"
done
