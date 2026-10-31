# celltnx design

celltnx is a MySQL 8.4 storage engine whose transactions are always serializable and track conflicts per **(row, column)**. Its tables sit beside InnoDB tables in the same server. Rows live in memory, in a skiplist per table with a version chain per row, and a lionturtle store under the data directory holds the durable copy.

The code has three layers:

- `core/` is the transaction layer. It uses only the C++ standard library and reaches its host through hooks in `Options`.
- `engine/` is the MySQL handler. It translates MySQL's rows, keys, statements and errors into core calls, and every concurrency decision stays in the core.
- `store/lionturtle/` persists commits.

`check/`, `workload/` and `sim/` test the core deterministically.

## Core idea

- **Cells, not rows.** Two transactions that touch the same row conflict only when one writes a column the other read. Blind writes to the same cell never conflict with each other, and the later commit's value wins.
- **One record per row.** At commit, each written row is merged: the commit takes the newest committed version, lays the transaction's written columns over it, then applies its deltas.
- **Deltas commute.** `col = col ± k` is recorded as a delta, a write with no read, as long as the transaction reads `col` nowhere else. Concurrent deltas on the same cell all commit.
- **Short commits.** Transactions execute in parallel without locks. Only the commit step (validate, merge, install) runs under one global commit lock, held for microseconds.

The target workload is TPC-C NewOrder and Payment with few warehouses. Their hot warehouse, district and customer rows see frequent conflicts on disjoint columns, and Payment's updates to the same columns commute.

## Data model

- **Columns.** A table has up to 63 columns. A `ColMask` is a `uint64_t` with one bit per column; bit 63 is `kExists`, the row's existence.
- **Values.** A value is NULL, `int64_t`, `uint64_t`, `double` or a byte string.
- **Row keys.** A primary key is a byte string whose byte order is the key order. `core/keys.h` encodes values order-preservingly and prefix-free, with NULL before every value, so concatenated components sort component by component.
- **Versions.** Each row holds a chain of `Version`s, newest first: a commit timestamp, the columns that commit wrote (deltas included) and the row image or a tombstone. Inserts and deletes write every column plus `kExists`. Each version also records where its run of same-mask versions began (`run_ts`), so validation checks a run of deltas once instead of walking it.
- **Row index.** Each table keeps a skiplist. The committer is its only writer, and readers traverse it without locks. A seek returns the node it compared, never a reloaded pointer, since the committer may have linked a smaller key in between. Node heights come from a hash of the key, so the structure is deterministic. Nodes are never removed, so transactions cache node pointers.
- **Secondary indexes.** Each index is a hidden versioned table. An entry's key is the index key, followed by the primary key unless the index is unique and the key holds no NULL, and its value is the primary key. A commit derives entries from the old and merged row images, so it needs no read of the indexed columns. Within one commit, an entry both vacated and claimed ends up claimed.
- **CHECK constraints.** Each `Check` names the columns it references and a predicate over the row.
- **Buckets.** `BucketOf(key)` takes a table's first `bucket_prefix` key bytes (8 by default), zero-padded, and clears the low `bucket_bits` bits. It is monotone, so a key range maps to a contiguous range of buckets.
  - Each bucket records the timestamp of the latest insert or delete in its range, in a per-table ordered map that only the committer writes.
  - A scan records the buckets its range overlaps. Validation requires each bucket's timestamp to be at or below the transaction's `readTs`. A value read at scan time would miss inserts committed between `readTs` and the scan, because the scan reads the older snapshot.
  - Index key spaces have their own buckets, and deriving an index entry counts as an insert or delete there.
- **Counters.** Auto-increment values and hidden keys come from per-table atomic counters outside transactions, as in InnoDB's interleaved mode. Aborted transactions leave gaps.
- **Catalog.** `CreateTable`, `CreateIndex`, `DropIndex`, `SetChecks` and `DropTable` run outside transactions, under the commit lock. `CreateIndex` builds the index from the latest rows and fails on a duplicate. The table list is replaced copy-on-write, so readers never take the commit lock.

## Transaction API

`core/db.h` declares the API:

```cpp
std::unique_ptr<Txn> tx = db.Begin();                // readTs = LastCommitTs()
tx->Get(tbl, pk, cols, &row, &found);                // tracked read of cols and kExists
tx->GetUntracked(tbl, pk, cols, &row, &found);       // snapshot read, not recorded
tx->GetRow(tbl, pk, track, &row, &found);            // whole row; tracks only track and kExists
tx->Scan(tbl, lo, hi, pred, proj, match, &out);      // [lo, hi); match sees only pred columns
tx->IndexScan(tbl, idx, lo, hi, pred, proj, match, &out);  // entries in [lo, hi), in entry order
tx->ScanRows(tbl, idx, lo, hi, track, &out);         // whole rows; idx < 0 is the primary key
tx->Set(tbl, pk, cols, vals);                        // kNotFound, kDuplicateKey
tx->Add(tbl, pk, col, delta);                        // commutative delta
tx->Insert(tbl, pk, row);                            // kDuplicateKey
tx->Delete(tbl, pk);                                 // kNotFound
tx->BeginStatement(); tx->EndStatement();            // reads see writes as of statement start
int sp = tx->Savepoint(); tx->RollbackTo(sp);        // statement and named savepoints
tx->Refresh();                                       // move the snapshot forward, validating reads
tx->Commit();                                        // Install, then WaitDurable
tx->Install();                                       // kConflict or kConstraint
tx->WaitDurable();
tx->Abort();
```

`GetRow` and `ScanRows` return untracked columns too. The caller must keep them from influencing anything it writes or returns; the engine uses them for columns a statement never reads.

Semantics:

- **Snapshot.** Reads see the snapshot at `readTs`, overlaid with the transaction's own writes. While a statement is open, reads see those writes as they stood when it began, so an `INSERT … SELECT` never reads its own output.
- **Existence.** `Set`, `Add`, `Insert` and `Delete` each record a read of `kExists` on their row. A concurrent insert or delete of that row therefore fails validation, and merge always applies updates to a live row. `Set`, `Add` and `Delete` return `kNotFound` when the row is missing from the transaction's view, and `Insert` returns `kDuplicateKey` when it is present.
- **Missing rows.** Reading a missing row records an absent-key read, a read of `kExists` on that key. Validation looks the key up, so only a commit of that exact key conflicts.
- **Unique indexes.** Every insert, and every write that sets a unique-indexed column, checks the transaction's view of the index. It records a read of the claimed entry, present or absent, so a concurrent claim of the same key conflicts. A duplicate names the index and the row that holds the key.
- **Deltas.** In `cell+delta` mode, `Add` records only its `kExists` read. In `row` and `cell` modes it also reads its column, which makes it equivalent to `Get` plus `Set`. Indexed columns never commute: `Add` on one reads it, so the new index key is known.
- **Reading a delta'd column** records a read of that column, so the delta no longer commutes.
- **Superseded deltas.** A `Set` of a column with a pending delta, or a `Delete` of a row with pending deltas, records a read of those columns. Statements that checked the delta's intermediate value then rest on a validated base.
- **Scans record** every bucket that overlaps `[lo, hi)`; the predicate columns and `kExists` for every row visited, plus the indexed columns for an index scan; and the projected columns for each row `match` accepts.
- **Savepoints.** `RollbackTo` discards writes made after the savepoint and keeps every recorded read, since the caller may have used those results.
- **Refresh.** `Refresh` validates every read so far against the commits since `readTs`, then moves the snapshot to the latest commit. A failed refresh keeps the snapshot and reports the conflict.
- **Modes** (`Options::mode`):
  - `row` widens every mask to all columns and turns deltas into a read plus a write. It is the baseline.
  - `cell` tracks reads per column.
  - `cell+delta` tracks reads per column and lets deltas commute.

## Commit protocol

**A read-only transaction** commits at once and serializes at its `readTs`. With a synced store, `Commit` returns once the store has synced `readTs`.

**A writing transaction** has a write set that is already deduplicated, one slot per row, and each slot caches the row's skiplist node. Validation, merge and install search the skiplist only for keys that were absent. Under the global commit lock, the commit:

1. **Takes a timestamp:** `commitTs = lastCommitTs + 1`.
2. **Validates.** It aborts with `kConflict` on any failure.
   - For each recorded read `(row, mask)`, no version of the row with `ts > readTs` has a mask that intersects `mask`. Validation finds the row by key, so absent-key reads see later inserts.
   - For each recorded bucket range, every bucket's timestamp is at or below `readTs`.
3. **Merges** every written row:
   - an insert's image is the inserted row, and a delete's is a tombstone;
   - an update starts from the newest committed version, lays the `Set` columns over it, then applies deltas;
   - every CHECK that references a written column is evaluated on the merged image. Every other CHECK still holds, because the newest committed version satisfies it;
   - index entries are derived from the old and merged images.

   A failed CHECK, an overflowing delta, a delta on a non-integer value, or a merged row over the store's size limit returns `kConstraint` naming the commit it saw. Nothing is installed until every row passes.
4. **Runs `on_install(commitTs)`,** so a recorder logs the commit before anything can persist it.
5. **Enqueues** the base-table writes for the store's writer, in commit order.
6. **Installs** each row's and entry's new version at the head of its chain, trims the chain (see GC), and sets bucket timestamps for inserts, deletes and entry changes.
7. **Publishes** `lastCommitTs = commitTs` with a release store, then releases the lock.

With a synced store, `Commit` returns once the store has synced `commitTs`.

- **Snapshots.** A new transaction takes `readTs = lastCommitTs` with one atomic load. Commits install in timestamp order under the lock, so every snapshot is complete and consistent.
- **Exact validation.** Because commits install in timestamp order, validation sees every version committed between `readTs` and `commitTs`.
- **GC.** Install trims each written chain to the versions newer than the oldest active `readTs`, plus the newest one at or below it. Validation needs the masks of the newer versions even after newer snapshots supersede them. A chain trims only once the installs since its last trim reach the versions that trim kept, so a hot row's walk to the horizon costs O(1) hops per install, amortized, and its chain stays within about twice its live versions. A row that stops being written keeps its last few versions until its next write.
- **Active snapshots.** `Begin` loads `lastCommitTs` and registers its `readTs` under one mutex, which GC also takes to compute the oldest `readTs`, so GC never trims a version a starting transaction needs. Trimmed chains have no readers: they are freed at once without a store, and held until durable with one, since the writer may still be persisting them.

## Durability

- **Writer.** The host runs `DB::RunWriter` on a thread; the simulator runs it as a coroutine. Each flush takes every queued commit, writes the batch, syncs it, and advances `DurableTs` to the batch's newest commit. Commits arriving during a sync share the next one.
- **Commit delay.** With `commit_delay` set, the writer first waits that long after finding work, trading latency for fewer syncs. The host supplies the wait, so the core keeps no clock.
- **Group wait.** Clients acknowledged by one sync commit again just after the next sync has begun without them. Left alone, the commits split into two groups that alternate between syncs, each waiting through two. The `gather` hook lets the host wait, after each sync, until as many new commits as the sync carried have joined those already queued, which merges the groups. The engine caps that wait at `celltnx_group_wait_us` or the previous flush's duration, whichever is less, because the wait pays only while it is shorter than the sync it saves. A 4 KiB `fdatasync` takes 2 ms on a drive that empties a volatile cache on each sync and 18 µs on EC2 instance store, where an uncapped 500 µs wait cost up to 20% of throughput ([BENCHMARKS.md](BENCHMARKS.md#group-wait)). Clients with long transactions never return in time: after two waits in a row in which fewer than half of them did, the writer skips waiting for 64 flushes, then tries again.
- **Errors.** A failed write or sync is permanent: the DB stops acknowledging, every later commit fails, and the process must restart and recover.
- **Store contract.** A crash may lose a suffix of the commits written since the last sync, but never part of a commit or a commit before a surviving one.
- **lionturtle.** Each flush writes every queued commit's row images, keyed by `0x01`, the table's durable ID (big-endian) and the primary key, plus the newest commit timestamp under `0x00`, as one atomic `lt_write`, then calls `lt_sync`. Flushes land in order, so a crash keeps a prefix of the commits. The store opens lionturtle with `preallocate_log`, which zero-fills each log segment ahead of its writes: on ext4 a sync that overwrites allocated space takes about half as long as one that grows the file. It also sets `checkpoint_distance` 8 and `cache_bytes` 64 MiB. lionturtle's commit is pinned in `store/lionturtle/fetch.sh`, and `store/lionturtle/check/` tests it against the engine's use: API edge cases, engine-shaped load, `kill -9` recovery and injected sync failures.
- **Recovery.** `DB::Recover` restores the commit clock to the store's newest commit, along with any rows its loader passes. The engine passes none and restores each table with `DB::LoadRows` when the table is first opened. Both install each row's newest image as of that commit, rebuild index entries from the rows, and move the table's counters past its largest 8-byte key. Every transaction begins after recovery, so no earlier history is needed.

## The MySQL engine

### Building and loading

`bench/build-mysql.sh release celltnx` fetches MySQL 8.4.13 and lionturtle, links `engine/` into the source tree as `storage/celltnx`, and builds `ha_celltnx.so`. The core and the store compile into the plugin, and lionturtle links in statically with its symbols hidden. Load it with `--plugin-load-add=ha_celltnx.so` and create tables with `ENGINE=CELLTNX`.

| Variable | Default | Meaning |
| --- | --- | --- |
| `celltnx_mode` | `cell+delta` | Conflict tracking: `row`, `cell` or `cell+delta`. Read-only. |
| `celltnx_bucket_bits` | 0 | Low key bits each phantom-tracking bucket spans. Read-only. |
| `celltnx_durable` | ON | Keep tables in a lionturtle store under `<datadir>/celltnx`; OFF keeps them in memory only. Read-only. |
| `celltnx_sync` | ON | Acknowledge a commit once it is synced; OFF acknowledges it once visible. Read-only. |
| `celltnx_commit_delay_us` | 0 | Microseconds the store's writer waits before each flush, so more commits share a sync. |
| `celltnx_group_wait_us` | 500 | Microseconds the store's writer may wait after each sync for the clients it acknowledged to commit again; it waits no longer than that sync took. |
| `celltnx_lock_wait_ms` | 1000 | How long a `SELECT … FOR UPDATE` waits for another transaction's row lock; 0 never waits. |

Status variables: `Celltnx_last_commit_ts` and `Celltnx_last_read_ts` give the session's last serialization point and snapshot. The global counters are `Celltnx_commits`, `Celltnx_conflicts`, `Celltnx_bucket_conflicts`, `Celltnx_check_failures`, `Celltnx_lock_waits`, `Celltnx_lock_timeouts`, `Celltnx_refreshes`, `Celltnx_refresh_conflicts`, `Celltnx_commit_lock_wait_ns` and `Celltnx_commit_lock_hold_ns`.

### Tables, rows and keys

Each table becomes a core table, and each secondary index a core index. A table without a primary key gets a hidden 8-byte counter key.

Integer columns become core integers, `FLOAT` and `DOUBLE` become floats, and every other column becomes its byte image through the Field API: strings, `DECIMAL`, temporal types, `BLOB`, `TEXT` and `JSON`.

Keys use the core's order-preserving encodings, one part per key column, each with a NULL tag. Integers and floats encode as numbers. Strings encode as their collation weights from `strnxfrm`, padded for PAD SPACE collations, so collation-equal strings get equal keys. `DECIMAL`, `DATETIME`, `TIMESTAMP` and `TIME` keys use their fixed-width binary images, which sort correctly as bytes. Keys are limited to 65,535 bytes and rows to 16 MiB. CREATE rejects prefix, full-text and spatial indexes, and tables over 63 columns.

Phantom tracking groups keys into buckets by the encoding of their leading numeric parts, 9 bytes each, so a TPC-C range within one district claims only that district. A key that starts with any other type puts the whole table or index in one bucket.

### Transactions and statements

Each connection has one core transaction, which takes its snapshot at its first statement or at `START TRANSACTION WITH CONSISTENT SNAPSHOT`, whatever `transaction_isolation` says. A finished transaction's allocations carry over to the connection's next one.

A statement sees the transaction's writes as they stood when it began. Each statement starts at a savepoint, and a failed statement rolls back to it; named savepoints work the same way.

A primary-key lookup reads one row. Every other read materializes the rows of its key range and records the range's buckets, so a later insert or delete inside the range conflicts. `records_in_range` counts committed keys in the range, up to 1,000, for the optimizer.

A commit validates, merges and installs under the global commit lock, then waits until the store has synced it. A transaction that wrote nothing commits at its snapshot without the lock.

### Column tracking and deltas

A statement reads the columns in `read_set` and `write_set`. Assigned columns count as read, because MySQL compares a row's old and new values to skip unchanged rows. Columns outside both sets return their values without recording a read.

In `cell+delta` mode an assignment `c = c + k`, `c = k + c` or `c = c - k`, in an UPDATE or an `ON DUPLICATE KEY UPDATE` clause, becomes a delta when all of these hold:

- `c` is an integer column;
- `k` is an integer literal or parameter;
- `c` appears nowhere else in the statement: an UPDATE's WHERE and ORDER BY, other assignments, or subqueries;
- `c` belongs to no index and no untranslatable CHECK.

`update_row` then adds `new − old` to the cell without reading it, so concurrent deltas commute. `UPDATE IGNORE`, tables with triggers or generated columns, and every other statement read and write whole columns.

A failed statement may have decided its error from a delta's untracked base value, for example an out-of-range sum. Rolling back such a statement records a read of each delta column it touched, so the error rests on validated data.

CHECK constraints are translated into core predicates on first use: comparisons, arithmetic, AND, OR, NOT, IS NULL, constants and columns. The core rechecks them against each merged row at commit. A CHECK outside that subset makes its columns final: they take no deltas, and MySQL's statement-time check is all they get.

### SELECT … FOR UPDATE

A primary-key read in `SELECT … FOR UPDATE` takes an advisory row lock. A transaction holding no lock waits for it, up to `celltnx_lock_wait_ms`; a transaction holding one never waits, so no wait can close a cycle. Once it holds the lock, if it waited or the row has changed since its snapshot, the transaction refreshes: it validates every read so far and moves its snapshot to the latest commit. A failed refresh returns 1213 at once and rolls the transaction back. Locks are released when the commit installs, before the durability wait, or at rollback.

A hot read-modify-write, such as NewOrder's `D_NEXT_O_ID` increment, therefore queues behind the previous one instead of rerunning after a commit-time conflict.

A delta committing inside such a read-modify-write would fail its validation, so an UPDATE assigning deltas also takes the row's lock when the lock's holder or waiters use a delta column. Each lock remembers the columns its holders and waiters read or assign until it falls idle. Deltas on other columns, such as Payment's `D_YTD` beside NewOrder's lock on the same district row, and deltas on rows no one locks, stay lock-free and commute.

Commit validation alone decides serializability. A wait that times out, a `NOWAIT` or `SKIP LOCKED` read, and a read by a transaction already holding a lock all proceed without the lock and may conflict at commit. A transaction that has read nothing moves its snapshot without validating, and so without the commit lock.

`LOCK IN SHARE MODE` is a plain tracked read, and table locks never block.

### Errors

| Cause | Error |
| --- | --- |
| A conflict at commit, or a failed refresh after a FOR UPDATE wait | 1213 `ER_LOCK_DEADLOCK` |
| A merged row fails a CHECK at commit | 3819 `ER_CHECK_CONSTRAINT_VIOLATED` |
| A delta overflows at commit | 1690 `ER_DATA_OUT_OF_RANGE` |
| A row, or a merged row at commit, exceeds 16 MiB | 1118 `ER_TOO_BIG_ROWSIZE` |
| A duplicate primary or unique key | 1062 `ER_DUP_ENTRY` |

MySQL keeps a statement's first error, so a failed COMMIT reports one of these with 1180 as a warning, and the server rolls the transaction back.

### Durable tables and DDL

The engine opens its lionturtle store under `<datadir>/celltnx` at startup, resumes the commit clock and runs the writer on a background thread. With `celltnx_sync` OFF, a commit is acknowledged once visible, and a crash may lose the latest acknowledged commits, as with InnoDB's `innodb_flush_log_at_trx_commit` at 0.

Each table loads its rows and rebuilds its secondary indexes when first opened, before any session can reach it. Auto-increment resumes past the largest loaded value, and hidden keys past the largest hidden key.

A table's durable ID is its data-dictionary object ID: MySQL stores a table in the dictionary before asking an engine without atomic DDL to create it, and RENAME keeps the ID. CREATE clears any rows left under the ID, and DROP deletes the table's rows and syncs. TRUNCATE drops and recreates the table, and ALTER TABLE copies it. DDL is not atomic: a crash between the dictionary's change and the engine's can leave a created table empty or a dropped table's rows unreachable in the store. Temporary tables live in memory only.

## Compared with InnoDB

| | celltnx | InnoDB |
| --- | --- | --- |
| Concurrency control | Optimistic: transactions run without locks and validate at commit | Pessimistic: row, gap and next-key locks held to commit, with MVCC for consistent reads |
| Conflict granularity | Cell: a row conflicts only on a column one transaction wrote and another read | Row, plus gaps between index records |
| Concurrent increments | `c = c ± k` deltas on one cell all commit | Each update locks the row; increments queue |
| Isolation | Always serializable, whatever `transaction_isolation` says | READ UNCOMMITTED, READ COMMITTED, REPEATABLE READ (default) or SERIALIZABLE |
| Plain reads | Lock-free snapshot reads, validated at commit | Lock-free consistent reads; shared next-key locks in SERIALIZABLE transactions |
| Phantoms | Scans record key buckets; a commit inside one conflicts | Next-key and gap locks block inserts into scanned ranges |
| `SELECT … FOR UPDATE` | Advisory lock on a primary-key row, so hot read-modify-writes queue; a timeout reads anyway | Exclusive record or next-key locks; a timeout fails the statement |
| Conflicts surface | At COMMIT as 1213, or at a FOR UPDATE that refreshes its snapshot | At the statement: 1213 on a deadlock, 1205 on a lock-wait timeout |
| CHECK constraints | MySQL checks each statement; the engine rechecks merged rows at commit | Checked at each statement |
| Data size | Every row stays in memory | Buffer pool caches pages; tables may exceed memory |
| Commit | Validate, merge and install under one global commit lock, held for microseconds; visible and unlocked before the sync, acknowledged after it | Locks released, then the redo log flushed; acknowledged after the flush |
| Durability | A writer thread writes each group of commits to lionturtle in one atomic write and one sync | Redo log flushed per `innodb_flush_log_at_trx_commit`, with group commit |
| Recovery | Load each table's newest rows when first opened; no undo needed, since the store holds only committed rows | Replay the redo log, then roll back incomplete transactions from undo |
| DDL | Copy ALTER; not atomic | Atomic; online ALTER for many changes |

celltnx lacks foreign keys, partitioning, prefix, full-text and spatial indexes, tables over 63 columns, XA and transactions that write both celltnx and InnoDB tables, replication, online backup, and row eviction.

Applications see these differences:

- Conflicts surface at COMMIT, so clients retry the whole transaction on 1213, as they would after an InnoDB deadlock.
- Two transactions inserting the same key both succeed at the statement; the second to commit gets 1213, and its retry reports the duplicate with 1062.
- A CHECK over a delta column can pass at the statement, on the transaction's view, and fail at COMMIT, on the merged row.
- Two concurrent UPDATEs that assign the same cell conflict, because an UPDATE reads the columns it assigns; deltas on that cell still commute.

## Deterministic simulation testing

The simulator (`sim/`) runs the core, a workload, a simulated disk and the serializability checker on one thread from a single seed. It injects faults, checks invariants as it goes, and reproduces any failure exactly from its seed.

**Determinism rules** for `core/`, `check/` and `workload/`:

- Randomness comes from a seeded `Rand` (PCG) per client. Waiting goes through `Options::wait` and yielding through `Options::yield`, which the simulator wires to its scheduler.
- That code starts no threads and reads no clock or global random source; hosts supply time through `Options::now` and waits through hooks.
- Iteration order never depends on hash order: read and write sets iterate in first-access order, and the checker's model uses ordered maps.
- With `wait` set, the commit lock is a flag coroutines wait on instead of a mutex, so a commit can interleave with other coroutines while holding it; invariant checks run only between commits.
- The Zipf generator computes `exp`, `log` and `pow` as fixed sequences of IEEE operations (`workload/detmath.h`), so a seed draws the same values on every compiler and libm, given the same FMA support.

**Scheduling:**

- Each client, the loader and the store's writer run as stackful coroutines on Boost.Context fibers (`sim/coroutine.h`). The writer is a daemon: the run ends when the clients do.
- Workloads are straight-line code against a client interface. The stress tests run each client on its own thread; the simulator runs each as a coroutine that yields before every operation.
- The scheduler picks the next runnable coroutine, one neither sleeping nor blocked on a false condition, from the seeded generator. With nothing runnable and nothing asleep, the run fails as a deadlock. Skewed seeds give clients unequal weights, so slow clients hold old snapshots across many commits.
- Transactions read snapshots, so a transaction's results depend only on the order of begins, commits and writer steps. Yielding at operation boundaries covers those orders. `Scan` and `IndexScan` also yield between rows, so commits land mid-scan.
- Finer points call `Options::interleave`, where the simulator switches with a per-seed probability: before a reader's skiplist seek returns, before each level a commit links into a skiplist, between a commit's row installs, and before it publishes its timestamp. Readers therefore see partially linked nodes and partially installed commits, as they can on real threads.

**Swarm parameters.** Each seed draws its own:

- mode and workload (`random`, `users` or `tpcc`), with the workload's transaction mix, secondary indexes, CHECKs, key-space size, θ and think time;
- client count, quota, bucket width and scheduler skew;
- whether a simulated disk is attached, whether commits wait for its sync, its sync latency, the writer's commit delay, and the probabilities of crashes and sync failures;
- the chance of switching at each interleave point: 0, 1%, 10% or 50%;
- with `-refresh -1`, the chance of a snapshot refresh before each operation: 0, 2%, 10% or 50%.

The commit delay, the interleave probability and the refresh rate come from their own random streams, so fixing one with `-commitdelay`, `-interleave` or `-refresh` leaves every other parameter unchanged. Small key spaces and narrow buckets force contention.

**Faults:**

- clients abort, roll back to savepoints, or abandon a transaction mid-flight;
- long read-only transactions hold back GC;
- deltas push values across CHECK boundaries, and unique keys collide;
- crashes at any step, including during group commit;
- lost unsynced commits, slow syncs, and sync failures, which crash the process.

A crash stops every coroutine, keeps the disk's synced commits plus a random prefix of the unsynced ones, opens a new DB, recovers it, checks it against the history, and restarts the writer and the unfinished clients.

**Invariants:**

- **Serializability.** The checker replays every run (see below).
- **Workload.** A read-only TPC-C audit verifies consistency conditions 1–4 at random points.
- **Internal,** every 1,000 steps and at the end: every skiplist level is sorted; version chains strictly decrease in timestamp, and none passes `lastCommitTs`; no bucket's timestamp passes `lastCommitTs`; every secondary index holds exactly one entry per live row, pointing at it, and unique indexes hold no duplicate; active snapshots are sorted; and `DurableTs() <= LastCommitTs()`.
- **Durability.** After each recovery, the recovered state equals the replay of every commit at or below the recovered timestamp. When commits wait for syncs, that timestamp is at least every acknowledged commit's and every acknowledged read-only transaction's `readTs`. The checker drops logged commits above it.
- **Liveness.** Every client finishes its quota within 5 million steps.

**Serializability checker** (`check/`). The workloads write unique values, so comparing values is meaningful. The recorder logs, for each finished transaction, its serialization point, the values it read and wrote, and whether its commit was acknowledged. Reads include not-found results and the complete result set of every scan and index scan. Writers serialize at `commitTs`, and read-only transactions at `readTs`, after every writer at or below it. `Replay` runs the log single-threaded in that order against an independent model, which enforces unique indexes itself; every logged outcome and the final tables must match. It also checks that writer timestamps run from 1 to `lastCommitTs` without gaps, that every committed row passes its CHECKs, that every constraint rejection violates a CHECK or overflows when replayed at the state it saw, and that a read-only transaction changes nothing.

**Reproduction and determinism.** A failure prints its seed and parameters, and `sim -seed N` replays it exactly. Each run hashes every operation's outcome and its step count into a trace. Sweeps rerun every tenth seed and fail on any mismatch, which catches nondeterminism. `sim -golden FILE` writes each seed's trace, steps, crash count, outcome and parameters, and `sim/golden.sh` compares them with a build of a git revision, HEAD by default, so a change meant to keep behavior identical, such as an optimization, can prove it.

**Self-test.** The `simbugs` preset compiles in deliberate bugs (`core/bugs.h`), and the simulator must find each within 30,000 seeds (`sim -bug NAME`). Internal invariants are off for these runs, so the checker, the workload audit and the durability check must catch each one:

| Bug | Effect |
| --- | --- |
| `skip-buckets` | validation ignores scanned buckets |
| `set-no-exists` | `Set` records no read of `kExists` |
| `gc-eager` | GC ignores active snapshots |
| `check-skip-delta` | merge skips CHECKs on delta-only rows |
| `no-absent-read` | a `Get` of a missing row records nothing |
| `delta-read-untracked` | reading a delta'd column records no read |
| `unique-no-read` | a unique-key claim records no read of the entry |
| `index-no-range` | an index scan records no buckets |
| `ack-before-sync` | commits are acknowledged before they sync |
| `read-only-no-wait` | read-only commits skip the durability wait |
| `ack-joined` | a flush acknowledges commits queued during its sync |
| `seek-reload` | a reader's seek reloads its result after comparing |
| `publish-early` | a commit publishes its timestamp before installing |
| `link-before-init` | a skiplist node is linked before its next pointers are set |
| `refresh-no-validate` | a snapshot refresh skips validating earlier reads (found with `-refresh -1`) |

`seek-reload`, `publish-early` and `link-before-init` surface only through interleave points. The simulator keeps trimmed versions allocated (`Options::keep_trimmed`), so a reader that walks a chain trimmed too early reads valid memory, and the checker sees its wrong answer.

**Coverage.** The simulator explores interleavings at operation boundaries and interleave points. The stress tests cover memory-level races on real threads under TSan, the store tests check lionturtle under `kill -9`, and the MTR suite covers the engine.

## Testing

- **Core tests** (`tests/txn_test.cc`, `tests/index_test.cc`): disjoint-column merges, read-write conflicts, write skew, phantoms, absent keys, a set racing a delete, 100 concurrent deltas on real threads, a CHECK limiting concurrent deltas, modes, savepoints, GC, refresh, size limits, and secondary indexes.
- **Checker tests** (`tests/check_test.cc`): `Replay` accepts a serializable history and rejects altered ones.
- **Stress tests** (`tests/stress_test.cc`): every workload on real threads in all three modes, replayed through the checker; clean under TSan.
- **Store tests** (`tests/store_test.cc`): row encoding, durable tables, the group-wait hook, and 8 `kill -9` rounds of TPC-C on one store, half with a commit delay, each recovering every acknowledged commit.
- **MTR suite** (`engine/mysql-test`): commit-time errors, column tracking and deltas, CHECKs at commit, FOR UPDATE waits and refreshes, status and system variables, and restarts with rows, indexes, CHECKs, auto-increment, BLOBs, keyless tables, RENAME, TRUNCATE and DROP.

```sh
./ci.sh                                    # every test in debug, ASan and TSan; golden; sweeps; self-test
build/release/sim -seeds 30000             # sweep 30,000 seeds in parallel
build/release/sim -seed 1234               # replay one seed
build/simbugs/sim -bug gc-eager -seeds 30000 -det 0
store/lionturtle/check/run.sh DIR          # lionturtle under the engine's load, kill -9 and sync failures
cd ~/.cache/celltnx/build-release/mysql-test && ./mtr --suite=celltnx
```

## References

- **Silo:** Tu et al., *Speedy Transactions in Multicore In-Memory Databases*, SOSP 2013. Version-based phantom protection, and a per-row-latch commit protocol that would replace the global commit lock.
- **HyPer MVCC:** Neumann, Mühlbauer, Kemper, *Fast Serializable MVCC for Main-Memory Database Systems*, SIGMOD 2015. Attribute-level validation against recently committed versions.
- **Escrow:** O'Neil, *The Escrow Transactional Method*, TODS 1986. Deltas.
- **TPC-C:** *TPC Benchmark C Standard Specification*, revision 5.11. NewOrder and Payment, and consistency conditions 1–4.
- **VOPR:** TigerBeetle's deterministic simulator. Seeded single-threaded runs, fault injection, and per-seed randomized parameters.
- **FoundationDB:** Zhou et al., *FoundationDB: A Distributed Unbundled Transactional Key Value Store*, SIGMOD 2021. Deterministic simulation and determinism checks by rerunning seeds.
- **Swarm testing:** Groce et al., *Swarm Testing*, ISSTA 2012. Per-seed feature and fault selection.
- **fjall:** `src/tx/optimistic/conflict_manager.rs`. Range-read conflict checks.
