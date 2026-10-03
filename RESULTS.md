# Results

Measured 2026-10-02 on one 24-core Linux machine with NVMe storage (ext4). Clients and servers share the machine; MySQL 8.0.46 runs in Docker on the host network. Each benchmark point is a single 8-second run (3 seconds in process), so expect roughly ±15% run-to-run variation.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | spec scenarios 1–9, indexes, savepoints, GC, overflow, SQL scenarios, durable restart | pass under `-race` |
| Stress tests | every workload on real goroutines, all modes, full history replay | pass under `-race` |
| Core simulation | 30,000 seeds, every workload, mode and disk setting; 303,745 crashes with recovery checks | 0 failures |
| Per-workload simulation | 30,000 seeds each for random, users and TPC-C; about 1.1 million crashes | 0 failures |
| SQL simulation | 30,000 seeds through go-mysql-server, replayed against its reference engine; 212,472 conflict retries | 0 failures |
| Determinism | every tenth seed rerun in every sweep (15,000 reruns) | 0 mismatches |
| Self-test | 10 deliberate bugs, internal assertions off | all found, by seed 747 at the latest |
| Race detector | 1,000 parallel SQL seeds in a `-race` build | no races |
| Crash test | 8 rounds of `kill -9` during TPC-C on one Badger store, about 22,000 commits | no acknowledged commit lost; recovered state equals the serial replay |
| go-mysql-server engine tests | 14 suites | 1,740 pass, 162 fail, 36 skip; see `sqlgms/enginetest.txt` |

The engine-test failures are features outside the prototype (ALTER TABLE, foreign keys, triggers, temporary tables, prefix indexes, events and procedures), metadata display (DESCRIBE and SHOW CREATE TABLE), LAST_INSERT_ID reporting after ON DUPLICATE KEY UPDATE, and transaction scripts that expect REPEATABLE READ outcomes where SERIALIZABLE must abort one writer.

The simulator also found real bugs while it was being built. Its oracle exposed two go-mysql-server bugs: the reference engine's ROLLBACK leaves secondary indexes stale, and engine construction races on package globals. It also caught a deferred CHECK that a later delete could hide, a load that could be lost before its first sync, and several harness mistakes. The engine tests found missing MySQL savepoint semantics and missing statement-level read consistency for UPDATE … JOIN. All of these are fixed.

## In process (no SQL)

TPC-C NewOrder and Payment, 1 warehouse, 16 clients, `GOGC=400`; commits/s.

| Think time | row | cell | cell+delta |
|---|---|---|---|
| none | 42,772 | 45,177 | 43,080 |
| 50% of operations pause 1–50µs | 31,233 | 46,250 | 51,300 |

Without pauses all modes are bounded by the global commit lock (p99 hold 25–40µs). With pauses, conflicts decide throughput: cell tracking gains 48% over row-level and deltas a further 11%. Without pauses, cell+delta still has 7.5× fewer conflicts than row mode (23,316 against 175,624).

## Over SQL against MySQL

Identical SQL from `cmd/sqlbench`, 32 clients; commits/s, with retries per transaction in parentheses for cell-tnx (MySQL never retried).

### No per-commit fsync (cell-tnx in memory; `innodb_flush_log_at_trx_commit=0`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,430 | 1,708 | 382 (6.6) | 799 (7.6) | 1,283 (1.4) |
| TPC-C, 4 warehouses | 3,991 | 4,089 | 948 (2.4) | 2,047 (1.4) | 2,673 (0.3) |
| TPC-C, 1 warehouse, 1 ms think | 88 | 99 | 72 (6.9) | 151 (10.1) | 241 (1.4) |
| users, θ=0.5 | 52,590 | 56,312 | 29,471 | 29,354 | 29,698 |
| users, θ=0.99, 0% disjoint | 37,875 | 29,396 | 12,622 | 12,524 | 13,860 |
| users, θ=0.99, 50% disjoint | 40,606 | 35,784 | 19,090 | 22,843 | 24,392 |
| users, θ=0.99, 90% disjoint | 40,041 | 42,204 | 33,953 | 53,220 | 53,222 |

### fsync per commit (cell-tnx on Badger in sync mode; `innodb_flush_log_at_trx_commit=1`)

| Workload | MySQL SERIALIZABLE | MySQL REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 1,318 | 1,831 | 362 (9.0) | 849 (6.1) | 1,845 (0.8) |
| TPC-C, 4 warehouses | 1,949 | 2,349 | 1,172 (2.0) | 2,256 (0.8) | 2,893 (0.1) |
| TPC-C, 1 warehouse, 1 ms think | 82 | 105 | 75 (6.7) | 133 (12.4) | 215 (1.6) |
| users, θ=0.5 | 3,283 | 3,317 | 5,968 | 6,006 | 5,926 |
| users, θ=0.99, 0% disjoint | 3,080 | 3,258 | 3,066 | 4,073 | 2,203 |
| users, θ=0.99, 50% disjoint | 3,239 | 3,392 | 4,635 | 5,732 | 5,671 |
| users, θ=0.99, 90% disjoint | 3,374 | 3,454 | 6,178 | 6,501 | 6,513 |

## What the numbers say

- **Cell-level tracking pays off where rows are hot and transactions touch disjoint columns.** Over SQL on TPC-C, cell+delta does 3.4× row-level OCC at 1 warehouse (1,283 against 382) and 2.8× at 4. Cell granularity alone gives about 2.1×; deltas add the rest, mostly by making Payment's YTD and balance updates commute.
- **Against MySQL, cell+delta is competitive with locking SERIALIZABLE and wins when transactions are interactive.** With fsync on both sides it beats MySQL SERIALIZABLE on TPC-C at 1 warehouse (1.4×) and 4 warehouses (1.5×). With 1 ms between statements, MySQL holds its locks across the pauses and cell+delta does 2.6–2.7× its throughput. Users updates on disjoint columns beat MySQL by 1.3× without fsync and 1.9× with it.
- **MySQL wins where per-statement cost dominates.** At low contention, MySQL runs single statements about 1.8× as fast: go-mysql-server parses and plans every statement in Go, which MySQL does in C++. The in-process numbers, 30–50k TPC-C commits/s, show the store itself is not the limit.
- **Hot read-modify-write is OCC's weak spot.** When most traffic hits one row and transactions read and rewrite the same column (users at θ=0.99 with 0% disjoint), every mode retries heavily and MySQL's locking does better. NewOrder's `D_NEXT_O_ID` increment is the same pattern and accounts for most of cell+delta's remaining TPC-C retries.
- **The global commit lock is not the bottleneck over SQL.** TPC-C over SQL peaks near 2,900 commits/s, against an in-process capacity of 40,000–50,000.

## Reproducing

```sh
go test -race ./...
go run ./cmd/sim -seeds 30000 && go run ./cmd/sim -seeds 30000 -workload sql
go test -tags simbugs -run SelfTest ./sim
bench/enginetest.sh
go run ./cmd/bench -workload tpcc -warehouses 1 -think 0.5 -clients 16
bench/compare.sh out.csv 8s 0 && bench/compare.sh out.csv 8s 1
```
