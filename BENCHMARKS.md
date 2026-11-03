# Benchmarks

## Summary

Every commit syncs to disk before it is acknowledged. InnoDB runs at SERIALIZABLE, and `celltnx` in its cell+delta mode, storing to lionturtle. EC2 i7i.12xlarge with instance-store NVMe, 128 clients; transactions per second.

| Workload | InnoDB SERIALIZABLE | celltnx cell+delta | Speedup |
|---|---|---|---|
| Hot account, 1 ms pause between statements | 349 | 32,071 | **92×** |
| Hot account | 1,859 | 76,395 | **41×** |
| TPC-C, 1 warehouse | 863 | 6,439 | **7.5×** |
| Users, heavy skew (θ=0.99, 50% disjoint) | 49,218 | 115,498 | **2.3×** |
| TPC-C, 4 warehouses | 3,981 | 9,255 | **2.3×** |
| 10 hot accounts | 39,906 | 77,043 | **1.9×** |
| Uniform transfers | 72,250 | 77,422 | **1.1×** |
| Users, mild skew (θ=0.5) | 122,287 | 130,639 | **1.1×** |

- **Contention sets the gap.** InnoDB holds a hot row's lock from the UPDATE to commit, so transfers through one account run one at a time: about 1,100–1,900 a second on this machine at every client count, with or without fsync. Deltas commute, so `cell+delta` runs a hot account as fast as uniform transfers, with no retries.
- **Without contention, the storage engine is not the limit.** The engines run within 8% of each other from 32 to 128 clients. In a profile of users at θ=0.5 and 128 clients, `celltnx` and lionturtle take 5% of the busy CPU. The kernel's loopback networking and thread wake-ups take 39%, MySQL's parser, executor and protocol 35%, and the benchmark client 21%, and half the machine sits idle: the round trips between client and server set the pace. `celltnx` commits 134,000 a second with `mysqld` on 13.7 cores. InnoDB commits 123,000 on 32.8 cores, 43% of its samples in its redo log and latches. At 256 clients InnoDB's uniform rate falls, and `celltnx` leads 1.5×.
- **At low concurrency, InnoDB commits sooner.** At 8 clients, uniform transfers run 14,827 a second on InnoDB SERIALIZABLE, 21,487 on REPEATABLE READ and 9,799 on `celltnx`: InnoDB's log threads spin through each sync, while each `celltnx` commit sleeps about 170 µs while the store's writer wakes, syncs and wakes it.
- **The commit lock caps the ledger.** From 128 clients, ledger commits queue on `celltnx`'s global commit lock: without fsync, transfers top out near 85,000 a second, hot account or not.

## Setup

An EC2 i7i.12xlarge: an Intel Xeon Platinum 8559C (24 cores, 48 threads, one NUMA node), 384 GiB of RAM, and one 3.75 TB instance-store NVMe drive formatted ext4, on Ubuntu 26.04 with Linux 7.0. A 4 KiB `fdatasync` there takes 18 µs at the median and 20 µs on average (`bench/results/i7i/fsync-latency.txt`).

MySQL 8.4.13 is built from source (RelWithDebInfo, GCC 15.2) by `bench/build-mysql.sh`, with `ha_celltnx.so` loaded beside InnoDB in the same `mysqld`. `celltnx` stores to lionturtle v0.5.3 with its default LZ4 compression. `bench/compare-mysql.sh` starts a fresh server for every configuration with identical flags and drives it with `bench/sqlbench` over TCP from the same machine. Durable runs set `innodb_flush_log_at_trx_commit=1` and `celltnx_sync=ON`; the others set 0 and OFF, though both engines still write their logs. Binary logging is off. Each point is the median of three 8-second runs.

InnoDB runs at SERIALIZABLE and REPEATABLE READ. `celltnx` runs in its three modes: `row` tracks whole rows, `cell` tracks (row, column) cells, and `cell+delta` also lets increments commute. Every mode queues `SELECT … FOR UPDATE` behind advisory row locks ([DESIGN.md](DESIGN.md#select--for-update)); the `-nowait` column turns that off. Retries per transaction appear in parentheses where they are nonzero; InnoDB retries fewer than 0.1 per transaction everywhere. `sqlbench` retries a deadlock after a random 0–1 ms backoff.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | core, key encodings, checker, workloads, threaded stress, row locks, lionturtle store | pass in debug, ASan and TSan builds |
| Simulation sweeps | 30,000 seeds each for random, users and TPC-C, and 30,000 over every workload with a drawn snapshot-refresh rate | 0 failures; 12,000 determinism reruns, 0 mismatches |
| Self-test | 15 deliberate bugs, including a refresh that skips validation | all found, by seed 594 at the latest |
| Store crash test | 8 rounds of `kill -9` during TPC-C on one lionturtle store, half with a 500 µs commit delay | no acknowledged commit lost; recovered state equals the serial replay |
| lionturtle check | `store/lionturtle/check/run.sh`: API edge cases, engine-shaped load with and without syncs, 21 `kill -9` rounds, 3 injected sync failures | every acknowledged commit recovered, checked row by row |
| MTR suite | cells, commit_conflict, deltas, durable (two restarts), for_update, status | 6 of 6 pass |
| Benchmark audits | every TPC-C and ledger run below: TPC-C consistency conditions 1–4; every balance equals its opening plus the ledger's credits minus its debits, and the ledger holds exactly the committed transfers | 178 TPC-C and 316 ledger audits consistent |

TSan runs suppress reports inside lionturtle's Rust code, which TSan does not instrument. Built with TSan instrumentation, lionturtle's own concurrency tests report only the deliberate optimistic reads in its memtable's B-tree, which validate after reading. [PLAN.md](PLAN.md) tracks three checks still to build: a wire-protocol oracle, a `mysqld`-level kill test, and MySQL's own suites run against `celltnx`.

## Hot accounts

The ledger workload moves money between 1,000 accounts. `accounts` holds `(id, balance, opening)` with `CHECK (balance >= 0)`; each transfer moves 1–100 between a hot account and a cold one, in either direction, in one transaction:

```sql
UPDATE accounts SET balance = balance - ? WHERE id = ?;  -- payer
UPDATE accounts SET balance = balance + ? WHERE id = ?;  -- payee
INSERT INTO entries VALUES (?, ?, ?, ?);
```

`hot=1` routes every transfer through one account, `hot=10` through one of ten, and `uniform` picks both accounts at random. Hot accounts open with 10⁹ and cold ones with 1,000, so a cold account occasionally runs dry: that transfer fails the CHECK and rolls back, as on InnoDB. In `cell+delta`, both UPDATEs are deltas, and the engine rechecks the CHECK on each merged balance at commit.

### fsync per commit

Transfers/s.

| Shape | Clients | InnoDB SERIALIZABLE | row | cell | cell+delta | cell+delta ÷ InnoDB |
|---|---|---|---|---|---|---|
| 1 hot | 32 | 1,417 | 4,624 (6.30) | 4,567 (6.35) | **56,464** | 40× |
| 10 hot | 32 | 41,233 | 19,911 (0.98) | 20,441 (0.97) | **55,430** | 1.3× |
| uniform | 32 | 52,855 | 51,244 (0.05) | 51,129 (0.05) | **56,230** | 1.1× |
| 1 hot, 1 ms pauses | 32 | 338 | 354 (17.6) | 355 (17.6) | **7,185** | 21× |
| 1 hot | 128 | 1,859 | | | **76,395** | 41× |
| 10 hot | 128 | 39,906 | | | **77,043** | 1.9× |
| uniform | 128 | 72,250 | | | **77,422** | 1.1× |
| 1 hot, 1 ms pauses | 128 | 349 | | | **32,071** | 92× |

At 128 clients and 1 hot, p99 is 147 ms for InnoDB and 5.2 ms for `cell+delta`; with pauses, 766 ms against 6.6 ms. REPEATABLE READ is within 3% of SERIALIZABLE on every ledger row.

### Clients

1 hot account against uniform transfers; transfers/s. InnoDB is SERIALIZABLE.

| Clients | InnoDB 1 hot | InnoDB uniform | cell+delta 1 hot | cell+delta uniform | 1 hot: cell+delta ÷ InnoDB |
|---|---|---|---|---|---|
| fsync, 8 | 1,419 | 14,827 | 9,343 | 9,799 | 6.6× |
| fsync, 32 | 1,417 | 52,855 | 56,464 | 56,230 | 40× |
| fsync, 128 | 1,859 | 72,250 | 76,395 | 77,422 | 41× |
| fsync, 256 | 1,706 | 54,771 | 79,375 | 79,703 | 47× |
| no fsync, 8 | 1,091 | 30,756 | 35,483 | 35,332 | 33× |
| no fsync, 32 | 1,213 | 64,562 | 68,875 | 69,154 | 57× |
| no fsync, 128 | 1,825 | 88,401 | 84,144 | 85,220 | 46× |
| no fsync, 256 | 1,641 | 55,686 | 85,053 | 85,391 | 52× |

InnoDB's three 8-client uniform runs with fsync spread from 10,601 to 22,506; REPEATABLE READ gives 21,487 there.

### No per-commit fsync, 32 clients

Transfers/s.

| Shape | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta | cell+delta ÷ SERIALIZABLE |
|---|---|---|---|---|---|---|
| 1 hot | 1,202 | 1,215 | 4,625 (6.25) | 4,504 (6.29) | **67,221** | 56× |
| 10 hot | 33,307 | 32,920 | 18,152 (1.06) | 18,611 (1.06) | **67,769** | 2.0× |
| uniform | 64,384 | 63,057 | 60,717 (0.07) | 60,052 (0.07) | **68,754** | 1.1× |
| 1 hot, 1 ms pauses | 357 | 363 | 354 (17.6) | 356 (17.5) | **6,814** | 19× |
| 1 hot, prepared statements | 1,369 | 1,362 | | | **72,605** | 53× |

A hot account costs `cell+delta` nothing: at every client count and both fsync settings, 1 hot runs within 5% of uniform, and within 2% from 32 clients up. InnoDB holds the hot row's lock from its UPDATE until commit, so transfers through one account run one at a time, and pauses inside that window divide throughput directly. Both engines make a commit visible and release its locks before syncing it, so fsync leaves InnoDB's hot row alone: 1,202 transfers/s without it and 1,417 with it at 32 clients.

Without deltas, every concurrent transfer conflicts on the balance: where InnoDB makes a transfer wait its turn, `celltnx` in `row` or `cell` mode aborts it at commit, and the transfer sleeps up to 1 ms and reruns all three statements, about six times per transfer at 32 clients. That still beats InnoDB's hot row here, 3.8× at 32 clients without fsync, but trails `cell+delta` by 15×.

## TPC-C and users

TPC-C runs NewOrder and Payment in TPC-C's 45:43 ratio, with 10 districts per warehouse, 100 customers per district and 5,000 items; the 1 ms think time pauses between statements. Users draws 1,000 rows by a Zipf distribution with skew θ: a share `disjoint` of transactions updates `email`, and the rest change `wallet` by a delta or read it `FOR UPDATE` and write it back.

### fsync per commit

Commits/s.

| Workload | Clients | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 32 | 609 | 987 | 1,073 (5.78) | 1,648 (4.94) | **3,876** (0.02) |
| TPC-C, 4 warehouses | 32 | 2,264 | 4,263 | 1,686 (2.11) | 3,682 (1.01) | **5,184** (0.01) |
| TPC-C, 1 warehouse, 1 ms think | 32 | 89 | 105 | 61 (6.98) | 195 (9.06) | **267** (0.04) |
| users θ=0.5, 50% disjoint | 32 | 112,747 | 113,894 | 109,017 (0.01) | 110,930 (0.01) | 112,082 |
| users θ=0.9, 50% disjoint | 32 | 107,013 | 108,238 | 56,306 (0.08) | 93,991 (0.05) | 106,458 (0.02) |
| users θ=0.99, 0% disjoint | 32 | 34,299 | 34,530 | 27,103 (0.08) | 27,243 (0.09) | **72,686** |
| users θ=0.99, 50% disjoint | 32 | 63,028 | 63,120 | 36,172 (0.10) | 68,064 (0.07) | **99,883** (0.03) |
| users θ=0.99, 90% disjoint | 32 | 108,608 | 108,101 | 72,836 (0.12) | 97,025 (0.09) | 95,504 (0.09) |
| TPC-C, 1 warehouse | 128 | 863 | 1,070 | | | **6,439** (0.02) |
| TPC-C, 4 warehouses | 128 | 3,981 | 6,262 | | | **9,255** (0.02) |
| TPC-C, 1 warehouse, 1 ms think | 128 | 118 | 127 | | | **330** (0.13) |
| users θ=0.5, 50% disjoint | 128 | 122,287 | 122,584 | | | **130,639** (0.03) |
| users θ=0.9, 50% disjoint | 128 | 82,361 | 82,002 | | | **121,369** (0.06) |
| users θ=0.99, 0% disjoint | 128 | 33,544 | 33,637 | | | **75,484** |
| users θ=0.99, 50% disjoint | 128 | 49,218 | 47,513 | | | **115,498** (0.07) |
| users θ=0.99, 90% disjoint | 128 | 71,796 | 70,473 | | | **125,482** (0.41) |

InnoDB leads by more than 1% on one row, users at θ=0.99 with 90% disjoint at 32 clients, by 14%; without fsync, `cell+delta` runs that row at 3.1× InnoDB.

### No per-commit fsync, 32 clients

Commits/s.

| Workload | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta | cell+delta without waits |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 579 | 915 | 1,124 (5.49) | 1,677 (4.84) | **4,104** (0.02) | 1,985 (1.31) |
| TPC-C, 4 warehouses | 2,012 | 3,954 | 1,719 (2.09) | 3,749 (1.02) | **5,263** (0.01) | 4,726 (0.32) |
| TPC-C, 1 warehouse, 1 ms think | 90 | 109 | 64 (6.14) | 190 (8.64) | **283** (0.02) | 228 (1.44) |
| users θ=0.5, 50% disjoint | 181,776 | 183,793 | 172,628 (0.03) | 180,712 (0.01) | 183,732 (0.01) | |
| users θ=0.9, 50% disjoint | 95,062 | 93,713 | 53,324 (0.05) | 93,670 (0.03) | **177,475** (0.03) | |
| users θ=0.99, 0% disjoint | 25,416 | 25,213 | 28,593 (0.07) | 29,129 (0.07) | **87,612** | |
| users θ=0.99, 50% disjoint | 37,945 | 40,153 | 38,713 (0.06) | 60,659 (0.04) | **163,607** (0.03) | |
| users θ=0.99, 90% disjoint | 69,756 | 67,964 | 66,771 (0.05) | 214,609 (0.12) | **219,012** (0.11) | |

On TPC-C at 1 warehouse, `cell+delta` commits 7.1× InnoDB SERIALIZABLE and 4.5× REPEATABLE READ without fsync, and 6.4× and 3.9× with it. Cell tracking alone gains 49% over row tracking. Deltas give the rest: Payment's `W_YTD` and `D_YTD` increments commute, and the only hot read-modify-write left, NewOrder's `D_NEXT_O_ID`, queues behind its FOR UPDATE lock. Without those waits it reruns after a commit-time conflict instead, and throughput falls to 1,985. In users, skew without disjoint columns (θ=0.99, 0%) defeats `row` and `cell`, while deltas keep `cell+delta` 3.4× ahead of InnoDB without fsync.

### Prepared statements

No per-commit fsync, 32 clients; commits/s.

| Workload | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | cell+delta |
|---|---|---|---|
| TPC-C, 1 warehouse | 663 | 1,030 | **5,045** |
| TPC-C, 4 warehouses | 2,215 | 4,660 | **6,485** |
| TPC-C, 1 warehouse, 1 ms think | 94 | 108 | **288** |
| users θ=0.5, 50% disjoint | 185,548 | 185,952 | 187,050 |
| users θ=0.9, 50% disjoint | 95,877 | 94,821 | **183,424** |
| users θ=0.99, 0% disjoint | 25,759 | 26,072 | **94,481** |
| users θ=0.99, 50% disjoint | 40,057 | 39,561 | **173,439** |
| users θ=0.99, 90% disjoint | 67,097 | 66,658 | **226,522** |
| ledger, 1 hot | 1,369 | 1,362 | **72,605** |
| ledger, 10 hot | 38,217 | 38,464 | **70,926** |
| ledger, uniform | 67,137 | 67,109 | **71,350** |

Prepared statements skip parsing in both engines, so the ratios hold: 7.6× on TPC-C at 1 warehouse and 53× on the hot account.

## Where the time goes

Profiles with fsync per commit, in `bench/results/i7i/profile/`: `top` for CPU per process and per thread, `perf record -a` for samples by binary and symbol, `perf trace -s` for system calls per thread, and `perf sched` for wake-up delays.

### 128 clients, no contention

Users at θ=0.5, one 30-second run per engine.

| | Commits/s | p50 | `mysqld` CPU | `sqlbench` CPU | Machine idle |
|---|---|---|---|---|---|
| `celltnx` cell+delta | 133,766 | 854 µs | 13.7 cores | 8.9 cores | 53% |
| InnoDB SERIALIZABLE | 123,172 | 619 µs | 32.8 cores | 6.3 cores | 17% |

InnoDB's connection threads spend 43% of all samples in its redo log and latches: `log_write_up_to` 20.0%, `rw_lock_s_lock_spin` 8.2%, `log_sync_point` 7.2%, `rw_lock_x_lock_func` 5.4% and a mutex's `enter` 2.5%. Of the busy CPU in the `celltnx` run, with idle time excluded:

| Where | Share |
|---|---|
| Kernel: loopback networking and thread wake-ups, for client and server | 39% |
| MySQL's parser, executor and protocol | 35% |
| Benchmark client (`sqlbench`), much of it goroutine scheduling | 21% |
| `celltnx` and lionturtle | 5% |

Neither engine runs out of CPU. Each transaction makes several round trips between the client and MySQL over loopback TCP, each waking a client goroutine and a server thread, and those round trips cap both engines. The client shares the machine with the server; a separate client machine was not measured. `celltnx`'s store writer runs at 64% of a core, and its p50 is 38% above InnoDB's.

### 8 clients, uniform transfers

| | Transfers/s | p50 | Blocking waits per connection, 5 s |
|---|---|---|---|
| `celltnx` cell+delta | 9,607 | 557 µs | 18,086 `futex` calls, 1,017 ms |
| InnoDB SERIALIZABLE | 15,884 | 264 µs | 604 `futex` calls, 49 ms |

InnoDB's log writer thread never sleeps: in a 3-second sample it ran for 2,965 ms and switched out 1,826 times, and its flush thread called `fdatasync` 19,000 times a second. Its connection threads spin until their commit is durable. `celltnx`'s writer wrote 4,800 times a second, one direct `pwritev2` of two commits, 31 µs each, and spent two-thirds of its time waiting for work. Each commit wakes the writer and is woken in turn, so a connection sleeps about 170 µs per commit.

### Engine profile

`perf record` of `mysqld` and the engine's counters over 10 seconds of `cell+delta` runs without fsync:

| Workload | SQL layer (`mysqld`) | Kernel | libc | `ha_celltnx.so` | Commit-lock hold per commit | Commit-lock wait per commit |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse, 32 clients | 55% | 24% | 9% | 7.2% | 17 µs | 4 µs |
| ledger, 1 hot, 128 clients | 45% | 30% | 12% | 9.6% | 7.5 µs | 1.2 ms |

`ha_celltnx.so` includes lionturtle, which links into it. TPC-C holds the commit lock about 6% of the time, and its latency comes from statements: in those 10 seconds, 35,800 commits made 15,700 FOR UPDATE waits and 16,800 snapshot refreshes; 11 refreshes and 1.7% of commits conflicted.

The ledger at 128 clients queues on the commit lock. Each commit holds it 7.5 µs and waits 1.2 ms for it, most of a transfer's 1.4 ms p50, so about 100 of the 128 connections wait on the lock at any moment. Hand-offs between waiters stretch each commit's turn to about 12 µs. The ledger tops out near that rate, 85,000 transfers a second without fsync, with one hot account or none, at 128 and 256 clients alike.

### Group wait

After each sync, the store's writer may wait for the clients it just acknowledged to commit again, so they share the next sync. The wait lasts at most `celltnx_group_wait_us` or as long as the previous flush took, whichever is less: tens of microseconds on this drive. `group-wait.csv` holds runs with the cap at 50 µs and at 0, which turns the wait off.

| 128 clients, fsync | 500 µs (default) | 50 µs | 0 |
|---|---|---|---|
| users θ=0.5 | 130,639 | 136,409 | 135,311 |
| ledger, 1 hot | 76,395 | 78,518 | 78,327 |
| ledger, uniform | 77,422 | 79,676 | 78,649 |
| TPC-C, 1 warehouse | 6,439 | 6,402 | 6,397 |

Each column comes from a separate server run, and the three stay within 5% of each other.

## What the numbers say

- **Contention is where `celltnx` wins.** A hot account serializes InnoDB at 1,100–1,900 transfers/s whatever the clients or fsync; `cell+delta` runs it at 56,000–85,000 from 32 clients up, 40–57× InnoDB.
- **Long transactions widen the gap.** InnoDB holds a row lock across every pause inside the transaction, and a delta holds nothing: 92× with 1 ms pauses and fsync at 128 clients.
- **Every part of the design pays on TPC-C.** Cell tracking, deltas and FOR UPDATE queueing together give 6.4–7.6× InnoDB SERIALIZABLE at 1 warehouse, and still 2.3–2.9× at 4 warehouses.
- **Optimism needs deltas under skew.** `row` and `cell` retry about six times per transfer on a hot account; `cell+delta` retries none.
- **Hot rows queue cheaply.** Users at θ=0.99 and 128 clients, where FOR UPDATE readers and deltas meet on a few hot rows, run at 1.7–2.3× InnoDB.
- **Without contention, everything around the engine sets the pace.** The storage engine takes 5% of the busy CPU; MySQL, the kernel's networking and the client take the rest. From 32 to 128 clients, throughput ties within 8%, and `celltnx` gets there on less than half InnoDB's CPU; at 256 clients InnoDB's uniform rate falls and `celltnx` leads 1.5×.
- **Two costs remain in `celltnx`'s commit path.** At 8 clients each commit sleeps through two thread handoffs, so InnoDB commits 1.5–2.2× faster. From 128 clients, the global commit lock caps the ledger near 85,000 transfers a second. A committing thread could flush its own group instead of handing it to the writer, and one holder of the commit lock could commit the transactions queued behind it.

## Reproducing

On an i7i.12xlarge running Ubuntu 26.04, format one instance-store drive as ext4 at `/mnt/nvme`, install the toolchain (GCC, CMake, Ninja, Bison, OpenSSL, libaio, libtirpc, Go 1.26, Rust 1.97), clone the repository there and set `MYSQL_ROOT=/mnt/nvme/celltnx`, so the build and the data directories live on the instance store. Then:

```
bench/build-mysql.sh release
bench/compare-mysql.sh durable.csv 8s 3 1
CLIENTS=128 MODES=cell+delta bench/compare-mysql.sh durable.csv 8s 3 1
for n in 8 256; do CLIENTS=$n MODES=cell+delta WORKLOADS="ledger-hot1 ledger-hot0" bench/compare-mysql.sh durable-clients.csv 8s 3 1; done
for w in 0 50; do CLIENTS=128 ONLY=celltnx MODES=cell+delta EXTRA=--celltnx-group-wait-us=$w LABEL=-gw$w WORKLOADS="tpcc1 users0.5 ledger-hot1 ledger-hot0" bench/compare-mysql.sh group-wait.csv 8s 3 1; done
bench/compare-mysql.sh nosync.csv 8s 3 0
EXTRA=--celltnx-lock-wait-ms=0 LABEL=-nowait ONLY=celltnx MODES=cell+delta WORKLOADS="tpcc1 tpcc4 tpcc1-think" bench/compare-mysql.sh nosync.csv 8s 3 0
PREPARE=1 MODES=cell+delta bench/compare-mysql.sh nosync.csv 8s 3 0
for n in 8 32 128 256; do CLIENTS=$n MODES=cell+delta WORKLOADS="ledger-hot1 ledger-hot0" bench/compare-mysql.sh ledger-clients.csv 8s 3 0; done
bench/median.sh durable.csv
./ci.sh
```

`ONLY=celltnx` skips InnoDB. To compare plugin builds on one machine, copy each build's `plugin_output_directory` aside and run them in turn with `EXTRA=--plugin-dir=DIR LABEL=-NAME`; `row-locks/` compares the FOR UPDATE queue (`-queue`) that way with a lock that woke every waiter of a shard (`-wake-all`). The CSVs and logs are in `bench/results/i7i/`.
