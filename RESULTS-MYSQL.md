# MySQL engine results

## Summary

Every commit syncs to disk before it is acknowledged. InnoDB runs at SERIALIZABLE; lionturtle cell+delta is `celltnx` in its cell+delta mode, storing to lionturtle. EC2 i7i.12xlarge with instance-store NVMe, 128 clients; transactions per second.

| Workload | InnoDB SERIALIZABLE | lionturtle cell+delta | Speedup |
|---|---|---|---|
| Hot account, 1 ms pause between statements | 348 | 32,035 | **92×** |
| Hot account | 1,856 | 75,102 | **40×** |
| TPC-C, 1 warehouse | 842 | 6,482 | **7.7×** |
| TPC-C, 4 warehouses | 3,921 | 8,915 | **2.3×** |
| 10 hot accounts | 39,142 | 74,504 | **1.9×** |
| Users, heavy skew (θ=0.99, 50% disjoint) | 48,317 | 82,979 | **1.7×** |
| Uniform transfers | 71,528 | 76,635 | **1.1×** |
| Users, mild skew (θ=0.5) | 124,198 | 123,054 | **1.0×** |

- **Contention sets the gap.** InnoDB holds a hot row's lock from the UPDATE to commit, so transfers through one account run one at a time: about 1,100–1,900 a second on this machine at every client count, with or without fsync. Deltas commute, so `cell+delta` runs a hot account as fast as uniform transfers, with no retries.
- **Without contention, the storage engine is not the limit.** The engines tie on throughput up to 128 clients. In a profile of users at θ=0.5 and 128 clients, `celltnx` and lionturtle take 5% of the busy CPU. The kernel's loopback networking and thread wake-ups take 38%, MySQL's parser, executor and protocol 36%, and the benchmark client 20%, and half the machine sits idle: the round trips between client and server set the pace. `celltnx` commits 134,000 a second with `mysqld` on 13.5 cores. InnoDB commits 130,000 on 30.9 cores, a quarter of its samples spinning on latches and the redo log. `celltnx` commits also queue behind its single store writer. At 256 clients InnoDB's uniform rate falls, and `celltnx` leads 1.5×.
- **At low concurrency, InnoDB commits sooner.** At 8 clients, uniform transfers run 14,336 a second on InnoDB and 8,848 on `celltnx`: InnoDB's log threads spin through each sync, while each `celltnx` commit sleeps about 150 µs while the store's writer wakes, syncs and wakes it.
- **The drive matters.** A 4 KiB `fdatasync` takes 18 µs on the i7i's drive, which has power-loss protection, and 2 ms on a desktop drive without it. On the desktop the hot account gives 3.2× rather than 40×, because every commit waits on the drive (see [The drive](#the-drive)).

## Setup

The main runs used an EC2 i7i.12xlarge: an Intel Xeon Platinum 8559C (24 cores, 48 threads, one NUMA node), 384 GiB of RAM, and one 3.75 TB instance-store NVMe drive formatted ext4, on Ubuntu 26.04 with Linux 7.0. A 4 KiB `fdatasync` there takes 18 µs at the median and 27 µs on average (`bench/results/i7i/fsync-latency.txt`). The desktop runs used an Intel Core Ultra 9 285 (24 cores), 125 GB of RAM and a Micron 2 TB NVMe drive with ext4, on Ubuntu 26.04 with Linux 7.0, where the same `fdatasync` takes 2.0 ms at the median.

On both, MySQL 8.4.13 is built from source (RelWithDebInfo, GCC 15.2) by `bench/build-mysql.sh`, with `ha_celltnx.so` loaded beside InnoDB in the same `mysqld`. `bench/compare-mysql.sh` starts a fresh server for every configuration with identical flags and drives it with `cmd/sqlbench` over TCP from the same machine. Durable runs set `innodb_flush_log_at_trx_commit=1` and `celltnx_sync=ON`; the others set 0 and OFF, though both engines still write their logs. Binary logging is off. Each point is the median of three 8-second runs.

InnoDB runs at SERIALIZABLE and REPEATABLE READ. `celltnx` runs in its three modes: `row` tracks whole rows, `cell` tracks (row, column) cells, and `cell+delta` also lets increments commute. Every mode queues `SELECT … FOR UPDATE` behind advisory row locks (`ENGINE.md`); the `-nowait` column turns that off. Retries per transaction appear in parentheses where they are nonzero; InnoDB retries fewer than 0.1 per transaction everywhere. `sqlbench` retries a deadlock after a random 0–1 ms backoff.

## Correctness

| Check | Scope | Result |
|---|---|---|
| Unit and scenario tests | core, Go key compatibility, checker, workloads, threaded stress, lionturtle store | pass in debug, ASan and TSan builds |
| Golden traces | C++ and Go simulators, seeds 0–9,999: trace, steps, crashes, failure and parameters | identical |
| Simulation sweeps | 30,000 seeds each for random, users and TPC-C, and 30,000 over every workload with a drawn snapshot-refresh rate | 0 failures; 12,000 determinism reruns, 0 mismatches |
| Self-test | 15 deliberate bugs, including a refresh that skips validation | all found, by seed 594 at the latest |
| Store crash test | 8 rounds of `kill -9` during TPC-C on one lionturtle store, half with a 500 µs commit delay | no acknowledged commit lost; recovered state equals the serial replay |
| MTR suite | cells, commit_conflict, deltas, durable (two restarts), for_update, status | 6 of 6 pass |
| Benchmark audits | every TPC-C and ledger run below: TPC-C consistency conditions 1–4; every balance equals its opening plus the ledger's credits minus its debits, and the ledger holds exactly the committed transfers | i7i: 177 TPC-C and 327 ledger audits consistent; desktop: 153 and 264 |

TSan runs suppress reports inside lionturtle's Rust code, which TSan does not instrument. Built with TSan instrumentation, lionturtle's own concurrency tests report only the deliberate optimistic reads in its memtable's B-tree, which validate after reading. The wire oracle, a `mysqld`-level kill test and MySQL's own suites under `celltnx` are not yet built (`PLAN-MYSQL.md`).

## Hot accounts

The ledger workload moves money between 1,000 accounts. `accounts` holds `(id, balance, opening)` with `CHECK (balance >= 0)`; each transfer moves 1–100 between a hot account and a cold one, in either direction, in one transaction:

```sql
UPDATE accounts SET balance = balance - ? WHERE id = ?;  -- payer
UPDATE accounts SET balance = balance + ? WHERE id = ?;  -- payee
INSERT INTO entries VALUES (?, ?, ?, ?);
```

`hot=1` routes every transfer through one account, `hot=10` through one of ten, and `uniform` picks both accounts at random. Hot accounts open with 10⁹ and cold ones with 1,000, so a cold account occasionally runs dry: that transfer fails the CHECK and rolls back, as on InnoDB. In `cell+delta`, both UPDATEs are deltas, and the engine rechecks the CHECK on each merged balance at commit.

### fsync per commit

i7i; transfers/s.

| Shape | Clients | InnoDB SERIALIZABLE | row | cell | cell+delta | cell+delta ÷ InnoDB |
|---|---|---|---|---|---|---|
| 1 hot | 32 | 1,446 | 4,393 (6.35) | 4,430 (6.35) | **55,737** | 39× |
| 10 hot | 32 | 41,072 | 19,543 (0.98) | 20,070 (0.97) | **56,550** | 1.4× |
| uniform | 32 | 51,799 | 51,664 (0.05) | 49,752 (0.05) | **56,798** | 1.1× |
| 1 hot, 1 ms pauses | 32 | 339 | 355 (17.6) | 353 (17.6) | **7,185** | 21× |
| 1 hot | 128 | 1,856 | | | **75,102** | 40× |
| 10 hot | 128 | 39,142 | | | **74,504** | 1.9× |
| uniform | 128 | 71,528 | | | **76,635** | 1.1× |
| 1 hot, 1 ms pauses | 128 | 348 | | | **32,035** | 92× |

At 128 clients and 1 hot, p99 is 146 ms for InnoDB and 5.6 ms for `cell+delta`; with pauses, 770 ms against 6.5 ms. REPEATABLE READ is within 11% of SERIALIZABLE on every ledger row.

### Clients

1 hot account against uniform transfers on the i7i; transfers/s. InnoDB is SERIALIZABLE.

| Clients | InnoDB 1 hot | InnoDB uniform | cell+delta 1 hot | cell+delta uniform | 1 hot: cell+delta ÷ InnoDB |
|---|---|---|---|---|---|
| fsync, 8 | 1,294 | 14,336 | 9,964 | 8,848 | 7.7× |
| fsync, 32 | 1,446 | 51,799 | 55,737 | 56,798 | 39× |
| fsync, 128 | 1,856 | 71,528 | 75,102 | 76,635 | 40× |
| fsync, 256 | 1,613 | 54,443 | 78,868 | 79,143 | 49× |
| no fsync, 8 | 1,092 | 30,539 | 35,707 | 34,596 | 33× |
| no fsync, 32 | 1,242 | 62,650 | 70,339 | 66,699 | 57× |
| no fsync, 128 | 1,845 | 87,964 | 84,764 | 84,973 | 46× |
| no fsync, 256 | 1,631 | 55,838 | 85,261 | 86,079 | 52× |

InnoDB's 8-client uniform runs with fsync spread widely (58%); REPEATABLE READ gives 20,729 there.

### No per-commit fsync, 32 clients

i7i; transfers/s.

| Shape | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta | cell+delta ÷ SERIALIZABLE |
|---|---|---|---|---|---|---|
| 1 hot | 1,306 | 1,243 | 4,531 (6.30) | 4,638 (6.22) | **66,723** | 51× |
| 10 hot | 32,846 | 32,316 | 17,948 (1.06) | 19,061 (1.05) | **69,899** | 2.1× |
| uniform | 62,392 | 64,951 | 58,497 (0.07) | 60,389 (0.07) | **69,894** | 1.1× |
| 1 hot, 1 ms pauses | 351 | 355 | 354 (17.6) | 354 (17.5) | **6,818** | 19× |
| 1 hot, prepared statements | 1,325 | 1,292 | | | **74,280** | 56× |

A hot account costs `cell+delta` nothing: at every client count and both fsync settings, 1 hot runs within 13% of uniform, and within 6% from 32 clients up. InnoDB holds the hot row's lock from its UPDATE until commit, so transfers through one account run one at a time, and pauses inside that window divide throughput directly. Both engines make a commit visible and release its locks before syncing it, so fsync leaves InnoDB's hot row alone: 1,306 transfers/s without it and 1,446 with it at 32 clients. On this Xeon, InnoDB's hot account runs about 8× slower than on the desktop (1,306 against 10,738 without fsync at 32 clients); the profiles below do not cover that case.

Without deltas, every concurrent transfer conflicts on the balance: where InnoDB makes a transfer wait its turn, `celltnx` in `row` or `cell` mode aborts it at commit, and the transfer sleeps up to 1 ms and reruns all three statements, about six times per transfer at 32 clients. That still beats InnoDB's hot row here, 3.5× at 32 clients without fsync, but trails `cell+delta` by 15×.

## TPC-C and users

TPC-C runs NewOrder and Payment in TPC-C's 45:43 ratio, with 10 districts per warehouse, 100 customers per district and 5,000 items; the 1 ms think time pauses between statements. Users draws 1,000 rows by a Zipf distribution with skew θ: a share `disjoint` of transactions updates `email`, and the rest change `wallet` by a delta or read it `FOR UPDATE` and write it back.

### fsync per commit

i7i; commits/s.

| Workload | Clients | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 32 | 605 | 947 | 1,061 (5.61) | 1,657 (4.73) | **3,866** (0.02) |
| TPC-C, 4 warehouses | 32 | 2,236 | 4,164 | 1,672 (2.07) | 3,589 (0.99) | **5,007** (0.01) |
| TPC-C, 1 warehouse, 1 ms think | 32 | 86 | 108 | 60 (6.85) | 195 (8.70) | **274** (0.03) |
| users θ=0.5, 50% disjoint | 32 | 113,537 | 113,337 | 107,982 (0.01) | 110,594 (0.01) | 111,698 |
| users θ=0.9, 50% disjoint | 32 | 106,699 | 107,772 | 61,129 (0.09) | 94,729 (0.05) | 105,674 (0.02) |
| users θ=0.99, 0% disjoint | 32 | 33,864 | 34,018 | 33,474 (0.09) | 33,021 (0.09) | **76,252** |
| users θ=0.99, 50% disjoint | 32 | 58,317 | 58,836 | 42,999 (0.10) | 72,364 (0.07) | **101,355** (0.03) |
| users θ=0.99, 90% disjoint | 32 | 108,052 | 104,947 | 74,258 (0.13) | 95,922 (0.09) | 95,194 (0.09) |
| TPC-C, 1 warehouse | 128 | 842 | 1,058 | | | **6,482** (0.02) |
| TPC-C, 4 warehouses | 128 | 3,921 | 6,146 | | | **8,915** (0.02) |
| TPC-C, 1 warehouse, 1 ms think | 128 | 118 | 125 | | | **334** (0.19) |
| users θ=0.5, 50% disjoint | 128 | 124,198 | 120,986 | | | 123,054 (0.03) |
| users θ=0.9, 50% disjoint | 128 | 79,737 | 80,476 | | | **107,015** (0.04) |
| users θ=0.99, 0% disjoint | 128 | 32,480 | 32,896 | | | 28,922 |
| users θ=0.99, 50% disjoint | 128 | 48,317 | 46,402 | | | **82,979** (0.03) |
| users θ=0.99, 90% disjoint | 128 | 70,370 | 68,349 | | | **119,766** (0.43) |

Users at θ=0.99 with no disjoint columns is the noisiest row: its three `cell+delta` runs at 128 clients spread 22%.

### No per-commit fsync, 32 clients

i7i; commits/s.

| Workload | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | row | cell | cell+delta | cell+delta without waits |
|---|---|---|---|---|---|---|
| TPC-C, 1 warehouse | 563 | 929 | 1,106 (5.36) | 1,701 (4.62) | **4,178** (0.02) | 1,810 (1.30) |
| TPC-C, 4 warehouses | 2,044 | 3,849 | 1,671 (2.07) | 3,558 (1.02) | **4,984** (0.01) | 4,318 (0.32) |
| TPC-C, 1 warehouse, 1 ms think | 89 | 108 | 62 (6.92) | 192 (8.99) | **272** (0.03) | 224 (1.45) |
| users θ=0.5, 50% disjoint | 186,772 | 185,466 | 175,332 (0.03) | 174,206 (0.01) | 178,826 (0.01) | |
| users θ=0.9, 50% disjoint | 91,625 | 91,291 | 65,199 (0.06) | 106,427 (0.04) | **180,033** (0.03) | |
| users θ=0.99, 0% disjoint | 24,911 | 25,333 | 35,680 (0.07) | 35,188 (0.07) | **88,507** | |
| users θ=0.99, 50% disjoint | 38,044 | 39,466 | 46,630 (0.07) | 74,117 (0.04) | **162,348** (0.03) | |
| users θ=0.99, 90% disjoint | 65,304 | 66,942 | 80,587 (0.06) | 207,767 (0.12) | **217,465** (0.12) | |

On TPC-C at 1 warehouse, `cell+delta` commits 7.4× InnoDB SERIALIZABLE and 4.5× REPEATABLE READ without fsync, and 6.4× and 4.1× with it. Cell tracking alone gains 54% over row tracking. Deltas give the rest: Payment's `W_YTD` and `D_YTD` increments commute, and the only hot read-modify-write left, NewOrder's `D_NEXT_O_ID`, queues behind its FOR UPDATE lock. Without those waits it reruns after a commit-time conflict instead, and throughput falls to 1,810. In users, skew without disjoint columns (θ=0.99, 0%) defeats `row` and `cell`, while deltas keep `cell+delta` 3.6× ahead of InnoDB without fsync.

### Prepared statements

i7i, no per-commit fsync, 32 clients; commits/s.

| Workload | InnoDB SERIALIZABLE | InnoDB REPEATABLE READ | cell+delta |
|---|---|---|---|
| TPC-C, 1 warehouse | 654 | 982 | **5,123** |
| TPC-C, 4 warehouses | 2,211 | 4,500 | **6,242** |
| TPC-C, 1 warehouse, 1 ms think | 91 | 107 | **280** |
| users θ=0.5, 50% disjoint | 190,517 | 184,315 | 186,368 |
| users θ=0.9, 50% disjoint | 91,780 | 92,453 | **175,473** |
| users θ=0.99, 0% disjoint | 25,917 | 25,502 | **91,787** |
| users θ=0.99, 50% disjoint | 38,570 | 38,365 | **173,385** |
| users θ=0.99, 90% disjoint | 62,627 | 64,824 | **226,327** |
| ledger, 1 hot | 1,325 | 1,292 | **74,280** |
| ledger, 10 hot | 37,724 | 36,453 | **74,524** |
| ledger, uniform | 66,136 | 70,037 | 70,114 |

Prepared statements skip parsing in both engines, so the ratios hold: 7.8× on TPC-C at 1 warehouse and 56× on the hot account.

## Where the time goes

Profiles on the i7i with fsync per commit, in `bench/results/i7i/profile/`: `top` for CPU per process and per thread, `perf record -a` for samples by binary and symbol, `perf trace -s` for system calls per thread, and `perf sched` for wake-up delays.

### 128 clients, no contention

Users at θ=0.5, one 30-second run per engine.

| | Commits/s | p50 | `mysqld` CPU | `sqlbench` CPU | Machine idle |
|---|---|---|---|---|---|
| `celltnx` cell+delta | 134,293 | 854 µs | 13.5 cores | 8.6 cores | 53% |
| InnoDB SERIALIZABLE | 130,469 | 560 µs | 30.9 cores | 7.2 cores | 18% |

InnoDB spends 27% of all samples spinning: `rw_lock_s_lock_spin` 11.4%, `rw_lock_x_lock_func` 7.7%, `log_write_up_to` 5.4% and a mutex's `enter` 3.0%. Of the busy CPU in the `celltnx` run, with idle time excluded:

| Where | Share |
|---|---|
| Kernel: loopback networking and thread wake-ups, for client and server | 38% |
| MySQL's parser, executor and protocol | 36% |
| Benchmark client (`sqlbench`), much of it Go's scheduler | 20% |
| `celltnx` and lionturtle | 5% |

Neither engine runs out of CPU. Each transaction makes several round trips between the client and MySQL over loopback TCP, each waking a client goroutine and a server thread, and those round trips cap both engines. The client shares the machine with the server; a separate client machine was not measured. `celltnx`'s commits also queue behind its one store writer, which runs at 64% of a core at 137,800 commits/s, so its p50 is 50% higher than InnoDB's.

### 8 clients, uniform transfers

| | Transfers/s | p50 | Blocking waits per connection, 5 s |
|---|---|---|---|
| `celltnx` cell+delta | 10,668 | 501 µs | 16,823 `futex` calls, 1,007 ms |
| InnoDB SERIALIZABLE | 16,568 | 264 µs | 756 `futex` calls, 56 ms |

InnoDB's log writer thread never sleeps: in a 3-second sample it ran for 2,969 ms and switched out 1,682 times, and its flush thread called `fdatasync` 19,000 times a second. Its connection threads spin until their commit is durable. `celltnx`'s writer wrote 5,400 times a second, one direct `pwritev2` of about two commits, 29 µs each, and spent two-thirds of its time waiting for work. Each commit wakes the writer and is woken in turn, so a connection sleeps about 150 µs per commit.

### Group wait

After each sync, the store's writer may wait for the clients it just acknowledged to commit again, so they share the next sync. The wait lasts at most `celltnx_group_wait_us` (500 µs) or as long as the previous flush took, whichever is less: the full 500 µs on the desktop, where a flush takes about 2 ms, and tens of microseconds on the i7i. With the full 500 µs wait on the i7i (an earlier run, recorded in `bench/results/i7i/durable.log`), no wait at all ran up to 20% faster at 128 clients; `group-wait.csv` holds the runs with fixed waits of 0 and 50 µs.

| 128 clients, fsync | 500 µs | Capped (default) | 50 µs | 0 |
|---|---|---|---|---|
| users θ=0.5 | 112,380 | 123,054 | 131,024 | 134,548 |
| ledger, 1 hot | 72,053 | 75,102 | 77,663 | 78,372 |
| ledger, uniform | 72,723 | 76,635 | 78,260 | 79,419 |
| TPC-C, 1 warehouse | 6,185 | 6,482 | 6,322 | 6,535 |

Each column comes from a separate server run. The capped wait still trails no wait by up to 9% on users: the writer sleeps in 20 µs steps, which the kernel's timer slack stretches to about 70 µs.

## The drive

The same benchmark on the desktop, whose drive empties a volatile cache on every sync. fsync per commit, 128 clients; transactions/s.

| Workload | Desktop InnoDB | Desktop cell+delta | Desktop ÷ | i7i InnoDB | i7i cell+delta | i7i ÷ |
|---|---|---|---|---|---|---|
| Hot account | 8,411 | 27,288 | 3.2× | 1,856 | 75,102 | 40× |
| Hot account, 1 ms pauses | 419 | 14,416 | 34× | 348 | 32,035 | 92× |
| TPC-C, 1 warehouse | 2,627 | 9,159 | 3.5× | 842 | 6,482 | 7.7× |
| Uniform transfers | 26,035 | 27,650 | 1.1× | 71,528 | 76,635 | 1.1× |
| users θ=0.5 | 27,618 | 36,344 | 1.3× | 124,198 | 123,054 | 1.0× |

On the desktop, every commit waits about 2 ms on the drive, and both engines group concurrent commits into each flush, so the drive bounds both on uncontended runs. `cell+delta`'s hot account rises from 27,288 to 75,102 transfers/s on the faster drive. InnoDB's falls from 8,411 to 1,856, which the drive does not explain: its hot row ignores fsync on both machines. The desktop's durable runs used the 500 µs group wait; a 2 ms flush is longer, so the cap leaves that wait unchanged.

Desktop results without per-commit fsync, at 32 clients: the hot account gives 101,390 transfers/s against InnoDB's 10,738 (9.4×), 15× at 256 clients, and TPC-C at 1 warehouse 11,130 commits/s against 2,660 (4.2×). Its CSVs and logs are in `bench/results/desktop/`.

## Engine profile on the desktop

`perf record` of `mysqld` during desktop `cell+delta` runs without fsync:

| Workload | SQL layer (`mysqld`) | Kernel | libc | `ha_celltnx.so` | Commit-lock hold per commit |
|---|---|---|---|---|---|
| TPC-C, 1 warehouse, 32 clients | 50% | 26% | 9% | 7.5% | 11.6 µs |
| ledger, 1 hot, 128 clients | 41% | 32% | 10% | 8.5% | 4.4 µs |

At 32 clients TPC-C holds the commit lock 13% of the time, with a mean wait of 2 µs, and its latency comes from statements. In that run, 142,000 commits made 62,000 FOR UPDATE waits and 64,000 snapshot refreshes; 16 refreshes and 1% of commits conflicted.

The ledger exposed one engine bottleneck, now fixed. Each commit walked a written row's version chain from its head down to the oldest live snapshot to trim it, and validation walked every version newer than the transaction's snapshot. On a hot row at 128 clients, those walks spanned hundreds of versions under the commit lock. The longer the lock was held, the older the oldest snapshot grew, so the walks lengthened further. A chain now trims only once its installs since the last trim reach the versions that trim kept, and each version records where its run of same-mask versions began, so validation checks a run of deltas once. On the 1-hot ledger at 128 clients, the mean hold fell from 13.1 to 4.4 µs. Transfers/s on the desktop, with the pre-fix runs in `bench/results/desktop/old/`:

| Clients | 1 hot before | 1 hot after |
|---|---|---|
| 32 | 103,568 | 103,524 |
| 128 | 65,936 | 124,274 |
| 256 | 43,702 | 104,574 |

## What the numbers say

- **Contention is where `celltnx` wins.** A hot account serializes InnoDB at 1,100–1,900 transfers/s on the i7i whatever the clients or fsync; `cell+delta` runs it at 56,000–85,000 from 32 clients up, 39–57× InnoDB.
- **Long transactions widen the gap.** InnoDB holds a row lock across every pause inside the transaction, and a delta holds nothing: 92× with 1 ms pauses and fsync at 128 clients.
- **Every part of the design pays on TPC-C.** Cell tracking, deltas and FOR UPDATE queueing together give 6.4–7.7× InnoDB SERIALIZABLE at 1 warehouse, and still 2.2–2.4× at 4 warehouses.
- **Optimism needs deltas under skew.** `row` and `cell` retry about six times per transfer on a hot account; `cell+delta` retries none.
- **Without contention, everything around the engine sets the pace.** The storage engine takes 5% of the busy CPU; MySQL, the kernel's networking and the client take the rest. Up to 128 clients, throughput ties within 12%, and `celltnx` gets there on less than half InnoDB's CPU; at 256 clients InnoDB's uniform rate falls and `celltnx` leads 1.5×.
- **Two costs remain in `celltnx`'s durable path.** At 8 clients each commit sleeps through two thread handoffs, so InnoDB commits 1.6× faster; at high rates, commits queue behind one store writer. A committing thread could flush its own group instead of handing it to the writer, and the writer could wait precisely instead of in 70 µs sleeps.

## Reproducing

On an i7i.12xlarge running Ubuntu 26.04, format one instance-store drive as ext4 at `/mnt/nvme`, install the toolchain (GCC, CMake, Ninja, Bison, OpenSSL, libaio, libtirpc, Go 1.26, Rust 1.97), clone the repository there and set `MYSQL_ROOT=/mnt/nvme/celltnx`, so the build and the data directories live on the instance store. Then:

```
bench/build-mysql.sh release
bench/compare-mysql.sh durable.csv 8s 3 1
CLIENTS=128 MODES=cell+delta bench/compare-mysql.sh durable.csv 8s 3 1
for n in 8 256; do CLIENTS=$n MODES=cell+delta WORKLOADS="ledger-hot1 ledger-hot0" bench/compare-mysql.sh durable-clients.csv 8s 3 1; done
bench/compare-mysql.sh nosync.csv 8s 3 0
EXTRA=--celltnx-lock-wait-ms=0 LABEL=-nowait ONLY=celltnx MODES=cell+delta WORKLOADS="tpcc1 tpcc4 tpcc1-think" bench/compare-mysql.sh nosync.csv 8s 3 0
PREPARE=1 MODES=cell+delta bench/compare-mysql.sh nosync.csv 8s 3 0
for n in 8 32 128 256; do CLIENTS=$n MODES=cell+delta WORKLOADS="ledger-hot1 ledger-hot0" bench/compare-mysql.sh ledger-clients.csv 8s 3 0; done
bench/median.sh durable.csv
cpp/ci.sh
```

The CSVs and logs from these runs are in `bench/results/i7i/`, and the desktop's in `bench/results/desktop/`.
