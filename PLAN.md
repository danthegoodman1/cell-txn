# celltnx Development Plan

## Overarching Goal

Run celltnx's transaction core inside MySQL 8.4 as a storage engine named `celltnx`, and benchmark it against InnoDB in the same server on TPC-C, the users workload and the hot-account ledger. The engine follows `DESIGN.md`: optimistic execution, validation per (row, column), commit-time row merging, commutative deltas and one global commit lock. The target is a large margin over InnoDB SERIALIZABLE, from in-memory rows and optimistic execution in every mode, and from cell tracking and deltas under contention.

`celltnx` sits beside InnoDB, which stays unchanged and keeps MySQL's data dictionary. A `celltnx` table's rows live in the engine's memory, in a skiplist per table with per-row version chains, and lionturtle holds the durable copy.

Out of scope throughout: replication and the binlog, XA, transactions that span `celltnx` and InnoDB tables, foreign keys, atomic DDL, prefix, full-text and spatial indexes, tables over 63 columns, evicting rows from memory, Silo-style per-row latches, and upstreaming.

## Implementation Principles

- **Build for speed; prove optimizations with traces.** Use flat containers for read and write sets, arena-allocated row images, interned keys, no per-operation heap strings, and cache-line-separated shared atomics. A change meant to keep behavior identical must keep every simulator trace identical: `sim/golden.sh` compares seeds 0–9,999 with the revision before it. New behavior, such as snapshot refresh, sits behind a simulator flag whose default keeps existing traces, and ships with its own sweep and self-test bug.
- **Measure every phase.** Each phase that touches the hot path reports throughput against the previous one: in-process from Phase 2, over SQL from Phase 3. Profile before optimizing, and keep each optimization only if a benchmark shows it.
- **Keep MySQL out of the core.** `core/` uses only the standard library and reaches its host through hooks in `Options` (`wait`, `yield`, `interleave`, `now`, `store`, `gather`). `DESIGN.md`'s determinism rules apply to `core/`, `check/` and `workload/`: no threads, clocks, global random source or iteration in hash order.
- **Track conservatively.** When the engine cannot prove a statement leaves a column unread, it tracks the column. When it cannot prove an assignment is a delta, it records a read and a write. Each refinement ships with a test showing the conservative path still applies where it should.
- **Keep the adapter thin.** `engine/` translates MySQL rows, keys, statements and errors into core calls. Every concurrency decision stays in the core.
- **Reclaim memory through the snapshot registry.** A commit trims only versions that no registered snapshot can reach, so a trimmed chain has no readers: it is freed at once without a store, and held until durable with one. The simulator keeps trimmed chains so the checker can still read them; ASan runs cover the freeing path.
- **Compare like with like.** Benchmarks run InnoDB and `celltnx` in the same `mysqld` binary with the same flags, clients and hardware.
- **Document the current design.** `DESIGN.md` describes the core and the engine as they stand.

## Testing Strategy

- **Unit and scenario tests** (GoogleTest, `tests/`): the core scenarios, indexes, skiplist, checker, workloads, vectors and the store.
- **Deterministic simulation** (`sim/`): scheduler, swarm parameters, faults, invariants, crashes and self-test bugs, on stackful fibers. A 30,000-seed sweep gates every phase that touches the core.
- **Golden traces:** `sim/golden.sh` checks that a change meant to keep behavior identical leaves every seed's trace, steps, crashes and outcome unchanged.
- **Sanitizers:** stress tests on real threads under TSan, and the whole suite under ASan and UBSan. Engine tests also run on `mysqld` built with `-DWITH_ASAN=ON` and `-DWITH_TSAN=ON`.
- **MTR:** `mysql-test-run` tests in `engine/mysql-test/t` (written `t/` below) script multi-connection interleavings. MySQL's own suites run with `--default-storage-engine=celltnx`, and the failures are recorded.
- **Wire oracle:** random SQL transactions from many connections run against `mysqld`. Every committed transaction is replayed serially, in the engine's serialization order, against InnoDB in the same server; every result and the final tables must match.
- **Crash test:** `kill -9` `mysqld` during TPC-C. The recovered state must equal the serial replay of every acknowledged commit.
- **Escaped bugs** get a simulator, oracle or MTR reproduction before the fix. Concurrency changes also pass the TSan stress tests under load.
- **Cleanup:** scripts remove the temporary directories, data directories, scratch binaries and containers they create.

## Phase 0: Foundations and MySQL spikes

Goal:
Fix the toolchain and layout, and resolve the MySQL unknowns that could force server patches, before building the engine.

Scope:
- Layout: `core`, `check`, `workload`, `sim`, `store`, `engine` and `tests` at the repository root. `bench/build-mysql.sh` checks out a pinned MySQL 8.4 tag and links `engine/` in as `storage/celltnx`.
- Toolchain: CMake, the MySQL tree's C++ standard, GoogleTest, Boost.Context for simulator fibers, lionturtle's C library for durability, and sanitizer presets.
- Spikes on a throwaway engine derived from `storage/example`.
- Golden traces for checking that optimizations keep behavior.

Completion gate:
Every preset builds; a `mysqld` built by `bench/build-mysql.sh` loads the engine; each spike has a recorded answer and, where needed, a chosen fallback; `sim/golden.sh` compares traces with a revision.

Testing plan:
- `ci.sh` builds every preset.
- Each spike is an MTR test that prints the behavior it observed.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Decision | 0A: MySQL 8.4 LTS, pinned to one tag (8.0 reached end of life in April 2026) | `bench/build-mysql.sh` pins `mysql-8.4.13`, the newest 8.4 tag on 2026-10-31. |
| Complete | Decision | 0B: The MySQL tree's C++ standard; `core/` compiles with no MySQL headers | `CMakeLists.txt` sets C++20, as MySQL 8.4 does; `celltnx_core` builds standalone in every preset and inside the plugin from the same sources (`engine/CMakeLists.txt`). |
| Complete | Decision | 0C: Simulator coroutines are stackful Boost.Context fibers | `sim/coroutine.h`; seeds 0–9,999 run in 21 s on 24 threads. Most of that is system time from mapping each fiber's guarded stack, which a stack pool would remove. |
| Complete | Decision | 0D: The durable store is lionturtle (v0.2.1, then main at 5077607 for `preallocate_log`) through its C library, linked statically into the plugin with `-Wl,--exclude-libs,ALL`. One `lt_write` and one `lt_sync` per flush of the commit queue, with `preallocate_log` so syncs overwrite zero-filled segments; `checkpoint_distance` 8 and `cache_bytes` 64 MiB. RocksDB stays the fallback behind `Store` | `store/lionturtle/check/` (run against `03dc0c4`, 2026-10-31): a plugin-style `.so` exports only its own symbols; TPC-C-shaped commits from one writer reach 50k commits/s unsynced (`lt_write` p99.9 < 0.2 ms) and 7.2k/s at 32 commits per 4 ms sync on NVMe, with 1.3–1.7× write amplification; 21 `kill -9` rounds and 5 injected sync failures recover every acknowledged commit, checked row by row; peak RSS 0.8 GB at these settings against 2.4 GB at the defaults. |
| In Progress | Decision | 0E: Engine keys use the core's order-preserving encodings over each column's value: integers and floats as numbers, strings as their collation weights from `strnxfrm` (padded for PAD SPACE collations), and fixed-width binary images (DECIMAL, DATETIME2, TIMESTAMP2, TIME2) as escaped bytes, with a NULL tag per nullable part | `EncodeValue` and `EncodeKey` in `engine/ha_celltnx.cc`; `Vectors.Keys` checks 62 key vectors. Missing: MTR test of collation-equal strings, order across types, and key sizes. |
| Complete | Work | 0F: Skeleton, presets (debug, release, asan, tsan, simbugs) and `ci.sh` | `CMakeLists.txt`, `CMakePresets.json`; `ci.sh` builds every preset, runs every test binary in debug, asan and tsan, then the golden comparison, the 30,000-seed sweeps and the self-test. |
| Complete | Work | 0G: `bench/build-mysql.sh` builds `mysqld` with the engine as a dynamic plugin | `bench/build-mysql.sh release celltnx` builds `ha_celltnx.so`; `mysqld --plugin-load-add=ha_celltnx.so` loads it, and the MTR suite loads it through `t/suite.opt`. |
| Complete | Risk | 0H: Commit-time errors. Call `my_error(ER_LOCK_DEADLOCK)` or `my_error(ER_CHECK_CONSTRAINT_VIOLATED)` before the handlerton `commit` returns; the client sees 1213 or 3819, the server rolls the transaction back, and an autocommit statement reports the error itself | `t/commit_conflict.test` and `t/cells.test`: MySQL keeps the first error, so the client sees 1213 or 3819 with 1180 as a warning, and the session continues. No server patch. |
| Complete | Risk | 0I: Delta detection. From `external_lock` or `start_stmt`, reach the SET list of `Sql_cmd_update` and the ON DUPLICATE KEY UPDATE list of `Sql_cmd_insert_base`, with their `Item` trees, for text and prepared statements | `ClassifyDeltas` reads both lists; `t/cells.test` and `t/deltas.test` show UPDATE, ON DUPLICATE KEY UPDATE and prepared deltas commuting. |
| Complete | Risk | 0J: CHECK expressions. At table open, read each CHECK constraint's `Item` tree from the `TABLE` | `InstallChecks` translates `table_check_constraint_list`; `t/cells.test` scenario 8 fails a merged CHECK at commit with 3819. |
| In Progress | Risk | 0K: Column sets. Record `table->read_set` and `write_set` for every TPC-C and users statement with the binlog off | `Prepare` builds masks from both bitmaps; `t/cells.test` scenario 1 commits disjoint columns. Missing: the per-statement bitmap listing for the benchmark statements. |
| Complete | Work | 0L: Golden traces. `sim -golden FILE` writes each seed's trace, steps, crashes, failure and parameters; `sim/golden.sh [rev]` builds a revision in a temporary worktree and compares | `sim/main.cc` `-golden`; `sim/golden.sh`; `ci.sh` runs it against HEAD for seeds 0–9,999. |
| Incomplete | Gate | Phase 0 gate | Missing: every row above complete. |

## Phase 1: Transaction core

Goal:
`core/` implements the data model, transaction API, commit protocol, secondary indexes, CHECKs, GC, the durability queue and recovery that `DESIGN.md` describes.

Scope:
- Order-preserving key encodings.
- The single-writer skiplist with lock-free readers, hash-derived heights, seeks that return the node they compared, and nodes that are never removed.
- Multiversion rows with write masks, buckets, absent-key reads, unique-index claims, deltas, savepoints and the three modes.
- The commit protocol: validate, merge, CHECK, hook, enqueue, install, publish.
- GC with the active-snapshot registry, and reclamation through it.
- The `Store` interface, durability queue, writer with group commit and commit delay, and recovery.
- Deliberate bugs behind `CELLTNX_SIMBUGS`, one per self-test bug in `DESIGN.md`.

Out of scope:
- The simulator and workloads (Phase 2), and RocksDB.

Completion gate:
The unit and scenario tests pass in every preset, and the TSan stress test on real threads is clean.

Testing plan:
- Scenario tests in `tests/txn_test.cc`, index tests in `tests/index_test.cc` and the skiplist test in `tests/skiplist_test.cc`.
- Run scenarios 7 and 8 and random concurrent transactions on real threads under TSan and ASan.
- In debug builds, assert that no reader touches a poisoned retired version.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Work | 1A: Key encodings | `core/keys.h`; `Vectors.Keys` checks 62 vectors in `tests/testdata/key_vectors.txt`. |
| Complete | Work | 1B: Skiplist with sequentially consistent atomics | `core/skiplist.h`; `Skiplist.ConcurrentGet` under TSan; the `seek-reload` and `link-before-init` self-test bugs. |
| Complete | Work | 1C: Rows, version chains, write masks and buckets | `core/db.h`, `core/internal.h`; `Txn.DisjointColumnsMerge`, `Txn.Phantom`; simulator sweeps. |
| Complete | Work | 1D: Transaction API and semantics (`Get`, `GetUntracked`, `GetRow`, `Scan`, `IndexScan`, `ScanRows`, `Set`, `Add`, `Insert`, `Delete`, savepoints, modes) | `core/txn.cc`; scenarios 1–9 in `tests/txn_test.cc`; simulator sweeps. |
| Complete | Work | 1E: Secondary indexes and unique claims | `tests/index_test.cc`; `unique-no-read` and `index-no-range` self-test bugs. |
| Complete | Work | 1F: Commit protocol, CHECKs, conflicts and constraint failures, with checked arithmetic (`__builtin_add_overflow`) | `core/commit.cc`; `Txn.DeltaCheck`, `Txn.OverflowIsConstraint`. |
| Complete | Work | 1K: Size limits. `Options` carries the store's key and row limits (65,535 and 16 MiB for lionturtle). A write past either fails at once, and a merge whose image exceeds the row limit, as when two transactions each grow different columns, fails with `kConstraint` before anything installs: the writer persists commits already visible in memory, so it must never meet a batch the store rejects | `Txn.RowSizeLimit`. |
| Complete | Work | 1G: GC and the active-snapshot registry | `Txn.GCKeepsSnapshots`; `gc-eager` self-test bug. |
| Complete | Work | 1H: Reclamation through the registry: trimmed chains are freed at once without a store, held until durable with one, and kept in the simulator | `Trim` and `DB::Retire` in `core/`; a chain trims once its installs since the last trim reach the versions that trim kept, and validation checks a run of same-mask versions once (`Version::run_ts`); core tests clean under ASan, including `Txn.GCKeepsSnapshots`, `Txn.ValidateAcrossDeltaRuns` and `Txn.RefreshPinsEarlierRows`. |
| Complete | Work | 1I: `Store` interface, durability queue, writer, commit delay and recovery | The simulator's `Disk` store with crashes, sync failures and commit delays; `ack-before-sync`, `read-only-no-wait` and `ack-joined` self-test bugs. |
| Complete | Work | 1J: `CELLTNX_SIMBUGS` deliberate bugs | `core/bugs.h`: 15 bugs, listed in `DESIGN.md`. |
| Complete | Test | Unit and scenario tests in every preset | `core_test`, `vectors_test`, `check_test`, `workload_test`, `stress_test` and `store_test` pass in debug, asan and tsan under `ci.sh`. |
| Complete | Test | TSan stress on real threads | `stress_test` (27 cases across workloads and modes) and `Skiplist.ConcurrentGet` clean under TSan. |
| Incomplete | Gate | Phase 1 gate | Missing: every row above complete. |

## Phase 2: Checker, workloads and simulator

Goal:
The simulator runs every workload with faults and crashes, passes 30,000-seed sweeps with determinism reruns, and finds every deliberate bug; an in-process benchmark sets a baseline.

Scope:
- `check/`: recorder, the FNV trace over canonical bytes, and replay against an independent model.
- `workload/`: random, users and TPC-C, Zipf, and the client interface.
- The PCG generator and its `Rand` methods, and `exp`, `log` and `pow` as fixed IEEE sequences, so a seed draws the same values on every compiler.
- `sim/`: scheduler, swarm draws, faults, invariants, simulated disk, crashes and recovery, determinism reruns, and the `-seed`, `-seeds`, `-soak` and `-bug` flags.
- An in-process benchmark of every workload and mode on real threads.
- A determinism lint for `core/`, `check/` and `workload/`.

Out of scope:
- SQL in the simulator; Phase 3's wire oracle covers SQL.

Completion gate:
The simulator passes a 30,000-seed sweep for each workload with determinism reruns, and finds every deliberate bug within 30,000 seeds.

Testing plan:
- A 30,000-seed sweep for each workload, and with drawn refresh rates.
- The self-test with every `CELLTNX_SIMBUGS` bug.
- Every workload on real threads under TSan with full history replay (`tests/stress_test.cc`).
- The in-process benchmark on TPC-C with think time 0.5, in every mode.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Work | 2A: PCG, `Rand` methods, and fixed-sequence `exp`, `log` and `pow` | `workload/rand.h`, `workload/detmath.h`; `Vectors.Rand`, `Math.FMA` and `Math.NoFMA` check them against recorded vectors in `tests/testdata/`. |
| Complete | Work | 2B: Recorder, trace and replay | `check/`; `check_test` rejects 16 altered histories and replays a recorded one. |
| Complete | Work | 2C: Random, users and TPC-C workloads with Zipf | `workload/`; `Workload.Draws` and `Workload.Traces` check configurations and 540 multi-client traces against recorded vectors. |
| Complete | Work | 2D: Fiber scheduler with `wait`, `yield` and `interleave` | `sim/sim.cc`, `sim/coroutine.h`; simulator sweeps. |
| Complete | Work | 2E: Simulated disk, crashes and recovery checks | `Disk` in `sim/sim.cc`; durable seeds in the sweeps crash, recover and replay. |
| Complete | Work | 2F: Internal, workload, durability and liveness invariants | `sim/sim.cc`; the self-test bugs. |
| In Progress | Work | 2G: Swarm draws and the CLI | `sim/main.cc` takes `-seed`, `-seeds`, `-start`, `-workers`, `-workload`, `-mode`, `-durable`, `-commitdelay`, `-interleave`, `-refresh`, `-bug`, `-det` and `-golden`. Missing: `-soak`. |
| Incomplete | Work | 2H: Determinism lint | Missing: script and a failing case it rejects. |
| Incomplete | Work | 2I: In-process benchmark | Missing: code. |
| Complete | Risk | 2J: Every sort in `core/` and `check/` is deterministic: unique keys or a stable sort | `Replay`'s index ranges sort on unique (entry key, primary key) pairs and its log sort is stable (`check/replay.cc`); `Txn::Own` uses `std::stable_sort` (`core/txn.cc`). |
| Complete | Test | 30,000-seed sweeps, per workload, with determinism reruns | `ci.sh`: 30,000 seeds each of random, users and TPC-C, and 30,000 with drawn refresh rates: 0 failures, and 0 mismatches over 3,000 reruns each. |
| Complete | Test | Self-test finds every deliberate bug | 30,000-seed runs find each bug, first at seed: skip-buckets 2, set-no-exists 6, gc-eager 5, check-skip-delta 4, no-absent-read 10, delta-read-untracked 594, unique-no-read 35, index-no-range 2, ack-before-sync 1, read-only-no-wait 1, ack-joined 4, seek-reload 16, publish-early 5, link-before-init 15, refresh-no-validate 3 (with `-refresh -1`). |
| Complete | Test | TSan stress across workloads and modes | `stress_test` clean under TSan. |
| Incomplete | Test | In-process baseline | Missing: commits/s per mode on TPC-C and users. |
| Incomplete | Gate | Phase 2 gate | Missing: every row above complete. |

## Phase 3: MySQL engine in row mode

Goal:
`celltnx` runs SQL correctly inside `mysqld` in `row` mode, where every statement reads and writes whole rows, so the handler is correct before any precision work.

Scope:
- Plugin and handlerton: read-only system variables `celltnx_mode` and `celltnx_bucket_bits`; one core transaction per THD through `thd_get_ha_data`.
- Table lifecycle: create (rejecting tables over 63 columns and unsupported index types), open, close, drop, rename and truncate, with the data-dictionary object ID as table identity. CREATE INDEX and ALTER TABLE run through MySQL's copy algorithm.
- Rows: integer and floating-point columns become core numbers, and every other column becomes its byte image, round-tripped through the Field API. The handler owns BLOB and TEXT buffers until its next read. Keyless tables get a hidden counter key.
- Keys: encode per 0E, decode `index_read_map` key buffers with `key_restore`, and set the bucket prefix from the leading numeric key parts.
- Reads: `rnd_init`/`rnd_next`; `index_read_map` with every `ha_rkey_function`; `index_next`, `index_prev`, `index_first`, `index_last`, `index_next_same` and `index_read_last_map`; `read_range_first`/`read_range_next`; and `position`/`rnd_pos`, with the skiplist node pointer as an 8-byte ref.
- Cursors see the transaction's own writes through an overlay whose iterators survive inserts by the same statement.
- Range tracking: a cursor records buckets only for the interval it has traversed, through the key that ended it, so `LIMIT` and open-ended `index_read_map` scans claim no range they never read.
- Writes: `write_row`, `update_row` and `delete_row`. A duplicate key returns `HA_ERR_FOUND_DUPP_KEY` with `errkey` and `dup_ref` set, so REPLACE and ON DUPLICATE KEY UPDATE work; the engine reads the duplicate row in full and tracks it.
- Transactions: `trans_register_ha` per statement and per transaction; `readTs` at the first statement, or at `START TRANSACTION WITH CONSISTENT SNAPSHOT`; statement rollback through a core savepoint; named savepoints; commit and rollback; abort on disconnect. KILL cannot interrupt the commit lock's critical section.
- Errors: a conflict returns 1213, a CHECK failure at commit 3819, and a duplicate key 1062, using the mechanism chosen in 0H. A delta overflow at commit returns MySQL's out-of-range error. A row whose encoded image passes the store's 16 MiB limit fails its statement with `ER_TOO_BIG_ROWSIZE`, and at commit (1K) with the same error.
- Optimizer: `info()` row counts and `records_in_range` estimates that steer point lookups to the primary key; `table_flags` and `index_flags` that decline ICP, MRR and BKA.
- Locks: table locks never block. `LOCK IN SHARE MODE` is a tracked read; `SELECT … FOR UPDATE` is a tracked read that may also wait for a row lock (4H).
- Auto-increment: a per-table atomic counter, as in InnoDB's interleaved mode.
- Isolation: always serializable, whatever `transaction_isolation` says.
- Session status variables `Celltnx_last_commit_ts` and `Celltnx_last_read_ts`, and global counters for conflicts by cause and commit-lock wait and hold.
- Wire oracle (`bench/wireoracle`): a seeded generator of random SQL transactions covering point, range and secondary-index reads, aggregates, joins, a correlated subquery, deltas over one and many rows, blind and indexed writes, unique-key collisions, ON DUPLICATE KEY UPDATE, REPLACE, single and range deletes, explicit and autocommit transactions, and rollbacks. Seeded clients run against `celltnx` tables; committed transactions then replay serially in serialization order against InnoDB copies of the tables in the same server, comparing rows, matched and affected counts, error classes and final tables. A transaction rejected at commit must fail its CHECK when replayed after the writers before it.
- `bench/sqlbench` gains `-engine NAME` (sets the engine for setup) and `-audit` (TPC-C consistency conditions 1–4 as SQL after the run).

Out of scope:
- Column-level tracking and deltas (Phase 4) and durability (Phase 6).

Completion gate:
The MTR suite passes on debug, ASan and TSan builds; the wire oracle passes 10,000 seeds at 4, 16 and 64 connections; TPC-C and users run from `bench/sqlbench` and pass `-audit`; MySQL's main suite has run with `--default-storage-engine=celltnx` and its failures are recorded and classified.

Testing plan:
- MTR: the core scenarios from `tests/txn_test.cc` in SQL with `row`-mode outcomes; indexes; savepoints and statement rollback; duplicate keys, REPLACE and ON DUPLICATE KEY UPDATE; auto-increment; keyless tables; every `ha_rkey_function`; collations, BLOBs and NULLs; the DDL lifecycle; and commit-time errors.
- The wire oracle on release and TSan builds.
- MySQL's main suite with the engine as the default.
- `bench/sqlbench` TPC-C and users on a TSan build.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Work | 3A: Plugin, handlerton and system variables | `engine/ha_celltnx.cc`; `t/status.test` lists every variable's default, sets a dynamic one and rejects setting a read-only one; `t/for_update.test` exercises `celltnx_lock_wait_ms`. |
| In Progress | Work | 3B: Table lifecycle, durable identity and copy ALTER | Create, open, close, drop, rename and truncate; the data-dictionary object ID is the durable identity (6D); `t/durable.test` covers the lifecycle across restarts, and `t/for_update.test` runs a copy `ALTER TABLE … ADD COLUMN` that keeps every row. Missing: CREATE INDEX and DROP INDEX through copy ALTER. |
| In Progress | Work | 3C: Row conversion through the Field API | `ReadField` and `WriteField`. Missing: MTR round-trip test for every column type. |
| In Progress | Work | 3D: Key encoding, decoding and bucket prefix | `EncodeKey` decodes key buffers with `key_restore`; the bucket prefix spans 9 bytes per leading numeric key part. Missing: MTR order and collation tests. |
| In Progress | Work | 3E: Read paths, `position` and `rnd_pos` | Every handler read path; an equality range on the whole primary key is a point read. Missing: MTR test per `ha_rkey_function`. |
| In Progress | Work | 3F: Own-write overlay with stable iterators | Scans return a statement-start view of the transaction's writes (`Txn::BeginStatement`). Missing: MTR test of a statement that inserts into the range it scans. |
| Incomplete | Work | 3G: Bucket tracking of traversed intervals | Missing: code and MTR phantom tests with `LIMIT` and open-ended scans. |
| In Progress | Work | 3H: Write paths and duplicate-key handling | `write_row`, `update_row`, `delete_row`; duplicates report 1062 through `errkey`. Missing: MTR tests for REPLACE and ON DUPLICATE KEY UPDATE. |
| In Progress | Work | 3I: Transaction hooks, savepoints, statement rollback, disconnect and KILL | Hooks, savepoints, statement rollback and disconnect in `engine/ha_celltnx.cc`; each connection reuses its finished transaction (`Txn.BeginReuse`). Missing: KILL, and MTR tests of savepoints and disconnect. |
| In Progress | Work | 3J: Error mapping (1213, 3819, 1062, out of range) | `ReportCommitError`; 1213 and 3819 in `t/commit_conflict.test` and `t/cells.test`. Missing: tests of 1062 and out of range. |
| In Progress | Work | 3K: Optimizer statistics and flags | `records_in_range` counts committed keys in the range, up to 1,000 (`DB::CountRange`). Missing: `EXPLAIN` of every benchmark statement showing primary-key or index access. |
| In Progress | Work | 3L: Non-blocking table locks; LOCK IN SHARE MODE as a tracked read | `lock_count` is 0. Missing: MTR test of LOCK IN SHARE MODE and LOCK TABLES. |
| In Progress | Work | 3M: Auto-increment | `get_auto_increment` takes `DB::NextID`. Missing: MTR tests, including gaps after aborts. |
| Complete | Work | 3N: Status variables for serialization order and counters | Session `Celltnx_last_commit_ts` and `Celltnx_last_read_ts`; global commits, conflicts by cause, CHECK failures, FOR UPDATE waits, timeouts and refreshes, and commit-lock wait and hold; `t/status.test`. |
| Incomplete | Work | 3O: Wire oracle | Missing: `bench/wireoracle`, its statement generator, and a run that catches a deliberately broken engine build. |
| Complete | Work | 3P: `sqlbench -engine` and `-audit` | `-engine` and `-audit` in `bench/sqlbench/main.go`: TPC-C consistency conditions 1–4, and ledger balances reconciled with the ledger; every audited run in `bench/results/*/*.log` passes. |
| Complete | Doc | `DESIGN.md` covers the handler, row and key formats, transactions and errors | `DESIGN.md`, The MySQL engine. |
| Incomplete | Test | MTR suite on debug, ASan and TSan builds | Missing: run logs. |
| Incomplete | Test | Wire oracle: 10,000 seeds at 4, 16 and 64 connections | Missing: run logs. |
| Incomplete | Test | MySQL main suite with `--default-storage-engine=celltnx` | Missing: `engine/mtr-main.txt` with failures classified. |
| Incomplete | Gate | Phase 3 gate | Missing: every row above complete. |

## Phase 4: Cell tracking, deltas and edge cases

Goal:
`cell` and `cell+delta` modes track exactly the columns each statement uses, turn qualifying `col = col ± k` assignments into deltas, and fall back to whole-row tracking wherever precision is unproven.

Scope:
- Read masks come from each statement's `table->read_set` and write masks from `write_set`; columns outside `read_set` return untracked. Primary-key columns added by `prepare_for_position` cost no conflicts: a key never changes in place, and `kExists` already covers inserts and deletes.
- Assigned non-delta columns count as read, because `compare_records` skips unchanged rows based on their old values.
- Deltas: at statement start (0I), an assignment `c = c ± k` becomes a delta when `c` is an integer column, `k` is an integer literal or parameter, `c` appears nowhere else in the statement (WHERE, other assignments, ORDER BY, subqueries), and `c` belongs to no key or index. `update_row` then records `Add(new − old)`.
- Whole-row statements: UPDATE IGNORE and INSERT IGNORE, multi-table UPDATE and DELETE, LOAD DATA, tables with triggers or generated columns, and any statement shape the classifier doesn't recognize read and write every column.
- Statement failures: when a statement rolls back, every column of every row it read becomes a tracked read. An error decided from an untracked value, such as a statement-time CHECK failure or an out-of-range `c + k`, then rests on validated data.
- Commit-time CHECKs (0J): at table open, translate each CHECK `Item` into a core predicate over integer, floating-point and string columns (comparisons, arithmetic, AND, OR, NOT, IS NULL, constants and columns). A CHECK outside that subset makes its columns final: a write to any of them reads all of them, and none takes deltas.
- Optional: engine condition pushdown (`cond_push`), so rows that fail a pushed WHERE track only the condition's columns. Build it only if Phase 5's profiles show false conflicts from scans.
- FOR UPDATE queues (4H): a primary-key read in `SELECT … FOR UPDATE` takes an advisory row lock. A transaction holding no lock waits for it, up to `celltnx_lock_wait_ms`, so hot read-modify-writes such as NewOrder's district update queue instead of rerunning after a commit-time conflict. After the lock, `Txn::Refresh` moves the snapshot past the commit it waited for, validating every earlier read; a failed refresh rolls back at once with 1213. A delta on a column the lock's users read queues behind them too. Validation alone decides serializability, so a lock that times out or is skipped costs at most a conflict.

Out of scope:
- Deltas on non-integer columns.

Completion gate:
The MTR suite passes the core scenarios with `cell`-mode outcomes; the wire oracle passes in every mode; on identical `bench/sqlbench` TPC-C runs, conflicts by cause follow the design: `cell` removes NewOrder–Payment conflicts and `cell+delta` also removes Payment–Payment conflicts.

Testing plan:
- MTR: each classifier rule and each whole-row case, with a second connection showing that a conflict does or doesn't occur.
- MTR: CHECK translation, the untranslatable-CHECK fallback, statement-failure read promotion, and the IGNORE fallback.
- Wire oracle with deltas, CHECKs on delta columns, ON DUPLICATE KEY UPDATE and REPLACE.
- Conflicts by cause per mode on identical `bench/sqlbench` TPC-C runs, from `Celltnx_conflicts` and `Celltnx_bucket_conflicts`.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Work | 4A: Masks from `read_set` and `write_set`, untracked returns | `Prepare`; `t/cells.test` scenario 1. |
| Complete | Work | 4B: Assigned columns count as read | `t/cells.test`: two transactions assigning one cell; the second commit conflicts. |
| In Progress | Work | 4C: Delta classifier for UPDATE and ON DUPLICATE KEY UPDATE, text and prepared | `ClassifyDeltas` and `IsDelta`; ON DUPLICATE KEY UPDATE writes only its clause's columns (`assigned_`); `t/cells.test` and `t/deltas.test` cover commuting UPDATE, ON DUPLICATE KEY UPDATE and prepared deltas, and assignments read elsewhere. Missing: a test per remaining rule (ORDER BY, subquery, indexed and final columns). |
| In Progress | Work | 4D: Whole-row fallback cases | UPDATE IGNORE, triggers and generated columns take no deltas. Missing: the remaining cases and an MTR test per case. |
| Complete | Work | 4E: Read promotion on statement rollback | `Trx::promote`; `t/cells.test`: a statement failing on a delta's base value makes it a tracked read. |
| In Progress | Work | 4F: CHECK translation and final-column fallback | `Translate` and final columns in `engine/ha_celltnx.cc`; `t/cells.test` scenario 8. Missing: test of the untranslatable fallback. |
| Incomplete | Work | 4G: Engine condition pushdown (optional) | Missing: profile evidence that it is needed. |
| Complete | Work | 4H: FOR UPDATE row locks with snapshot refresh | `RowLocks` and `LockRow` in `engine/ha_celltnx.cc`, `Txn::Refresh` and `Txn::Changed` in the core; deltas queue behind a holder that uses their column; `Txn.RefreshPastDisjointWrites`, `Txn.RefreshConflicts`, `Txn.RefreshPinsEarlierRows`; `t/for_update.test`; simulator sweeps with refresh and the `refresh-no-validate` self-test pass. `BENCHMARKS.md`, TPC-C and users: without fsync on the i7i, `cell+delta` at 1 warehouse commits 4,178/s with waits and 1,810/s without. |
| Complete | Doc | `DESIGN.md` covers column tracking, deltas, fallbacks and CHECKs | `DESIGN.md`, Column tracking and deltas; SELECT … FOR UPDATE. |
| Incomplete | Test | MTR scenarios in `cell` and `cell+delta` | Missing: run log. |
| Incomplete | Test | Wire oracle: 10,000 seeds per mode | Missing: run logs. |
| Incomplete | Test | Conflicts by cause per mode follow the design | Missing: table of conflicts by cause per mode on TPC-C at 1 warehouse. |
| Incomplete | Gate | Phase 4 gate | Missing: every row above complete. |

## Phase 5: In-memory benchmark against InnoDB

Goal:
Measure `celltnx` against InnoDB in the same `mysqld` without per-commit fsync, and decide whether durability is worth building.

Scope:
- `bench/compare-mysql.sh` starts the built `mysqld` for each configuration with identical flags (`--disable-log-bin`, `--innodb-buffer-pool-size=4G`, `--max-connections=1000`, `--innodb-lock-wait-timeout=10`, `--mysqlx=OFF`). It runs `bench/sqlbench` with 32 clients over TPC-C at 1 and 4 warehouses, TPC-C with 1 ms think time, users across the θ and disjoint-share grid, and the hot-account ledger at 1, 10 and no hot accounts and with 1 ms pauses, sends statements as text and with `PREPARE=1`, takes the median of three runs, and appends to a CSV. A client sweep from 8 to 256 runs the ledger with 1 hot account and uniformly.
- Configurations: InnoDB SERIALIZABLE, InnoDB REPEATABLE READ, and `celltnx` in `row`, `cell` and `cell+delta`. Reference: InnoDB in the official `mysql:8.4` image, showing whether the source build matches a release build.
- No per-commit fsync on either side: `celltnx_sync=OFF`, `innodb_flush_log_at_trx_commit=0`. Both still write: InnoDB its redo log, `celltnx` its lionturtle store.
- `perf record` profiles of `mysqld` per configuration, and commit-lock wait and hold from the engine's counters.
- `BENCHMARKS.md`.

Out of scope:
- Durable comparisons (Phase 6).

Completion gate:
`BENCHMARKS.md` reports the full matrix and the decision. Proceed to Phase 6 only if `cell+delta` beats InnoDB SERIALIZABLE on TPC-C at 1 warehouse in the same server; report how much of the gain comes from cell granularity and how much from deltas.

Testing plan:
- The MTR suite and wire oracle pass on the benchmarked build.
- `bench/sqlbench -audit` passes after every TPC-C and ledger run.
- Report run-to-run variation for each configuration.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Work | 5A: `bench/compare-mysql.sh` | `bench/compare-mysql.sh` and `bench/median.sh`; full runs on EC2 i7i in `bench/results/i7i/` and on the desktop in `bench/results/desktop/`: `nosync.csv`, `ledger-clients.csv` and `durable.csv`. |
| Incomplete | Work | 5B: Reference run in the official image | Missing: CSV rows for InnoDB in the official `mysql:8.4` image. |
| Complete | Work | 5C: Profiles and commit-lock metrics | `BENCHMARKS.md`, Where the time goes and Engine profile on the desktop: CPU by process, binary and symbol, and commit-lock hold per commit, from `perf` and `Celltnx_commit_lock_hold_ns`; the ledger profile found the version-chain walk under the commit lock (1H). Commit-lock p99 needs a histogram the engine lacks; it reports totals. |
| Complete | Doc | 5D: `BENCHMARKS.md` | `BENCHMARKS.md`: summary, setup, correctness, hot accounts, TPC-C and users, prepared statements, profiles, the drive, reproduction. |
| In Progress | Test | Audit and oracle on the benchmarked build | i7i: 177 TPC-C and 327 ledger audits consistent; desktop: 153 and 264 (`bench/results/*/*.log`); MTR 6 of 6 on the desktop build. Missing: wire oracle (3O). |
| Complete | Decision | Proceed to Phase 6 | TPC-C at 1 warehouse without fsync on the i7i: `cell+delta` 4,178 commits/s against InnoDB SERIALIZABLE's 563; `row` 1,106 and `cell` 1,701, so cell tracking gains 54%, and deltas with FOR UPDATE queueing give the rest (`BENCHMARKS.md`). |
| Incomplete | Gate | Phase 5 gate | Missing: every row above complete. |

## Phase 6: Durability and the durable benchmark

Goal:
`celltnx` loses no acknowledged commit across crashes, and is benchmarked against InnoDB with an fsync per commit.

Scope:
- `store/lionturtle` implements the `Store` contract (0D): one `lt_write` per flush, holding every queued commit's row images and the newest commit timestamp; the writer calls `lt_sync` once per flush, and any status other than `LT_OK` stops the store for good. Auto-increment counters resume past the largest loaded value. `bench/build-mysql.sh` builds `lionturtle-c` at the commit pinned in `store/lionturtle/fetch.sh` with cargo and links it into the plugin.
- The engine keeps its store under `<datadir>/celltnx` and runs the writer on a background thread. `celltnx_sync` chooses whether a commit waits for its sync, and `celltnx_commit_delay_us` delays each flush; `celltnx_group_wait_us` lets the writer wait after each sync for the clients it acknowledged, no longer than that sync took.
- Recovery: plugin init opens the store and restores `lastCommitTs`. Each table loads its rows and rebuilds its indexes on first open, from the `TABLE_SHARE`'s definitions, before any session can reach it.
- DDL: create, drop and truncate persist and sync before returning, keyed by the table's data-dictionary object ID, which RENAME keeps. Without atomic DDL, a crash between the data dictionary's commit and the engine's leaves an empty table or unreachable rows; `DESIGN.md` documents this.
- Crash tests: a store-level kill test in `tests/store_test.cc`, building on `store/lionturtle/check/usecase.cc`, and a `mysqld` kill test that runs TPC-C, kills the server, restarts it, and compares the tables with a serial replay of acknowledged commits, ordered by `Celltnx_last_commit_ts`.
- Durable benchmark: rerun Phase 5's matrix with `innodb_flush_log_at_trx_commit=1` against `celltnx` in sync mode on the same disk, at 32 and 128 clients, with the writer's default group wait.

Out of scope:
- Atomic DDL, online backup and replication.

Completion gate:
Both crash tests pass 8 rounds on one store, half of them with a commit delay; the simulator sweep, MTR suite and wire oracle pass with the store attached; `BENCHMARKS.md` contains the durable tables.

Testing plan:
- Store-level and `mysqld`-level `kill -9` tests.
- A durable restart MTR test covering rows, indexes, CHECKs and auto-increment counters.
- The 30,000-seed simulator sweep after any core change this phase makes.

Status ledger:

| Status | Type | Item | Evidence / Gap |
| --- | --- | --- | --- |
| Complete | Work | 6A: lionturtle store | `store/lionturtle/store.{h,cc}`: one `lt_write` per flush with the newest commit timestamp, rows in `EncodeRow`'s image. `store_test`: `RowEncoding`, `DurableTables`, and `Crash`, 8 `kill -9` rounds of TPC-C on one store, half with a 500 µs commit delay, each recovering every acknowledged commit and replaying serially to the recovered state. |
| Complete | Work | 6B: Engine writer thread and durability variables | Writer thread, `celltnx_durable`, `celltnx_sync` and `celltnx_commit_delay_us` in `engine/ha_celltnx.cc`; the MTR suite runs with the store attached, and `t/status.test` checks the variables. |
| Complete | Work | 6C: Lazy per-table recovery | `DB::LoadRows` and `ha_celltnx::open`; `t/durable.test` restarts twice and checks rows, unique and secondary indexes, CHECKs, auto-increment, BLOBs and keyless tables. |
| Complete | Work | 6D: Durable DDL | Tables keyed by data-dictionary object ID, which MySQL assigns before an engine without atomic DDL creates the table, so RENAME needs no store change; `t/durable.test` covers RENAME, TRUNCATE, and DROP followed by CREATE across restarts. |
| Incomplete | Work | 6E: `mysqld` kill test | Missing: harness and 8 passing rounds. |
| Complete | Work | 6F: Durable benchmark | `bench/results/i7i/durable.csv` and `durable-clients.csv` on an EC2 i7i.12xlarge (instance-store NVMe, 18 µs `fdatasync`), and `bench/results/desktop/durable.csv` on a drive without power-loss protection; tables in `BENCHMARKS.md`. At 128 clients on the i7i: hot account 75,102 transfers/s against InnoDB SERIALIZABLE's 1,856; TPC-C at 1 warehouse 6,482 commits/s against 842. |
| Complete | Work | 6G: Group wait bounded by flush time | `gather` in `engine/ha_celltnx.cc` waits no longer than the previous flush took; `LionturtleStore.GatherSeesFlushTime` in `tests/store_test.cc`; `ci.sh` passes. On the i7i the full 500 µs wait cost up to 20% at 128 clients (`bench/results/i7i/group-wait.csv`, `BENCHMARKS.md`). |
| Incomplete | Work | 6H: Durable commit latency | At 8 clients each commit sleeps about 150 µs through two thread handoffs, and InnoDB commits 1.6× faster (`bench/results/i7i/profile/`). Missing: committing threads flushing their own group, or precise short waits, measured on the i7i. |
| Complete | Doc | `DESIGN.md` covers durability, recovery and DDL crash behavior | `DESIGN.md`, Durability; Durable tables and DDL. |
| Incomplete | Test | Simulator sweep, MTR and wire oracle with the store attached | Missing: run logs. |
| Incomplete | Gate | Phase 6 gate | Missing: every row above complete. |
