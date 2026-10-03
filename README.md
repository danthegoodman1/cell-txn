# cell-tnx

A prototype of serializable optimistic concurrency control that tracks conflicts per **(row, column)** instead of per row, merges rows at commit, and lets `col = col ± k` deltas commute. It runs in process, behind a MySQL-protocol server built on go-mysql-server, and durably on Badger. `SPEC.md` describes the design.

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

`-mode` is `row`, `cell` or `cell+delta`. With `-dir`, commits persist in Badger and are acknowledged once synced (`-nosync` acknowledges first). `-commitdelay 500us` holds each sync open that long so concurrent commits share it. `-pprof 127.0.0.1:6060` serves `net/http/pprof`, with mutex sampling, for profiling under load.

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
