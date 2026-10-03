# Cell-level serializable OCC: prototype spec

## Goal
Test whether SERIALIZABLE transactions get more throughput under contention when conflicts are tracked per **(row, column)** instead of per row, combined with commit-time row merging and commutative deltas.

The primary workload is TPC-C NewOrder and Payment with few warehouses. Their hot warehouse, district and customer rows see frequent conflicts on disjoint columns, and Payment's same-column updates commute.

Deliverables:
- a Go transaction layer with secondary indexes and Badger persistence;
- a serializability checker for the transaction layer, and an end-to-end SQL oracle;
- a deterministic simulator in the style of TigerBeetle's VOPR: seeded, single-goroutine runs with fault injection, crashes and recovery;
- benchmark harnesses: in-process, and identical SQL against MySQL;
- a MySQL-compatible server built on go-mysql-server.

## Core idea
- Two transactions touching the same row conflict only when one **writes a column the other read**. Blind writes to the same cell never conflict with each other; the later commit's value wins.
- Storage stays **one record per row**. At commit, each written row is **merged**: take the newest committed version, lay this transaction's written columns over it, then apply its deltas.
- `col = col ± k` is recorded as a **delta** (a write with no read) as long as the transaction doesn't otherwise read `col`. Concurrent deltas on the same cell all commit.
- Transactions execute fully in parallel. Only the commit step (validate, merge, install) runs under **one global commit lock**, held for microseconds. This is not SQLite's model, which holds its write lock for the whole transaction.

## Non-goals (prototype)
- Distribution.
- DDL inside transactions; ALTER TABLE, DROP INDEX and RENAME INDEX.
- Foreign keys and triggers. Any statement involving them reads and writes whole rows.
- Prefix, full-text, spatial and vector indexes, and JSON key columns.
- More than 63 columns per table.
- Non-integer deltas.
- Evicting rows from memory: every row stays resident, and Badger holds the durable copy.

## Layout (Go)
- `keys/`: order-preserving, prefix-free key encodings
- `txn/`: transactions, the multiversion store, secondary indexes, the commit protocol, the durability queue and recovery
- `store/badger`: the Badger store and its `kill -9` test
- `check/`: operation recorder and serializability checker for the transaction layer
- `workload/`: the `random`, `users` and `tpcc` workloads, shared by the bench, the stress tests and the simulator. `random` issues random transactions over a small key space using every API call, scans, index scans, inserts and deletes included.
- `sim/`, `cmd/sim`: deterministic simulator (scheduler, simulated disk, crashes, invariants), the SQL simulation and its oracle, seed runner, sweeps and soak
- `sqlgms/`, `cmd/server`: go-mysql-server backend and MySQL-protocol server
- `cmd/bench`, `bench/sweep.sh`: in-process benchmark and contention sweep
- `cmd/sqlbench`, `bench/compare.sh`: SQL benchmark and the MySQL comparison

## Data model
- **Table:** columns 0..62. `type ColMask uint64`. Bit 63 is reserved as **EXISTS** (row existence).
  - Each CHECK constraint declares the columns it references and a predicate over the row.
  - Each secondary index declares its columns, whether it is unique, and a key function.
- **Row key:** a byte string whose byte order is the key order.
  - `keys` encodes NULL before every value, integers sign-flipped big-endian, floats with their sign bit folded, and strings escaped and terminated, so concatenated components sort component by component.
  - The SQL layer encodes strings by their collation's rune weights, so collation equality is key equality, and decimals by sign, decimal exponent and digits.
- **Row value:** the full row. Values are `nil`, `int64`, `uint64`, `float64` or `string`. The SQL layer stores times as UTC microseconds and decimals and JSON as text.
- **In-memory row entry:** a version chain of `(commitTs, writeMask, image | tombstone)`, newest first.
  - `writeMask` holds the columns that commit wrote, deltas included.
  - Inserts and deletes write every column plus EXISTS.
- **Secondary index:** a hidden versioned table. An entry's key is the index key, followed by the row's primary key unless the index is unique and the key has no NULL; its value is the primary key.
  - Entries are derived at commit from the old and merged row images, so they need no read of the indexed columns.
  - Within one commit, an entry both vacated and claimed ends up claimed.
- **Buckets:** `bucketOf(key)` takes a table's first `BucketPrefix` key bytes, zero-padded, and clears the low `BucketBits` bits. It is monotone, so a key range maps to a contiguous bucket range.
  - `BucketPrefix` defaults to 8, which makes `bucketOf` equal to `pk >> BucketBits` for 8-byte keys; the SQL layer uses the width of the leading fixed-width key columns, so composite keys split on their last integer column.
  - Each bucket records `lastTs`, the commitTs of the latest insert or delete in its range, in a per-table ordered map only the committer touches.
  - A scan records the buckets its range overlaps; validation requires `lastTs <= readTs` for each. Comparing against a value read at scan time would miss inserts committed between `readTs` and the scan, because the scan reads the older snapshot.
  - Index key spaces have their own buckets; deriving an index entry counts as an insert or delete there.
- **Auto-increment:** values come from a per-table atomic counter outside transactions, as in InnoDB's interleaved mode. Aborted transactions leave gaps.
- **Row index:** a skiplist per table. The committer is its only writer; readers traverse it without locks. Node heights come from a hash of the key, so the structure is deterministic. Nodes are never removed, so transactions cache node pointers.
- **Catalog:** tables, indexes and CHECKs are created outside transactions, under the commit lock. Creating an index builds it from the latest rows and fails on a duplicate. The table list is replaced copy-on-write.
- Readers never take the commit lock.

## Transaction API (core)
```go
db.Begin() *Txn                                         // readTs = lastCommitTs
t.Get(tbl, pk, cols) (row, found, err)                  // tracked read of cols (+EXISTS)
t.GetUntracked(tbl, pk, cols)                           // snapshot read, not recorded
t.GetRow(tbl, pk, track)                                // whole row; tracks only track (+EXISTS)
t.Scan(tbl, lo, hi, pred, proj, match) ([]Rec, error)   // [lo, hi); match sees only pred columns
t.IndexScan(tbl, idx, lo, hi, pred, proj, match)        // entries in [lo, hi), in entry order
t.ScanRows(tbl, idx, lo, hi, track)                     // whole rows; idx < 0 is the primary key
t.Set(tbl, pk, cols, vals) error                        // ErrNotFound, DuplicateKeyError
t.Add(tbl, pk, col, delta int64) error                  // commutative delta
t.Insert(tbl, pk, row) error                            // DuplicateKeyError
t.Delete(tbl, pk) error                                 // ErrNotFound
t.Savepoint() int; t.RollbackTo(sp)                     // statement-level rollback
t.Commit() error                                        // Install, then WaitDurable
t.Install() error                                       // ConflictError | ConstraintError
t.WaitDurable() error
t.Abort()
```
`GetRow` and `ScanRows` return untracked columns too. The caller must not let them influence anything it writes or returns; the SQL layer uses them for the columns a statement never reads.

Semantics:
- **Snapshot:** reads see the snapshot at `readTs`, overlaid with the transaction's own writes.
- **Existence:** `Set`, `Add`, `Insert` and `Delete` each record a read of EXISTS on their row.
  - A concurrent insert or delete of that row therefore fails validation, and merge always applies updates to a live row.
  - `Set`, `Add` and `Delete` return `ErrNotFound` when the row is missing from the transaction's view. `Insert` returns a `DuplicateKeyError` when it's present.
- **Missing row:** reading a missing row records an **absent-key read**, i.e. a read of EXISTS on that key. Validation looks the key up, so only a commit of that exact key conflicts.
- **Unique indexes:** a write that sets a unique-indexed column, and every insert, checks the transaction's view of the index. It records a read of the claimed entry, present or absent, so a concurrent claim of the same key conflicts. A `DuplicateKeyError` names the index and the row that holds the key.
- **Deltas:** in `cell+delta` mode, `Add` records only its EXISTS read. In `row` and `cell` modes it also reads its column, which makes it equivalent to `Get`+`Set`. Indexed columns never commute: `Add` on one reads it, so the new index key is known.
- **Reading a delta'd column:** reading a column that has a pending delta records a read of that column, so the delta no longer commutes.
- **Superseded deltas:** a `Set` of a column with a pending delta, or a `Delete` of a row with pending deltas, records a read of those columns. Statements that checked the delta's intermediate value then rest on a validated base.
- **Scan records:**
  - every bucket that overlaps `[lo, hi)`;
  - `pred + EXISTS` for every row visited, plus the indexed columns for an index scan;
  - also `proj` for each row `match` accepts.
- **Savepoints:** `RollbackTo` discards writes made after the savepoint and keeps every recorded read, since the client may have seen those results.
- **Mode** (DB-level flag):
  - `row`: every mask is widened to all columns, and deltas become read+write. This is the baseline.
  - `cell`
  - `cell+delta`

## Commit protocol
**Read-only transaction:** commit immediately. It serializes at `readTs`. With a store in sync mode, the COMMIT returns once `durableTs >= readTs`.

**Otherwise:**
- **Before taking the lock,** deduplicate the write set. Reads and writes cache each row's skiplist node, so validation, merge and install search the skiplist only for keys that were absent.
- **Inside the global commit lock:**
  1. **Timestamp.** `commitTs = lastCommitTs + 1`.
  2. **Validate.** On any failure, abort with the cause.
     - Each recorded read `(row, mask)`: no version of the row with `commitTs > readTs` has a `writeMask` that intersects `mask`. Validation finds the row by key, so absent-key reads see later inserts.
     - Each recorded bucket range: every `lastTs <= readTs`.
  3. **Merge.** For every written row:
     - an insert's image is the inserted row, and a delete's is a tombstone;
     - an update starts from the newest committed version, lays the `Set` columns over it, then applies deltas;
     - evaluate, on the merged image, every CHECK that references a written column. Every other CHECK still holds, because the newest committed version satisfies it;
     - derive index entries from the old and merged images.

     If any check fails, a delta overflows or hits a non-integer, return a `ConstraintError` naming the commit it saw. Nothing is installed until every row passes.
  4. **Hook.** Run `Txn.OnInstall(commitTs)`, so a recorder logs the commit before anything can persist it.
  5. **Enqueue.** With a store, hand the base-table writes to the writer queue, in commit order.
  6. **Install.** Prepend `(commitTs, writeMask, image)` to each row's and entry's chain and trim it (see GC). Set the bucket `lastTs` for inserts, deletes and entry changes.
  7. **Publish.** Atomically store `lastCommitTs = commitTs`, then release the lock.
- **Acknowledge.** With a store in sync mode, the COMMIT returns once `durableTs >= commitTs`.

- **Snapshots:** new transactions take `readTs = lastCommitTs` (an atomic load). Commits install in timestamp order under the lock, so this is always a complete, consistent snapshot.
- **Exact validation:** commits install in timestamp order, so validation sees every version committed between `readTs` and `commitTs`.
- **GC:** install trims each written chain to every version newer than the oldest active `readTs`, plus the newest one at or below it. Validation needs the write masks of the newer versions even after newer snapshots supersede them. A row that stops being written keeps its last few versions until its next write.
- **Active snapshots:** `Begin` loads `lastCommitTs` and registers its `readTs` under one mutex, which GC also takes to compute the oldest `readTs`. GC therefore never trims a version a starting transaction needs.

## Durability
- **Writer:** the host runs `RunWriter` on a goroutine; the simulator runs it as a coroutine. It takes every queued commit, writes the batch, syncs it, and advances `durableTs` to the batch's newest commit (group commit). With a commit delay, it first sleeps that long after finding work, so commits arriving meanwhile join the batch; this trades up to the delay in commit latency for fewer syncs. The host supplies the sleep, so the core stays free of wall-clock time. A write or sync error is permanent: the DB stops acknowledging and the process must restart and recover.
- **Store contract:** a crash may lose a suffix of the commits written since the last sync, but never part of a commit or a commit before a surviving one.
- **Badger:** each commit is one managed transaction committed with `CommitAt(commitTs)`, submitted in order with asynchronous callbacks; `Sync` follows, then `SetDiscardTs`. Badger applies a transaction atomically and writes its log in submission order, which gives the contract; the `kill -9` test checks it.
- **Recovery:** `Recover` loads the newest image of every row as of the store's last commit, rebuilds index entries from the rows, and resumes auto-increment counters past their largest values. Every transaction begins after recovery, so no earlier history is needed.
- **Catalog:** the server logs each successful DDL statement, with its current database, to `catalog.jsonl` and syncs it before acknowledging; startup replays the log before `Recover`.

## Deterministic simulation testing
The simulator (`cmd/sim`) runs the whole system on one goroutine from a single seed. It injects faults, checks invariants as it goes, and reproduces any failure exactly from its seed.

**Determinism rules.** These apply to `txn/`, `check/` and `workload/`:
- Randomness comes from a seeded `rand.Rand` per client; waiting and yielding go through `Options.Wait` and `Options.Yield`, which the simulator wires to its scheduler.
- Core code starts no goroutines and never calls `time.Now`, `time.Sleep` or the global `math/rand`. `sim/rules_test.go` rejects `go` statements and imports of `time`, `math/rand` and `os` there.
- Iteration order never depends on Go map order: iterate slices or ordered structures.
- Every wait goes through `Options.Wait`: production blocks on a condition variable, the simulator suspends the coroutine until the condition holds. The commit lock is the one exception: nothing yields while holding it, so it's never contended in simulation.
- `store/badger` is exempt, because the simulator replaces it with `sim/disk`.

**Scheduling:**
- Each client, the loader and the durability writer run as `iter.Pull` coroutines, which switch deterministically. The writer is a daemon: the run ends when the clients do.
- Workloads are straight-line Go against a client interface. The bench runs each client on a goroutine; the simulator runs each as a coroutine that yields before every operation.
- The scheduler picks the next runnable coroutine from the seeded PRNG: neither sleeping nor blocked on a false condition. With nothing runnable and nothing asleep, the run fails as a deadlock. Skewed seeds give clients unequal weights, so slow clients hold old snapshots across many commits.
- Transactions read snapshots, so a transaction's results depend only on the order of begins, commits and writer steps. Yielding at operation boundaries covers those orders.
- The store calls `Options.Yield` wherever another goroutine can observe intermediate state. `Scan` and `IndexScan` yield between rows, so commits land mid-scan.

**Swarm parameters.** Each seed draws its own:
- mode, workload, transaction mix and secondary indexes;
- client count, quota, key-space size, bucket width and θ;
- think time, scheduler skew and the share of long-running readers;
- whether a simulated disk is attached, sync or not, its sync latency, the writer's commit delay, and the probabilities of crashes and sync failures. The commit delay comes from its own random stream, so a sweep with `-commitdelay 0` reproduces every other parameter.

Small key spaces and narrow buckets force contention.

**Faults:**
- clients abort, roll back to savepoints, or abandon a transaction mid-flight;
- long-running snapshots hold back GC;
- deltas push values across CHECK boundaries, and unique keys collide;
- crashes at any step, including during group commit;
- lost unsynced commits, slow syncs, and sync failures, which crash the process.

A crash stops every coroutine, keeps the disk's synced commits plus a random prefix of the unsynced ones, opens a new DB, recovers it, checks it against the history, and restarts the writer and the unfinished clients.

**Invariants:**
- **Serializability:** the checker replays every run.
- **Workload:** a read-only audit transaction verifies TPC-C consistency conditions 1–4 at random points.
- **Internal, every 1,000 steps and at the end:**
  - every skiplist level is sorted;
  - version chains strictly decrease in `commitTs`, and none is past `lastCommitTs`;
  - no bucket has `lastTs` past `lastCommitTs`;
  - every secondary index holds exactly one entry per live row, pointing at it, and unique indexes hold no duplicate;
  - active snapshots are sorted, and `durableTs <= lastCommitTs`.
- **Durability:**
  - after each recovery, the recovered state equals the replay of every commit at or below the recovered `lastCommitTs`;
  - in sync mode, that timestamp is at least every acknowledged `commitTs` and every acknowledged read-only `readTs`;
  - the checker drops logged commits above it.
- **Liveness:** every client finishes its quota of transactions within 5 million steps.

**Reproduction:**
- A failure prints its seed and parameters. `cmd/sim -seed N [-workload sql]` replays it exactly.
- Each run hashes every operation's outcome. Sweeps rerun every tenth seed and fail on any mismatch, which catches nondeterminism.

**Self-test.** The `simbugs` build tag enables deliberate bugs, and the simulator must find each one within 30,000 seeds (`go test -tags simbugs -run SelfTest ./sim`, or `sim -bug NAME`). Internal invariants are off for these runs, so the checker, the workload invariants and the durability check must catch each bug:
- validation skips scanned buckets;
- `Set` omits its EXISTS read;
- GC ignores active snapshots;
- merge skips the CHECK on delta-only rows;
- a `Get` of a missing row records nothing;
- reading a delta'd column records no read;
- a unique-key claim records no read of the entry;
- an index scan records no buckets;
- a commit is acknowledged before its sync;
- a read-only commit skips its durability wait;
- a flush acknowledges commits that joined the queue during its sync.

**SQL simulation.** `sim -workload sql` runs random SQL transactions through go-mysql-server and the store, with the clients as coroutines and no wire protocol.
- Statements cover point, range and secondary-index reads, aggregates, joins, a correlated subquery, deltas over one and many rows, blind and indexed writes, unique-key collisions, `INSERT … ON DUPLICATE KEY UPDATE`, `REPLACE`, single and range deletes, explicit and autocommit transactions, and rollbacks.
- **Oracle:** every committed transaction's statements are rerun, one transaction at a time in serialization order, on go-mysql-server's in-memory reference engine. Every result (rows, matched or affected counts, error classes) and both final tables must match. UPDATE results compare matched rows.
- A transaction the store rejected at commit must hit a CHECK error when run serially after the writers before it, on a reference rebuilt from them: the reference engine's ROLLBACK leaves its secondary indexes stale.
- Deltas in one transaction share a sign, so a transaction whose final rows pass its CHECKs passes them after every statement, as the reference engine requires.
- go-mysql-server initializes package globals when an engine is built, so `sqlgms` builds engines under a lock; parallel sweeps are race-free under `-race`.

**CI and soak:**
- `go test ./...` runs core seeds 0–999 and SQL seeds 0–299.
- `cmd/sim -seeds 30000` sweeps in parallel; `-workload`, `-mode` and `-durable` pin those parameters.
- `cmd/sim -soak` runs random seeds until interrupted and reports each failing seed with its parameters.

**Coverage boundary:**
- The simulator explores interleavings at operation boundaries and `Options.Yield` points. Memory-level races are covered by the `-race` stress tests.
- The simulator models Badger's crash behavior; the `kill -9` test checks that model against Badger itself.

## Phase 1: core, in-memory store, checker, simulator, bench
Use Go 1.26 and only the standard library.

**Tests.** Scenarios 1–6 and 9 interleave transactions in a fixed order on one goroutine; 7 and 8 run on real goroutines. Everything passes under `-race`:
1. T1 does `Get(A.email)` then `Set(A.name)`. T2 sets `A.wallet` and commits first. Both commit, and A keeps both changes.
2. T2 reads `A.email`; T1 writes `A.email` and commits first. T2 gets `ErrConflict`.
3. Write skew: two transactions each read `A.wallet` and `B.wallet`, and each writes one of them. One aborts.
4. Phantom:
   - T1 scans a range and writes; T2 inserts into that range and commits first. T1 aborts.
   - If T2 inserts into a different bucket instead, T1 commits.
5. Absent keys:
   - T1 gets missing key K and writes; T2 inserts K and commits first. T1 aborts.
   - If T2 inserts a different key in K's bucket instead, T1 commits.
   - 100 concurrent inserts of distinct keys in one bucket all commit.
   - Of two concurrent inserts of the same key, one commits and the other gets `ErrConflict`.
6. T1 sets `A.email`; T2 deletes A and commits first. T1 gets `ErrConflict`.
7. 100 concurrent `Add(A.wallet, -1)`: all commit, and the final value is the initial value minus 100.
8. With `CHECK wallet >= 0` and wallet = 5, run 10 concurrent `Add(-1)`. Exactly 5 commit; 5 fail with `ErrConstraint`.
9. `row` mode: scenario 1 aborts one transaction.
10. Stress tests run every workload on real goroutines and pass the checker in all three modes.
11. Secondary indexes: unique claims, concurrent claims, phantoms in index ranges, rows moving into a range, and indexed deltas.
12. The simulator passes 30,000-seed sweeps with every workload in all three modes, with and without a disk, passes the determinism reruns, and finds every self-test bug.

**Checker:**
- **Workloads:** write unique values, so comparing values is meaningful.
- **Log:** for each committed transaction, record its serialization point, the values it read and the values it wrote, and whether its commit was acknowledged.
  - Reads include not-found results and the complete result set of every scan and index scan.
  - Writers serialize at `commitTs`.
  - Read-only transactions serialize at `readTs`, after every writer at or below it.
- **Replay:** run the log single-threaded in that order against an independent model, which enforces unique indexes itself. Every logged read and write outcome must match the model, and the final states must match.
- **Also verified:**
  - writer timestamps run from 1 to `lastCommitTs` without gaps;
  - every committed row passes its CHECKs;
  - every constraint rejection violates a CHECK or overflows when replayed at the state it saw;
  - a read-only transaction changes nothing.

**Bench (`cmd/bench`):**
- **TPC-C workload:** NewOrder and Payment, 1–8 warehouses. Like TPC-C, 60% of Payments find the customer by last name through a secondary index. Payment's YTD, balance and payment-count updates and NewOrder's stock counters are `Add`s, so the mode alone decides whether they commute.
  - Expected conflicts: in `row` mode, NewOrder and Payment conflict on warehouse, district and customer rows. `cell` mode removes those conflicts, and `cell+delta` also removes Payment–Payment conflicts. NewOrder–NewOrder conflicts on `D_NEXT_O_ID` remain in every mode.
- **Users workload:** `users(id, name, email, wallet)` with N rows picked by Zipf(θ); pay and deposit (`Add(wallet, ∓x)`), profile (`Set email`), audit (`Get email`, insert into `audit`), same-column (`Get`+`Set wallet`) and read-only lookups. The mix sets the fraction of same-row collisions that touch disjoint columns.
- **Settings:** think time, clients, duration, mode, bucket bits; `-check` records the history and runs the checker after the run.
- **Output:** commits/s, conflicts by cause, retries per transaction, p50/p99 latency, commit-lock wait and hold p99; CSV via `bench/sweep.sh`.

**Decision gate:** proceed to Phase 2 only if `cell` or `cell+delta` clearly beats `row` on TPC-C with few warehouses. Report how much of the gain comes from cell granularity and how much from deltas.

## Phase 2: go-mysql-server front end
**Storage interfaces:** `Provider` (`MutableDatabaseProvider`), `Database` (`TableCreator`, `TableDropper`) and `Table`, which implements `InsertableTable`, `UpdatableTable`, `DeletableTable`, `ReplaceableTable`, `IndexAddressableTable`, `IndexedTable`, `IndexAlterableTable`, `CheckTable`, `CheckAlterableTable` and `AutoIncrementTable`.
- Keyless tables get a hidden counter key; an update or delete finds the stored row by value.
- `PartitionRows` reads eagerly, so a statement never reads its own later writes.
- Index lookups: each range becomes a key interval, from its prefix of point columns and the bounds of the next column, plus an exact range filter, since `PreciseMatch` drops the engine's own filter. A point on the whole primary key becomes a `GetRow`, which records an absent-key read rather than a bucket.
- Indexes return rows in index order and declare ascending, reversible order.

**Sessions:** `Session` embeds `sql.BaseSession` and implements `TransactionSession` and `LifecycleAwareSession`.
- `StartTransaction` begins a core transaction; `CommitTransaction` commits it and, on failure, detaches it, which the engine does not.
- `CommandEnd` aborts an autocommit transaction a failed statement left attached.
- Named savepoints map to core savepoints.
- Each editor's `StatementBegin` opens a core savepoint; `DiscardChanges` rolls back to it.

**Plan annotation.** The engine skips post-analyze rules for single-table INSERT VALUES, UPDATE and DELETE, so `sqlgms` wraps the exec builder instead, which sees every final plan and every subquery plan. For each table node of ours it computes:
- **Read columns:** every column any expression in the plan references through that node, including inside subqueries, except assignment targets.
- **Write columns:** UPDATE and ON DUPLICATE KEY UPDATE targets.
- **Delta columns:** `SET c = c ± <literal|param>` where `c` is an integer column the statement reads nowhere else, outside every key and index.

It replaces the node's table with a view carrying these sets. Assigned non-delta columns also count as read: the engine skips writing a row whose new image equals the old one, so the old values of assigned columns decide whether the write happens. A plan with UPDATE JOIN, triggers, foreign-key handlers or LOAD DATA, or a table with generated columns, stays unannotated: every column is read and written.

**Backend behavior:**
- Reads track the view's read columns and return the rest untracked; index lookups also track the index columns.
- `Update(old, new)` writes the assigned columns, the columns the engine changed (such as ON UPDATE timestamps) and nothing else; a delta column becomes `Add(new - old)`, which equals the statement's constant whatever the untracked old value was. A changed primary key becomes a delete and an insert.
- A duplicate key returns `sql.NewUniqueKeyErr` with the existing row, read in full and tracked, which ON DUPLICATE KEY UPDATE and REPLACE use.

**CHECK constraints:** the exec builder compiles the plan's CHECKs into statement-time checks and the core's commit-time checks, then strips them from the plan.
- A statement evaluates each CHECK over a column it writes. If every column the CHECK references is final (written outright or tracked), the result stands.
- Otherwise the CHECK is evaluated on the transaction's view: a failure reads the CHECK's columns and fails the statement, which commit validation then confirms; a pass leaves the merged row to the commit-time check.

**Errors:**
- `ConflictError` becomes `sql.ErrLockDeadlock` (MySQL 1213, which clients retry).
- `ConstraintError` at COMMIT becomes a CHECK violation.
- `DuplicateKeyError` becomes a primary- or unique-key violation (MySQL 1062) at the statement.

**Server:** `sqlgms.Serve` starts go-mysql-server's MySQL-protocol server over the store, in memory or durable under a data directory.

**Acceptance:**
- the Phase 1 scenarios pass as SQL (`sqlgms/sql_test.go`), and a durable server restarts with its catalog, rows, indexes, CHECKs and auto-increment counters (`server_test.go`);
- the SQL simulation passes 30,000-seed sweeps through the oracle, with determinism reruns;
- go-mysql-server's engine tests run through a `Harness` (`CELLTNX_ENGINETEST=1 go test -run TestEngine ./sqlgms`); `bench/enginetest.sh` records the failing cases.

## Phase 3: comparison with MySQL
- **Client:** `cmd/sqlbench` sends identical SQL to both systems through `go-sql-driver/mysql` with client-side parameter interpolation, and retries on error 1213 or 1205. CHECK violations count as business outcomes.
- **Reads before writes** use `SELECT … FOR UPDATE`, as TPC-C implementations on MySQL do; cell-tnx treats it as a plain tracked read.
- **MySQL setup:** MySQL 8.0 in Docker on the host network, at SERIALIZABLE (the fair baseline) and REPEATABLE READ (for reference). Binlog off.
- **Durability:** in-memory cell-tnx against `innodb_flush_log_at_trx_commit=0`, then Badger in sync mode against `innodb_flush_log_at_trx_commit=1`.
- **Workloads:** TPC-C at 1 and 4 warehouses, TPC-C with 1 ms think time between statements, and the users workload at θ = 0.5, 0.9 and 0.99 and at disjoint-column shares of 0, 0.5 and 0.9.
- **Report:** throughput, retries per transaction, and p50/p99 latency for five configurations: MySQL SERIALIZABLE, MySQL REPEATABLE READ, and cell-tnx in `row`, `cell` and `cell+delta` modes. `bench/compare.sh` runs the matrix.
- **Attribution:** go-mysql-server adds per-statement cost that MySQL lacks. The Phase 1 in-process results separate the gap between modes from the gap between engines.

## Phase 4: Badger persistence
Built as described under Durability. Every row stays in memory; Badger is the durable copy.
- **Simulation:** `sim/disk` implements the store contract, with atomic per-commit writes, loss of a random suffix of unsynced commits on crash, sync latency, and sync failures that crash the process.
- **Acceptance:**
  - the simulator passes 30,000-seed sweeps with crashes and finds the durability self-test bugs;
  - in sync mode, `kill -9` during TPC-C loses no acknowledged commit, and the recovered state equals the serial replay of every surviving commit, over several kill and recover rounds on one store (`store/badger/crash_test.go`);
  - the Phase 3 comparison reruns with fsync on both sides.

## Known semantic differences
- Conflicts surface at COMMIT; clients must retry the whole transaction. Concurrent inserts of the same key surface as a conflict, and the retry reports the duplicate.
- A CHECK over a delta column that passes on the transaction's view but fails on the merged row fails at COMMIT, not at the statement. Deltas of mixed sign on one row in one transaction are checked only at their final value.
- An UPDATE reads the columns it assigns, so two concurrent UPDATEs assigning the same cell conflict; deltas still commute.
- `SELECT … FOR UPDATE` takes no lock.
- go-mysql-server's default collation is binary; case-insensitive comparisons need an explicit collation.

## Later (not in scope now)
- Replace the global commit lock with Silo-style per-row latches if the lock-wait metrics show it limiting throughput on many cores. Commits would then install out of timestamp order, so snapshots need a visibility watermark in place of `readTs = lastCommitTs`, and validation must follow Silo's protocol.
- Evict cold rows to Badger, reading them back at `readTs`, with a loading path that respects the single-writer skiplist.
- ALTER TABLE, DROP INDEX, and foreign keys.

## References
- **Silo:** Tu et al., *Speedy Transactions in Multicore In-Memory Databases*, SOSP 2013. Version-based phantom protection, and the per-row-latch commit protocol for later.
- **HyPer MVCC:** Neumann, Mühlbauer, Kemper, *Fast Serializable MVCC for Main-Memory Database Systems*, SIGMOD 2015. Attribute-level validation against recently committed versions.
- **Escrow:** O'Neil, *The Escrow Transactional Method*, TODS 1986. Deltas.
- **TPC-C:** *TPC Benchmark C Standard Specification*, revision 5.11. NewOrder and Payment, and consistency conditions 1–4.
- **VOPR:** TigerBeetle's deterministic simulator. Seeded single-threaded runs, fault injection, and per-seed randomized parameters.
- **FoundationDB:** Zhou et al., *FoundationDB: A Distributed Unbundled Transactional Key Value Store*, SIGMOD 2021. Deterministic simulation and determinism checks by rerunning seeds.
- **Swarm testing:** Groce et al., *Swarm Testing*, ISSTA 2012. Per-seed feature and fault selection.
- **fjall:** `src/tx/optimistic/conflict_manager.rs`. Range-read conflict checks.
- **go-mysql-server:** `sql/tables.go`, `sql/session.go`, `sql/analyzer/`, `sql/rowexec/`.
