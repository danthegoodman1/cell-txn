# Results

Measured 2026-10-03 on one 24-core Linux machine with NVMe storage (ext4). Clients and servers share the machine; MySQL 8.0.46 runs in Docker on the host network. Each SQL benchmark point is the median of three 8-second runs: run to run, cell-tnx points varied by up to 10% and MySQL's by up to 35%. In-process points are single 3-second runs.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | spec scenarios 1–9, indexes, savepoints, GC, overflow, SQL scenarios, durable restart | pass under `-race` |
| Stress tests | every workload on real goroutines, all modes, full history replay | pass under `-race` |
| Stress under load | TPC-C stress tests in every mode, 600 times each beside a simulator sweep (5,400 runs) | 0 failures |
| Core simulation | 30,000 seeds, every workload, mode, disk setting and commit delay and interleave probability; 463,644 crashes with recovery checks | 0 failures |
| Per-workload simulation | 30,000 seeds each for random, users and TPC-C; 1,695,016 crashes | 0 failures |
| SQL simulation | 30,000 seeds through go-mysql-server, replayed against its reference engine; 233,778 conflict retries | 0 failures |
| Determinism | every tenth seed rerun in every sweep (15,000 reruns) | 0 mismatches |
| Self-test | 14 deliberate bugs, internal assertions off | all found, by seed 367 at the latest |
| Race detector | 1,000 parallel SQL seeds in a `-race` build | no races |
| Crash test | 8 rounds of `kill -9` during TPC-C on one Badger store, half with a 500µs commit delay; about 18,200 commits | no acknowledged commit lost; recovered state equals the serial replay |
| go-mysql-server engine tests | 14 suites | 1,740 pass, 162 fail, 36 skip; see `sqlgms/enginetest.txt` |

The engine-test failures are features outside the prototype (ALTER TABLE, foreign keys, triggers, temporary tables, prefix indexes, events and procedures), metadata display (DESCRIBE and SHOW CREATE TABLE), LAST_INSERT_ID reporting after ON DUPLICATE KEY UPDATE, and transaction scripts that expect REPEATABLE READ outcomes where SERIALIZABLE must abort one writer.

The simulator also found real bugs while it was being built. Its oracle exposed two go-mysql-server bugs: the reference engine's ROLLBACK leaves secondary indexes stale, and engine construction races on package globals. It also caught a deferred CHECK that a later delete could hide, a load that could be lost before its first sync, and several harness mistakes. The engine tests found missing MySQL savepoint semantics and missing statement-level read consistency for UPDATE … JOIN. The stress tests found a lock-free skiplist seek that reloaded its result after comparing, so a key the committer linked in between could make a scan or point read miss existing rows; the simulator now interleaves commits with lock-free reads at fine-grained points, and three self-test bugs that only those points expose check that it catches this class. All of these are fixed.

## In process (no SQL)

TPC-C NewOrder and Payment, 1 warehouse, 16 clients, `GOGC=400`; commits/s.

| Think time | row | cell | cell+delta |
|---|---|---|---|
| none | 43,088 | 46,478 | 42,975 |
| 50% of operations pause 1–50µs | 31,000 | 46,907 | 53,059 |

Without pauses all modes are bounded by the global commit lock (p99 hold 25–37µs). With pauses, conflicts decide throughput: cell tracking gains 51% over row-level and deltas a further 13%. Without pauses, cell+delta still has 7.7× fewer conflicts than row mode (23,186 against 177,474).

## Over SQL against MySQL

Identical SQL from `cmd/sqlbench`, 32 clients; commits/s, with retries per transaction in parentheses for cell-tnx on TPC-C (MySQL never retried). Statements go as text with their arguments interpolated.

### No per-commit fsync (cell-tnx in memory; `innodb_flush_log_at_trx_commit=0`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,419 | 1,712 | 505 (6.7) | 1,108 (6.9) | 1,733 (1.4) |
| TPC-C, 4 warehouses | 3,344 | 4,323 | 1,244 (2.4) | 2,786 (1.4) | 3,579 (0.3) |
| TPC-C, 1 warehouse, 1 ms think | 88 | 97 | 73 (6.6) | 161 (9.2) | 258 (1.4) |
| users, θ=0.5, 50% disjoint | 56,996 | 56,912 | 124,876 | 129,431 | 129,080 |
| users, θ=0.9, 50% disjoint | 40,847 | 36,787 | 51,661 | 74,998 | 78,925 |
| users, θ=0.99, 0% disjoint | 36,640 | 30,559 | 31,936 | 31,503 | 33,908 |
| users, θ=0.99, 50% disjoint | 39,446 | 33,389 | 38,806 | 58,481 | 62,824 |
| users, θ=0.99, 90% disjoint | 42,003 | 45,059 | 55,522 | 107,439 | 107,038 |

### fsync per commit (cell-tnx on Badger in sync mode; `innodb_flush_log_at_trx_commit=1`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,235 | 1,750 | 369 (9.1) | 841 (6.3) | 1,977 (0.8) |
| TPC-C, 4 warehouses | 1,951 | 2,384 | 1,310 (1.9) | 2,410 (0.8) | 3,109 (0.1) |
| TPC-C, 1 warehouse, 1 ms think | 88 | 103 | 77 (6.8) | 140 (11.3) | 241 (1.4) |
| users, θ=0.5, 50% disjoint | 3,257 | 3,360 | 5,930 | 5,939 | 5,856 |
| users, θ=0.9, 50% disjoint | 3,198 | 3,340 | 5,395 | 5,816 | 5,820 |
| users, θ=0.99, 0% disjoint | 3,186 | 3,337 | 3,665 | 3,732 | 3,666 |
| users, θ=0.99, 50% disjoint | 3,188 | 3,389 | 4,543 | 5,598 | 5,636 |
| users, θ=0.99, 90% disjoint | 3,399 | 3,422 | 6,187 | 6,367 | 6,488 |

### Commit delay

`-commitdelay` makes the writer sleep before each flush, so commits arriving meanwhile share its sync. cell+delta with fsync per commit; commits/s, the change from no delay, and p50 and p99 commit latency in ms.

| Commit delay | users, θ=0.5, 50% disjoint | TPC-C, 1 warehouse | TPC-C, 4 warehouses |
|---|---|---|---|
| none | 5,883; p50 4.7, p99 11 | 1,979; p50 9.3, p99 115 | 3,132; p50 9.4, p99 28 |
| 100 µs | 5,742 (−2%); p50 4.9, p99 11 | 2,111 (+7%); p50 9.6, p99 95 | 3,164 (+1%); p50 9.7, p99 27 |
| 250 µs | 6,331 (+8%); p50 5.0, p99 11 | 2,083 (+5%); p50 10.0, p99 97 | 3,108 (−1%); p50 9.6, p99 27 |
| 500 µs | 7,684 (+31%); p50 3.0, p99 11 | 2,168 (+10%); p50 9.8, p99 87 | 2,996 (−4%); p50 9.0, p99 26 |
| 750 µs | 7,324 (+24%); p50 3.6, p99 11 | 2,105 (+6%); p50 9.9, p99 88 | 2,871 (−8%); p50 9.7, p99 28 |
| 1 ms | 6,522 (+11%); p50 4.4, p99 12 | 2,034 (+3%); p50 10.4, p99 95 | 2,799 (−11%); p50 10.4, p99 30 |

A Badger write and sync takes about 2.4 ms here, and the writer syncs almost continuously. Without a delay, users commits split into two alternating groups: a client commits again about 0.1 ms after its last commit returns, just after the next sync has started, so each commit waits through about two syncs. A 500 µs delay merges the groups, raising throughput by 31% and cutting p50 latency from 4.7 to 3.0 ms. TPC-C transactions spend about 7 ms in SQL, so their commits arrive spread out: a delay gains up to 10% at 1 warehouse, where it also trims retries and p99 latency, and costs throughput at 4 warehouses beyond 100 µs. The delay is off by default.

### Prepared statements

With `PREPARE=1`, every statement with arguments goes as a server-side prepared statement. Both servers then skip parsing; go-mysql-server still rebuilds and analyzes the plan on every execution. No per-commit fsync; commits/s.

| Workload | MySQL SERIALIZABLE, text | MySQL SERIALIZABLE, prepared | cell+delta, text | cell+delta, prepared |
|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,419 | 1,568 | 1,733 | 2,193 |
| TPC-C, 4 warehouses | 3,344 | 4,023 | 3,579 | 4,428 |
| users, θ=0.5, 50% disjoint | 56,996 | 66,645 | 129,080 | 131,754 |

### Where the server's time goes

CPU profiles of the cell+delta server without fsync (`-pprof`), as shares of server CPU.

| Stage | TPC-C, 4 warehouses (10 cores busy) | users, θ=0.5 (15 cores busy) |
|---|---|---|
| Parse, plan and analyze | 41% | 35% |
| Wire protocol | 19% | 21% |
| GC | 16% | 21% |
| Row execution and storage | 14% | 13% |
| Commit | 1% | 2% |
| Scheduler and other runtime | 7% | 7% |

Mutex contention sits in go-mysql-server's global process list, which every statement locks to begin and end; the commit lock accounts for under 5% of it.

## What the numbers say

- **Cell-level tracking pays off where rows are hot and transactions touch disjoint columns.** Over SQL on TPC-C, cell+delta does 3.4× row-level OCC at 1 warehouse (1,733 against 505) and 2.9× at 4. Cell granularity alone gives about 2.2×; deltas add the rest, mostly by making Payment's YTD and balance updates commute.
- **Against MySQL SERIALIZABLE, cell+delta wins everywhere except hot read-modify-write without fsync.** Without fsync it does 1.2× MySQL on TPC-C at 1 warehouse, matches it at 4, and beats it by 1.6–2.6× on users updates that touch disjoint columns. With fsync on both sides it wins TPC-C by 1.6× and users by 1.8–1.9×. With 1 ms between statements, MySQL holds its locks across the pauses and cell+delta does 2.7–2.9× its throughput. MySQL REPEATABLE READ, a weaker level, still wins TPC-C at 4 warehouses without fsync.
- **Over SQL, go-mysql-server's per-statement cost bounds throughput.** TPC-C peaks near 3,600 commits/s over SQL (4,400 with prepared statements) against 43,000–53,000 in process. Parsing, planning and analysis take about 40% of the server's CPU; the commit takes 1–2%. The server is not CPU-saturated on TPC-C: each client runs its statements one at a time, so per-statement latency sets the rate.
- **Hot read-modify-write is OCC's weak spot.** When most traffic hits one row and transactions read and rewrite the same column (users at θ=0.99 with 0% disjoint), every mode retries and MySQL's locking does slightly better without fsync. NewOrder's `D_NEXT_O_ID` increment is the same pattern and accounts for most of cell+delta's remaining TPC-C retries.
- **With fsync, cell-tnx commits users updates at about 5,900/s against MySQL's 3,300.** The single writer covers every queued commit with one sync, and a commit's p50 latency is 4.7 ms against MySQL's 8.1 ms. A 500 µs commit delay raises that to 7,700/s at 3.0 ms; it suits short transactions and is tuned per workload.

## Reproducing

```sh
go test -race ./...
go run ./cmd/sim -seeds 30000 && go run ./cmd/sim -seeds 30000 -workload sql
go test -tags simbugs -run SelfTest ./sim
bench/enginetest.sh
go run ./cmd/bench -workload tpcc -warehouses 1 -think 0.5 -clients 16
bench/compare.sh out.csv 8s 0 && bench/compare.sh out.csv 8s 1
PREPARE=1 bench/compare.sh out.csv 8s 0
SERVER_ARGS=-commitdelay=500us ONLY=ours bench/compare.sh out.csv 8s 1
```
