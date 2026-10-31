# MySQL engine results

Measured 2026-10-31 on one machine: an Intel Core Ultra 9 285 (24 cores), 125 GB of RAM and a Micron 2 TB NVMe drive with ext4, running Linux 7.0. MySQL 8.4.13 is built from source (RelWithDebInfo, GCC 15.2) by `bench/build-mysql.sh`, with `ha_celltnx.so` loaded beside InnoDB in the same `mysqld`. `bench/compare-mysql.sh` starts a fresh server for every configuration with identical flags and drives it with `cmd/sqlbench` over TCP from the same machine. Each point is the median of three 8-second runs; the median run-to-run spread is 2%, and the largest are InnoDB's under fsync (up to 38%).

InnoDB runs at SERIALIZABLE and REPEATABLE READ. `celltnx` runs in its three modes: `row` tracks whole rows, `cell` tracks (row, column) cells, and `cell+delta` also lets increments commute. Every mode queues `SELECT … FOR UPDATE` behind advisory row locks (`ENGINE.md`); the `-nowait` rows turn that off. Retries per transaction appear in parentheses where they are nonzero; InnoDB retries fewer than 0.1 per transaction everywhere.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | core, Go key compatibility, checker, workloads, threaded stress, lionturtle store | pass in debug, ASan and TSan builds |
| Golden traces | C++ and Go simulators, seeds 0–9,999: trace, steps, crashes, failure and parameters | identical |
| Simulation sweeps | 30,000 seeds each for random, users and TPC-C, and 30,000 over every workload with a drawn snapshot-refresh rate | 0 failures; 12,000 determinism reruns, 0 mismatches |
| Self-test | 15 deliberate bugs, including a refresh that skips validation | all found, by seed 594 at the latest |
| Store crash test | 8 rounds of `kill -9` during TPC-C on one lionturtle store, half with a 500 µs commit delay | no acknowledged commit lost; recovered state equals the serial replay |
| MTR suite | cells, commit_conflict, deltas, durable (two restarts), for_update, status | 6 of 6 pass |
| Benchmark audits | every TPC-C and ledger run below: TPC-C consistency conditions 1–4; every balance equals its opening plus the ledger's credits minus its debits, and the ledger holds exactly the committed transfers | 153 TPC-C and 264 ledger audits consistent |

TSan runs suppress reports inside lionturtle's Rust code, which TSan does not instrument. The wire oracle, a `mysqld`-level kill test and MySQL's own suites under `celltnx` are not yet built (`PLAN-MYSQL.md`).

## Hot accounts

The ledger workload moves money between 1,000 accounts. `accounts` holds `(id, balance, opening)` with `CHECK (balance >= 0)`; each transfer moves 1–100 between a hot account and a cold one, in either direction, in one transaction:

```sql
UPDATE accounts SET balance = balance - ? WHERE id = ?;  -- payer
UPDATE accounts SET balance = balance + ? WHERE id = ?;  -- payee
INSERT INTO entries VALUES (?, ?, ?, ?);
```

`hot=1` routes every transfer through one account, `hot=10` through one of ten, and `uniform` picks both accounts at random. Hot accounts open with 10⁹ and cold ones with 1,000, so a cold account occasionally runs dry: that transfer fails the CHECK and rolls back, as on InnoDB. In `cell+delta`, both UPDATEs are deltas, and the engine rechecks the CHECK on each merged balance at commit.

### No per-commit fsync, 32 clients

Transfers/s; p99 latency below.

| Shape | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta | cell+delta ÷ SERIALIZABLE |
|---|---|---|---|---|---|---|
| 1 hot | 10,738 | 10,841 | 9,441 (4.86) | 9,388 (4.88) | **101,390** | 9.4× |
| 10 hot | 65,038 | 64,913 | 42,730 (0.82) | 42,616 (0.82) | **101,440** | 1.6× |
| uniform | 83,865 | 84,647 | 91,737 (0.07) | 91,409 (0.07) | **102,912** | 1.2× |
| 1 hot, 1 ms pause between statements | 543 | 542 | 321 (18.0) | 321 (18.1) | **7,855** | 14.5× |
| 1 hot, prepared statements | 12,200 | 12,262 | | | **109,113** | 8.9× |

p99 at 1 hot: 7.7 ms for InnoDB, 0.65 ms for `cell+delta`. With pauses: 86 ms against 4.6 ms.

### Clients

1 hot account against uniform transfers, no per-commit fsync; transfers/s.

| Clients | InnoDB 1 hot | InnoDB uniform | cell+delta 1 hot | cell+delta uniform | 1 hot: cell+delta ÷ InnoDB |
|---|---|---|---|---|---|
| 8 | 17,767 | 49,418 | 59,348 | 59,876 | 3.3× |
| 32 | 10,818 | 85,959 | 103,524 | 103,820 | 9.6× |
| 128 | 8,653 | 75,100 | 124,274 | 125,651 | 14.4× |
| 256 | 6,856 | 56,648 | 104,574 | 103,251 | 15.3× |

InnoDB is SERIALIZABLE; REPEATABLE READ is within 2% on 1 hot. At 256 clients, p99 is 102 ms for InnoDB and 11 ms for `cell+delta`.

### fsync per commit

InnoDB with `innodb_flush_log_at_trx_commit=1`; `celltnx` with `celltnx_sync=ON`. Transfers/s.

| Shape | Clients | InnoDB SERIALIZABLE | row | cell | cell+delta | cell+delta ÷ InnoDB |
|---|---|---|---|---|---|---|
| 1 hot | 32 | 7,374 | 4,529 (3.86) | 4,561 (3.85) | **10,734** | 1.5× |
| 10 hot | 32 | 7,220 | 7,188 (0.82) | 7,183 (0.84) | **10,743** | 1.5× |
| uniform | 32 | 7,365 | 10,018 (0.05) | 10,082 (0.05) | **10,788** | 1.5× |
| 1 hot, 1 ms pauses | 32 | 412 | 272 (19.2) | 272 (19.2) | **3,829** | 9.3× |
| 1 hot | 128 | 8,411 | | | **27,288** | 3.2× |
| 10 hot | 128 | 25,806 | | | **27,346** | 1.1× |
| uniform | 128 | 26,035 | | | **27,650** | 1.1× |
| 1 hot, 1 ms pauses | 128 | 419 | | | **14,416** | 34× |

At 128 clients and 1 hot, p99 is 62 ms for InnoDB and 7.2 ms for `cell+delta`; with pauses, 630 ms against 11 ms.

A hot account costs `cell+delta` nothing: at every client count and both fsync settings, 1 hot runs within 2% of uniform. InnoDB holds the hot row's lock from its UPDATE until commit, so transfers through one account run one at a time, and pauses or an fsync inside that window divide throughput directly. Without deltas, `row` and `cell` fare worse than InnoDB on a hot row: every concurrent transfer conflicts on the balance, and an aborted transfer reruns all three statements.

## TPC-C and users

TPC-C runs NewOrder and Payment in TPC-C's 45:43 ratio, with 10 districts per warehouse, 100 customers per district and 5,000 items; the 1 ms think time pauses between statements. Users draws 1,000 rows by a Zipf distribution with skew θ: a share `disjoint` of transactions updates `email`, and the rest change `wallet` by a delta or read it `FOR UPDATE` and write it back. Commits/s, 32 clients, no per-commit fsync.

| Workload | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta | cell+delta without waits |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 2,660 | 3,458 | 2,244 (6.04) | 3,641 (5.13) | **11,130** (0.02) | 3,796 (1.34) |
| TPC-C, 4 warehouses | 6,988 | 9,660 | 3,916 (2.07) | 7,368 (1.04) | **10,584** (0.01) | 7,594 (0.33) |
| TPC-C, 1 warehouse, 1 ms think | 116 | 139 | 80 (6.58) | 208 (9.23) | **384** (0.02) | 306 (1.37) |
| users θ=0.5, 50% disjoint | 207,466 | 207,880 | 237,317 | 244,128 | **254,024** | |
| users θ=0.9, 50% disjoint | 160,257 | 158,075 | 147,826 (0.09) | 191,966 (0.05) | **228,514** (0.02) | |
| users θ=0.99, 0% disjoint | 117,562 | 117,523 | 93,071 (0.10) | 92,230 (0.10) | **141,130** | |
| users θ=0.99, 50% disjoint | 150,156 | 149,326 | 116,022 (0.11) | 163,806 (0.06) | **214,710** (0.03) | |
| users θ=0.99, 90% disjoint | 181,773 | 183,914 | 174,491 (0.10) | 268,537 (0.10) | **276,851** (0.09) | |

On TPC-C at 1 warehouse, `cell+delta` commits 4.2× InnoDB SERIALIZABLE and 3.2× REPEATABLE READ, with p99 at 22 ms against 38 ms. Cell tracking alone gains 62% over row tracking. Deltas give the rest: Payment's `W_YTD` and `D_YTD` increments commute, and the only hot read-modify-write left, NewOrder's `D_NEXT_O_ID`, queues behind its FOR UPDATE lock. Without those waits it reruns after a commit-time conflict instead, and throughput falls to 3,796. In users, skew without disjoint columns (θ=0.99, 0%) defeats `row` and `cell`, while deltas keep `cell+delta` 20% ahead of InnoDB.

### Prepared statements

| Workload | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | cell+delta |
|---|---|---|---|
| TPC-C, 1 warehouse | 3,044 | 4,155 | **12,766** |
| TPC-C, 4 warehouses | 8,035 | 11,016 | **13,978** |
| TPC-C, 1 warehouse, 1 ms think | 114 | 142 | **385** |
| users θ=0.5, 50% disjoint | 227,615 | 220,304 | **279,100** |
| users θ=0.9, 50% disjoint | 167,495 | 168,964 | **251,537** |
| users θ=0.99, 0% disjoint | 124,954 | 124,948 | **151,808** |
| users θ=0.99, 50% disjoint | 158,482 | 155,961 | **235,339** |
| users θ=0.99, 90% disjoint | 190,179 | 189,700 | **303,689** |
| ledger, 10 hot | 68,425 | 68,509 | **108,992** |
| ledger, uniform | 90,226 | 90,277 | **110,674** |

Outside the think-time runs, prepared statements skip parsing, gaining InnoDB 5–20% and `cell+delta` 8–32%, so the ratios hold or grow.

### fsync per commit

| Workload | Clients | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 32 | 2,248 | 3,178 | 2,011 (5.90) | 4,982 (2.10) | **6,437** (0.01) |
| TPC-C, 4 warehouses | 32 | 4,758 | 5,628 | 4,196 (1.49) | 6,663 (0.41) | **6,736** |
| TPC-C, 1 warehouse, 1 ms think | 32 | 109 | 135 | 80 (6.61) | 189 (9.37) | **310** (0.02) |
| users, every θ and share | 32 | 7,396–7,728 | 7,594–7,761 | 11,197–11,749 | 11,154–11,771 | **11,496–11,767** |
| TPC-C, 1 warehouse | 128 | 2,627 | 3,038 | | | **9,159** (0.02) |
| TPC-C, 4 warehouses | 128 | 6,483 | 7,431 | | | **11,216** (0.02) |
| TPC-C, 1 warehouse, 1 ms think | 128 | 142 | 158 | | | **384** (0.15) |
| users, every θ and share | 128 | 26,525–28,593 | 26,330–28,605 | | | **33,011–36,344** |

At 32 clients InnoDB settles near 7,500 commits/s on users and the ledger, hot account or not: the rate its group commit reaches on this drive. `celltnx` reaches 11,500–11,800. The store's writer syncs lionturtle once per batch, and after each sync waits up to `celltnx_group_wait_us` (500 µs) for the clients it just acknowledged to commit again, so each batch carries nearly every client.

## Where the server's time goes

`perf record` of `mysqld` during `cell+delta` runs without fsync:

| Workload | SQL layer (`mysqld`) | Kernel | libc | `ha_celltnx.so` | Commit-lock hold per commit |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse, 32 clients | 50% | 26% | 9% | 7.5% | 11.6 µs |
| ledger, 1 hot, 128 clients | 41% | 32% | 10% | 8.5% | 4.4 µs |

MySQL's parser, executor and protocol, and the kernel's network and scheduler paths, take most of the CPU; the engine takes under a tenth. At 32 clients TPC-C holds the commit lock 13% of the time, with a mean wait of 2 µs, and its latency comes from statements. In that run, 142,000 commits made 62,000 FOR UPDATE waits and 64,000 snapshot refreshes; 16 refreshes and 1% of commits conflicted.

The ledger exposed one engine bottleneck, now fixed. Each commit walked a written row's version chain from its head down to the oldest live snapshot to trim it, and validation walked every version newer than the transaction's snapshot. On a hot row at 128 clients, those walks spanned hundreds of versions under the commit lock. The longer the lock was held, the older the oldest snapshot grew, so the walks lengthened further. A chain now trims only once its installs since the last trim reach the versions that trim kept, and each version records where its run of same-mask versions began, so validation checks a run of deltas once. On the 1-hot ledger at 128 clients, the mean hold fell from 13.1 to 4.4 µs. Transfers/s, with the pre-fix runs in `bench/results/old/`:

| Clients | 1 hot before | 1 hot after |
|---|---|---|
| 32 | 103,568 | 103,524 |
| 128 | 65,936 | 124,274 |
| 256 | 43,702 | 104,574 |

## What the numbers say

- **Contention is where `celltnx` wins most.** A hot account serializes InnoDB, and the gap grows with clients: 3.3× at 8, 15× at 256. Deltas let `cell+delta` treat a hot account like any other.
- **Long transactions widen the gap.** InnoDB holds a row lock across every pause inside the transaction, and a delta holds nothing: 14.5× with 1 ms pauses and no fsync, 34× with fsync at 128 clients.
- **Every part of the design pays on TPC-C.** Cell tracking, deltas and FOR UPDATE queueing together give 4.2× InnoDB SERIALIZABLE at 1 warehouse, and still 1.5× at 4 warehouses, where contention is lower.
- **Optimism needs deltas under skew.** `row` and `cell` trail InnoDB on a hot account and on users at θ=0.99 with no disjoint columns; `cell+delta` leads in both.
- **Durability costs `celltnx` less.** Under fsync per commit, `celltnx` commits 1.5× InnoDB at 32 clients on users and 2.9× on TPC-C at 1 warehouse.
- **The server is now the ceiling.** The engine uses under 9% of CPU, and the commit lock is busy 13% of the time on TPC-C and about half the time on the 128-client ledger, so further gains must come from MySQL's statement path: prepared statements or batching.

## Reproducing

```
bench/build-mysql.sh release celltnx
bench/compare-mysql.sh bench/results/nosync.csv 8s 3 0
EXTRA=--celltnx-lock-wait-ms=0 LABEL=-nowait ONLY=celltnx MODES=cell+delta WORKLOADS="tpcc1 tpcc4 tpcc1-think" bench/compare-mysql.sh bench/results/nosync.csv 8s 3 0
PREPARE=1 MODES=cell+delta bench/compare-mysql.sh bench/results/nosync.csv 8s 3 0
for n in 8 32 128 256; do CLIENTS=$n MODES=cell+delta WORKLOADS="ledger-hot1 ledger-hot0" bench/compare-mysql.sh bench/results/ledger-clients.csv 8s 3 0; done
bench/compare-mysql.sh bench/results/durable.csv 8s 3 1
CLIENTS=128 MODES=cell+delta bench/compare-mysql.sh bench/results/durable.csv 8s 3 1
bench/median.sh bench/results/nosync.csv
cpp/ci.sh
```

The CSVs and logs from these runs are in `bench/results/`.
