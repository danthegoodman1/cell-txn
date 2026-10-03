# Results

Measured 2026-10-03 on one 24-core Linux machine with NVMe storage (ext4). Clients and servers share the machine; MySQL 8.0.46 runs in Docker on the host network. Each SQL benchmark point is the median of three 8-second runs: run to run, cell+delta points varied by up to 10% and MySQL SERIALIZABLE's by up to 70%. In-process points are single 3-second runs.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | spec scenarios 1–9, indexes, savepoints, GC, overflow, SQL scenarios, durable restart, plan cache, process list | pass under `-race` |
| Stress tests | every workload on real goroutines, all modes, full history replay | pass under `-race` |
| Stress under load | TPC-C stress tests in every mode, 600 times each beside a simulator sweep (5,400 runs) | 0 failures |
| Core simulation | 30,000 seeds, every workload, mode, disk setting and commit delay and interleave probability; 463,644 crashes with recovery checks | 0 failures |
| Per-workload simulation | 30,000 seeds each for random, users and TPC-C; 1,695,016 crashes | 0 failures |
| SQL simulation | 30,000 seeds through go-mysql-server and the plan cache, which re-analyzes every reused plan, replayed against go-mysql-server's reference engine; 233,778 conflict retries | 0 failures |
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
| TPC-C, 1 warehouse | 1,254 | 1,588 | 618 (6.7) | 1,245 (6.9) | 2,096 (1.4) |
| TPC-C, 4 warehouses | 3,681 | 4,091 | 1,520 (2.4) | 3,210 (1.4) | 4,353 (0.3) |
| TPC-C, 1 warehouse, 1 ms think | 85 | 98 | 79 (6.8) | 162 (9.4) | 265 (1.4) |
| users, θ=0.5, 50% disjoint | 56,040 | 53,354 | 159,531 | 167,360 | 168,508 |
| users, θ=0.9, 50% disjoint | 40,008 | 40,587 | 71,277 | 93,438 | 95,129 |
| users, θ=0.99, 0% disjoint | 32,044 | 29,946 | 43,831 | 42,494 | 44,657 |
| users, θ=0.99, 50% disjoint | 35,592 | 34,005 | 54,581 | 75,901 | 77,410 |
| users, θ=0.99, 90% disjoint | 39,505 | 39,400 | 87,878 | 189,668 | 191,663 |

### fsync per commit (cell-tnx on Badger in sync mode; `innodb_flush_log_at_trx_commit=1`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,233 | 1,806 | 395 (9.7) | 836 (6.3) | 2,142 (0.7) |
| TPC-C, 4 warehouses | 1,967 | 2,437 | 1,409 (1.8) | 2,382 (0.8) | 3,073 (0.1) |
| TPC-C, 1 warehouse, 1 ms think | 85 | 102 | 80 (6.8) | 143 (11.6) | 254 (1.4) |
| users, θ=0.5, 50% disjoint | 3,324 | 3,256 | 5,825 | 5,816 | 5,832 |
| users, θ=0.9, 50% disjoint | 3,213 | 3,332 | 5,192 | 5,677 | 5,680 |
| users, θ=0.99, 0% disjoint | 3,211 | 3,276 | 3,406 | 3,378 | 3,440 |
| users, θ=0.99, 50% disjoint | 3,140 | 3,388 | 4,357 | 5,500 | 5,399 |
| users, θ=0.99, 90% disjoint | 3,340 | 3,422 | 6,138 | 6,488 | 6,470 |

### Commit delay

`-commitdelay` makes the writer sleep before each flush, so commits arriving meanwhile share its sync. cell+delta with fsync per commit; commits/s, the change from no delay, and p50 and p99 commit latency in ms.

| Commit delay | users, θ=0.5, 50% disjoint | TPC-C, 1 warehouse | TPC-C, 4 warehouses |
|---|---|---|---|
| none | 5,763; p50 4.7, p99 12 | 2,062; p50 9.3, p99 101 | 3,089; p50 9.4, p99 29 |
| 100 µs | 5,702 (−1%); p50 4.9, p99 12 | 2,233 (+8%); p50 9.5, p99 87 | 3,193 (+3%); p50 9.6, p99 27 |
| 250 µs | 5,625 (−2%); p50 5.1, p99 25 | 2,249 (+9%); p50 9.9, p99 79 | 3,129 (+1%); p50 9.1, p99 26 |
| 500 µs | 7,974 (+38%); p50 3.0, p99 11 | 2,300 (+12%); p50 9.5, p99 72 | 2,990 (−3%); p50 9.3, p99 27 |
| 750 µs | 6,930 (+20%); p50 4.1, p99 11 | 2,176 (+6%); p50 10.0, p99 79 | 2,902 (−6%); p50 9.8, p99 28 |
| 1 ms | 6,371 (+11%); p50 4.4, p99 11 | 2,119 (+3%); p50 10.5, p99 89 | 2,777 (−10%); p50 10.5, p99 30 |

A Badger write and sync takes about 2.4 ms here, and the writer syncs almost continuously. Without a delay, users commits split into two alternating groups: a client commits again about 0.1 ms after its last commit returns, just after the next sync has started, so each commit waits through about two syncs. A 500 µs delay merges the groups, raising throughput by 38% and cutting p50 latency from 4.7 to 3.0 ms. TPC-C transactions spend several milliseconds in SQL, so their commits arrive spread out: a delay gains up to 12% at 1 warehouse, where it also trims retries and p99 latency, and at 4 warehouses gains 3% at 100 µs and costs throughput beyond. The delay is off by default.

### Plan cache

The server's plan cache reuses an analyzed plan for a statement that differs from an earlier one only in its literals (see `SPEC.md`). cell+delta without fsync, with `-noplancache` and without; medians of three alternating runs, commits/s.

| Workload | planned in full | plan cache | change |
|---|---|---|---|
| users, θ=0.5, 50% disjoint | 134,780 | 169,433 | +26% |
| TPC-C, 1 warehouse | 1,812 | 2,125 | +17% |
| TPC-C, 4 warehouses | 3,749 | 4,370 | +17% |

### Prepared statements

With `PREPARE=1`, every statement with arguments goes as a server-side prepared statement. Both servers then skip parsing. The plan cache covers only text statements, so go-mysql-server builds and analyzes every prepared execution in full. No per-commit fsync; commits/s.

| Workload | MySQL SERIALIZABLE, text | MySQL SERIALIZABLE, prepared | cell+delta, text | cell+delta, prepared |
|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,254 | 1,605 | 2,096 | 2,164 |
| TPC-C, 4 warehouses | 3,681 | 3,978 | 4,353 | 4,483 |
| users, θ=0.5, 50% disjoint | 56,040 | 56,501 | 168,508 | 136,952 |

### Where the server's time goes

CPU profiles of the cell+delta server without fsync (`-pprof`), as shares of server CPU.

| Stage | TPC-C, 4 warehouses (10 cores busy) | users, θ=0.5 (14 cores busy) |
|---|---|---|
| Row execution and storage | 38% | 29% |
| GC | 16% | 21% |
| Wire protocol | 12% | 15% |
| Scheduler and other runtime | 12% | 11% |
| Plan cache: keying statements and rebuilding plans | 10% | 11% |
| Parsing | 10% | 9% |
| Plan building and analysis | 1% | 0% |
| Commit | 1% | 4% |

Plan building and analysis remain only for statements the cache doesn't cover, such as TPC-C's customer lookup by last name.

## What the numbers say

- **Cell-level tracking pays off where rows are hot and transactions touch disjoint columns.** Over SQL on TPC-C, cell+delta does 3.4× row-level OCC at 1 warehouse (2,096 against 618) and 2.9× at 4. Cell granularity alone gives about 2×; deltas add the rest, mostly by making Payment's YTD and balance updates commute.
- **cell+delta beats MySQL SERIALIZABLE on every workload.** Without fsync it does 1.7× MySQL on TPC-C at 1 warehouse and 1.2× at 4, and 1.4–4.9× on users. With fsync on both sides it wins TPC-C by 1.6–1.7× and users by 1.1–1.9×. With 1 ms between statements, MySQL holds its locks across the pauses and cell+delta does 3.0–3.1× its throughput. It also beats MySQL REPEATABLE READ, a weaker level, everywhere.
- **Over SQL, go-mysql-server's per-statement cost still bounds throughput.** TPC-C peaks near 4,400 commits/s over SQL against 43,000–52,000 in process. The plan cache removes plan building and analysis for repeated statement shapes, worth 17–26%; row execution, GC, the wire protocol and parsing now take most of the server's CPU, and the commit 1–4%. Each client runs its statements one at a time, so per-statement latency sets the rate.
- **Hot read-modify-write gains least.** When most traffic hits one row and transactions read and rewrite the same column (users at θ=0.99 with 0% disjoint), every mode retries, and cell+delta leads MySQL SERIALIZABLE by only 1.1–1.4×. NewOrder's `D_NEXT_O_ID` increment is the same pattern and accounts for most of cell+delta's remaining TPC-C retries.
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
