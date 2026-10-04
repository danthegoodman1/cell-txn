# Results

Measured 2026-10-03 on one 24-core Linux machine with NVMe storage (ext4). Clients and servers share the machine; MySQL 8.0.46 runs in Docker on the host network. Each SQL benchmark point is the median of three 8-second runs: run to run, cell+delta points varied by up to 12% and MySQL SERIALIZABLE's by up to 66%. In-process points are single 3-second runs.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | spec scenarios 1–9, indexes, savepoints, GC, overflow, SQL scenarios, durable restart, plan cache, process list | pass under `-race` |
| Stress tests | every workload on real goroutines, all modes, full history replay | pass under `-race` |
| Stress under load | TPC-C stress tests in every mode, 600 times each beside a simulator sweep (5,400 runs) | 0 failures |
| Core simulation | 30,000 seeds, every workload, mode, disk setting and commit delay and interleave probability; 463,644 crashes with recovery checks | 0 failures |
| Per-workload simulation | 30,000 seeds each for random, users and TPC-C; 1,695,016 crashes | 0 failures |
| SQL simulation | 30,000 seeds through go-mysql-server and the plan cache, which re-analyzes every reused plan, with a drawn share of statements sent as prepared statements, replayed as text against go-mysql-server's reference engine; 232,623 conflict retries | 0 failures |
| Determinism | every tenth seed rerun in every sweep (15,000 reruns) | 0 mismatches |
| Self-test | 14 deliberate bugs, internal assertions off | all found, by seed 367 at the latest |
| Race detector | 1,000 parallel SQL seeds in a `-race` build | no races |
| Crash test | 8 rounds of `kill -9` during TPC-C on one Badger store, half with a 500µs commit delay; about 18,200 commits | no acknowledged commit lost; recovered state equals the serial replay |
| go-mysql-server engine tests | 14 suites, through the plan cache, which checks each new template and every reuse against a fresh analysis | 1,740 pass, 162 fail, 36 skip; see `sqlgms/enginetest.txt` |

The engine-test failures are features outside the prototype (ALTER TABLE, foreign keys, triggers, temporary tables, prefix indexes, events and procedures), metadata display (DESCRIBE and SHOW CREATE TABLE), LAST_INSERT_ID reporting after ON DUPLICATE KEY UPDATE, and transaction scripts that expect REPEATABLE READ outcomes where SERIALIZABLE must abort one writer.

The simulator also found real bugs while it was being built. Its oracle exposed two go-mysql-server bugs: the reference engine's ROLLBACK leaves secondary indexes stale, and engine construction races on package globals. It also caught a deferred CHECK that a later delete could hide, a load that could be lost before its first sync, and several harness mistakes. The engine tests found missing MySQL savepoint semantics and missing statement-level read consistency for UPDATE … JOIN. The stress tests found a lock-free skiplist seek that reloaded its result after comparing, so a key the committer linked in between could make a scan or point read miss existing rows; the simulator now interleaves commits with lock-free reads at fine-grained points, and three self-test bugs that only those points expose check that it catches this class. All of these are fixed.

## In process (no SQL)

TPC-C NewOrder and Payment, 1 warehouse, 16 clients, `GOGC=400`; commits/s.

| Think time | row | cell | cell+delta |
|---|---|---|---|
| none | 42,560 | 45,124 | 43,021 |
| 50% of operations pause 1–50µs | 30,924 | 45,380 | 51,528 |

Without pauses all modes are bounded by the global commit lock (p99 hold 27–37µs). With pauses, conflicts decide throughput: cell tracking gains 47% over row-level and deltas a further 14%. Without pauses, cell+delta still has 7.4× fewer conflicts than row mode (23,379 against 174,065).

## Over SQL against MySQL

Identical SQL from `cmd/sqlbench`, 32 clients; commits/s, with retries per transaction in parentheses for cell-tnx on TPC-C (MySQL never retried). Statements go as text with their arguments interpolated.

### No per-commit fsync (cell-tnx in memory; `innodb_flush_log_at_trx_commit=0`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,216 | 1,655 | 701 (6.7) | 1,365 (6.6) | 2,372 (1.3) |
| TPC-C, 4 warehouses | 3,658 | 4,158 | 1,690 (2.4) | 3,536 (1.4) | 4,784 (0.3) |
| TPC-C, 1 warehouse, 1 ms think | 84 | 97 | 79 (7.1) | 160 (9.8) | 264 (1.5) |
| users, θ=0.5, 50% disjoint | 53,274 | 52,336 | 176,885 | 185,334 | 185,928 |
| users, θ=0.9, 50% disjoint | 36,955 | 35,108 | 81,095 | 105,823 | 107,718 |
| users, θ=0.99, 0% disjoint | 35,386 | 31,230 | 48,808 | 48,180 | 49,966 |
| users, θ=0.99, 50% disjoint | 41,221 | 35,866 | 62,521 | 85,267 | 88,110 |
| users, θ=0.99, 90% disjoint | 36,927 | 37,953 | 98,405 | 210,784 | 211,180 |

### fsync per commit (cell-tnx on Badger in sync mode; `innodb_flush_log_at_trx_commit=1`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,262 | 1,800 | 413 (9.9) | 861 (6.2) | 2,248 (0.6) |
| TPC-C, 4 warehouses | 1,957 | 2,392 | 1,503 (1.8) | 2,446 (0.8) | 3,168 (0.1) |
| TPC-C, 1 warehouse, 1 ms think | 85 | 103 | 81 (6.9) | 142 (12.0) | 254 (1.4) |
| users, θ=0.5, 50% disjoint | 3,271 | 3,341 | 5,809 | 5,804 | 5,813 |
| users, θ=0.9, 50% disjoint | 3,236 | 3,338 | 5,083 | 5,656 | 5,673 |
| users, θ=0.99, 0% disjoint | 2,532 | 3,270 | 3,411 | 3,328 | 3,442 |
| users, θ=0.99, 50% disjoint | 3,156 | 3,406 | 4,164 | 5,431 | 5,381 |
| users, θ=0.99, 90% disjoint | 3,313 | 3,418 | 6,001 | 6,367 | 6,470 |

### Commit delay

`-commitdelay` makes the writer sleep before each flush, so commits arriving meanwhile share its sync. cell+delta with fsync per commit; commits/s, the change from no delay, and p50 and p99 commit latency in ms.

| Commit delay | users, θ=0.5, 50% disjoint | TPC-C, 1 warehouse | TPC-C, 4 warehouses |
|---|---|---|---|
| none | 5,721; p50 4.7, p99 12 | 2,163; p50 9.2, p99 95 | 3,190; p50 9.3, p99 27 |
| 100 µs | 5,754 (+1%); p50 4.9, p99 12 | 2,482 (+15%); p50 9.3, p99 70 | 3,313 (+4%); p50 8.5, p99 25 |
| 250 µs | 7,248 (+27%); p50 2.8, p99 11 | 2,493 (+15%); p50 9.2, p99 63 | 3,224 (+1%); p50 8.6, p99 24 |
| 500 µs | 8,002 (+40%); p50 3.0, p99 11 | 2,458 (+14%); p50 9.3, p99 63 | 3,065 (−4%); p50 9.1, p99 25 |
| 750 µs | 6,806 (+19%); p50 4.2, p99 11 | 2,455 (+13%); p50 9.8, p99 61 | 2,944 (−8%); p50 9.8, p99 26 |
| 1 ms | 5,509 (−4%); p50 4.5, p99 22 | 2,340 (+8%); p50 10.3, p99 68 | 2,840 (−11%); p50 10.4, p99 28 |

A Badger write and sync takes about 2.4 ms here, and the writer syncs almost continuously. Without a delay, users commits split into two alternating groups: a client commits again about 0.1 ms after its last commit returns, just after the next sync has started, so each commit waits through about two syncs. A 250–500 µs delay merges the groups, raising throughput by 27–40% and cutting p50 latency from 4.7 to about 3 ms. TPC-C transactions spend several milliseconds in SQL, so their commits arrive spread out: a delay gains up to 15% at 1 warehouse, where it also trims retries and p99 latency, and at 4 warehouses gains 4% at 100 µs and costs throughput beyond. The delay is off by default.

### Plan cache

The server's plan cache reuses an analyzed plan for a statement that differs from an earlier one only in its literals, and finds text statements by their tokens without parsing them (see `SPEC.md`). cell+delta without fsync, with `-noplancache` and without; medians of three alternating runs, commits/s.

| Workload | Statements | planned in full | plan cache | change |
|---|---|---|---|---|
| users, θ=0.5, 50% disjoint | text | 131,378 | 185,130 | +41% |
| TPC-C, 1 warehouse | text | 1,793 | 2,379 | +33% |
| TPC-C, 4 warehouses | text | 3,704 | 4,797 | +30% |
| users, θ=0.5, 50% disjoint | prepared | 135,601 | 186,218 | +37% |
| TPC-C, 1 warehouse | prepared | 2,142 | 2,973 | +39% |
| TPC-C, 4 warehouses | prepared | 4,491 | 5,952 | +33% |

### Prepared statements

With `PREPARE=1`, every statement with arguments goes as a server-side prepared statement. Both servers then skip parsing, and cell-tnx's plan cache covers prepared statements as it covers text. No per-commit fsync; commits/s.

| Workload | MySQL SERIALIZABLE, text | MySQL SERIALIZABLE, prepared | cell+delta, text | cell+delta, prepared |
|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,216 | 1,591 | 2,372 | 2,954 |
| TPC-C, 4 warehouses | 3,658 | 3,762 | 4,784 | 5,888 |
| users, θ=0.5, 50% disjoint | 53,274 | 53,148 | 185,928 | 184,072 |

### Where the server's time goes

CPU profiles of the cell+delta server without fsync and with text statements (`-pprof`), as shares of server CPU.

| Stage | TPC-C, 4 warehouses | users, θ=0.5 |
|---|---|---|
| Row execution and storage | 43% | 33% |
| GC | 17% | 21% |
| Wire protocol | 13% | 17% |
| Scheduler and other runtime | 13% | 13% |
| Plan cache: tokens, keys and plan rebuilding | 12% | 12% |
| Commit | 2% | 5% |
| Parsing, plan building and analysis | 1% | 0% |

Parsing, plan building and analysis remain only for statements the cache doesn't cover, such as TPC-C's customer lookup by last name.

## What the numbers say

- **Cell-level tracking pays off where rows are hot and transactions touch disjoint columns.** Over SQL on TPC-C, cell+delta does 3.4× row-level OCC at 1 warehouse (2,372 against 701) and 2.8× at 4. Cell granularity alone gives about 2×; deltas add the rest, mostly by making Payment's YTD and balance updates commute.
- **cell+delta beats MySQL SERIALIZABLE on every workload.** Without fsync it does 2.0× MySQL on TPC-C at 1 warehouse and 1.3× at 4, and 1.4–5.7× on users. With fsync on both sides it wins TPC-C by 1.6–1.8× and users by 1.4–2.0×. With 1 ms between statements, MySQL holds its locks across the pauses and cell+delta does 3.0–3.1× its throughput. It also beats MySQL REPEATABLE READ, a weaker level, everywhere.
- **Over SQL, go-mysql-server's per-statement cost still bounds throughput.** TPC-C peaks near 4,800 commits/s with text statements and 5,900 with prepared ones, against 43,000–52,000 in process. The plan cache skips parsing, plan building and analysis for repeated statement shapes, worth 30–41%; row execution, GC and the wire protocol now take about 70% of the server's CPU, and the commit 2–5%. Each client runs its statements one at a time, so per-statement latency sets the rate.
- **Hot read-modify-write gains least.** When most traffic hits one row and transactions read and rewrite the same column (users at θ=0.99 with 0% disjoint), every mode retries, and cell+delta leads MySQL SERIALIZABLE by only 1.4×. NewOrder's `D_NEXT_O_ID` increment is the same pattern and accounts for most of cell+delta's remaining TPC-C retries.
- **With fsync, cell-tnx commits users updates at about 5,800/s against MySQL's 3,300.** The single writer covers every queued commit with one sync, and a commit's p50 latency is 4.7 ms against MySQL's 8.1 ms. A 500 µs commit delay raises that to 8,000/s at 3.0 ms; it suits short transactions and is tuned per workload.

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
SERVER_ARGS=-noplancache ONLY=ours bench/compare.sh out.csv 8s 0
```
