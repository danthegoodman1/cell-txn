# cell-tnx

A prototype of serializable optimistic concurrency control that tracks conflicts per **(row, column)** instead of per row, merges rows at commit, and lets `col = col ± k` deltas commute. It runs in process, behind a MySQL-protocol server built on go-mysql-server, and durably on Badger. `SPEC.md` describes the design.

## MySQL engine

The core also runs inside MySQL 8.4: `cpp/` ports it to C++ as the `celltnx` storage engine, which keeps rows in memory and stores them durably in lionturtle (`ENGINE.md`). Against InnoDB in the same server, with every commit synced to disk on an EC2 i7i.12xlarge:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/speedup-dark.png">
  <img src="docs/images/speedup-light.png" alt="celltnx cell+delta over InnoDB SERIALIZABLE, transactions per second at 128 clients with fsync per commit: hot account with 1 ms pauses 92×, hot account 40×, TPC-C at 1 warehouse 7.7×, TPC-C at 4 warehouses 2.3×, 10 hot accounts 1.9×, heavily skewed users 1.7×, uniform transfers 1.1×, mildly skewed users 1.0×.">
</picture>

Contention sets the gap. A hot account holds InnoDB under 2,000 transfers a second at every client count, while `cell+delta` merges the balance increments at commit and runs it as fast as uniform transfers. Ledger transfers per second, fsync per commit:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/scaling-dark.png">
  <img src="docs/images/scaling-light.png" alt="Ledger transfers per second at 8, 32, 128 and 256 clients with fsync per commit. One hot account: celltnx 9,964, 55,737, 75,102 and 78,868; InnoDB 1,294, 1,446, 1,856 and 1,613. Uniform transfers: celltnx 8,848, 56,798, 76,635 and 79,143; InnoDB 14,336, 51,799, 71,528 and 54,443.">
</picture>

Without contention the engines tie on throughput up to 128 clients, and the storage engine is not what limits them. In a profile of an uncontended 128-client run, `celltnx` and lionturtle take 5% of the busy CPU. The kernel's loopback networking and thread wake-ups take 38%, MySQL's parser, executor and protocol 36%, and the benchmark client 20%, while half the machine sits idle. Every transaction makes several round trips between the client and MySQL, and those round trips set the pace for both engines. InnoDB reaches the same rate on 2.3 times the CPU. `celltnx`'s own code shows only at low concurrency, where each commit waits on two thread handoffs and InnoDB commits 1.6× faster at 8 clients.

`RESULTS-MYSQL.md` has every table, the profiles and a desktop drive for comparison; `PLAN-MYSQL.md` tracks the remaining work.

```sh
bench/build-mysql.sh release                          # MySQL 8.4.13 with the celltnx plugin
bench/compare-mysql.sh out.csv 8s 3 1                 # InnoDB and every celltnx mode, fsync per commit
bench/median.sh out.csv                               # median of each configuration's runs
cpp/ci.sh                                             # C++ tests under ASan and TSan, golden traces, sweeps, self-test
```

## Verifying it

```sh
go test -race ./...                                   # unit, stress, SQL and server tests; 1,000 core and 300 SQL seeds
go run ./cmd/sim -seeds 30000                         # core simulator: every workload and mode, simulated disk and crashes
go run ./cmd/sim -seeds 30000 -workload sql           # SQL simulator with the end-to-end oracle
go run ./cmd/sim -seed 1234 [-workload sql]           # replay one seed exactly
go test -tags simbugs -run SelfTest ./sim             # the simulator must catch 14 deliberate bugs
go test -run TestCrash ./store/badger                 # kill -9 and recover against real Badger
bench/enginetest.sh                                   # go-mysql-server's engine tests
```

## Running it

```sh
go run ./cmd/server -addr 127.0.0.1:3307 -mode cell+delta [-dir ./data]
mysql -h 127.0.0.1 -P 3307 -u root
```

`-mode` is `row`, `cell` or `cell+delta`. With `-dir`, commits persist in Badger and are acknowledged once synced (`-nosync` acknowledges first). `-commitdelay 500us` holds each sync open that long so concurrent commits share it. `-noplancache` plans every statement in full instead of reusing cached plans. `-pprof 127.0.0.1:6060` serves `net/http/pprof`, with mutex sampling, for profiling under load.

## Benchmarks

```sh
go run ./cmd/bench -workload tpcc -think 0.5           # in process, every mode
bench/sweep.sh out.csv 5s                              # in-process contention sweep
bench/compare.sh out.csv 8s [durable: 0|1]             # identical SQL against MySQL 8 and every mode
```

`bench/compare.sh` expects MySQL 8 on 127.0.0.1:3308; its header shows the `docker run` line. `PREPARE=1` sends statements as server-side prepared statements, and `SERVER_ARGS` passes flags to cell-tnx's server.

## Layout

| Path | Contents |
|---|---|
| `txn/` | transactions, multiversion store, secondary indexes, commit protocol, durability and recovery |
| `keys/` | order-preserving key encodings |
| `check/` | operation recorder and serializability checker |
| `workload/` | random, users and TPC-C workloads |
| `sim/` | deterministic simulator, simulated disk, SQL oracle |
| `store/badger/` | Badger store and crash test |
| `sqlgms/` | go-mysql-server backend and server |
| `cmd/` | `sim`, `bench`, `server`, `sqlbench` |
| `cpp/` | the C++ port: `core/`, `check/`, `workload/`, `sim/`, the lionturtle store in `store/lionturtle/`, and the MySQL engine and its MTR suite in `engine/` |
| `bench/` | benchmark scripts, and results in `bench/results/` |
