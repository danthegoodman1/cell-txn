# The celltnx storage engine

`celltnx` runs cell-tnx's transaction core inside MySQL 8.4 as a storage engine. Its tables sit beside InnoDB tables in the same server. Rows live in memory, in a skiplist per table with a version chain per row; a lionturtle store under the data directory holds the durable copy. Transactions are always serializable: they execute without locks, and a commit validates every cell the transaction read against later commits. `SPEC.md` describes the core; this document describes how the engine maps MySQL onto it.

## Building and loading

`bench/build-mysql.sh release celltnx` fetches MySQL 8.4.13 and lionturtle (pinned in `cpp/store/lionturtle/fetch.sh`), links `cpp/engine` into the source tree as `storage/celltnx`, and builds `ha_celltnx.so`. Load it with `--plugin-load-add=ha_celltnx.so` and create tables with `ENGINE=CELLTNX`. lionturtle links in statically with its symbols hidden.

| Variable | Default | Meaning |
| --- | --- | --- |
| `celltnx_mode` | `cell+delta` | Conflict tracking: `row`, `cell` or `cell+delta`. Read-only. |
| `celltnx_bucket_bits` | 0 | Low key bits each phantom-tracking bucket spans. Read-only. |
| `celltnx_durable` | ON | Keep tables in a lionturtle store under `<datadir>/celltnx`; OFF keeps them in memory only. Read-only. |
| `celltnx_sync` | ON | Acknowledge a commit once it is synced; OFF acknowledges it once visible. Read-only. |
| `celltnx_commit_delay_us` | 0 | Microseconds the store's writer waits before each flush, so more commits share a sync. |
| `celltnx_group_wait_us` | 500 | Microseconds the store's writer may wait after each sync for the clients it acknowledged to commit again; it never waits longer than that sync took. |
| `celltnx_lock_wait_ms` | 1000 | How long a `SELECT … FOR UPDATE` waits for another transaction's row lock; 0 never waits. |

## Tables, rows and keys

Each table becomes a core table, and each secondary index a core index table whose entries map the index columns plus the primary key to nothing. A table without a primary key gets a hidden 8-byte counter key.

Integer columns become core integers, `FLOAT` and `DOUBLE` become floats, and every other column becomes its byte image through the Field API: strings, `DECIMAL`, temporal types, `BLOB`, `TEXT` and `JSON`.

Keys use the core's order-preserving encodings, one part per key column, each with a NULL tag. Integers and floats encode as numbers. Strings encode as their collation weights from `strnxfrm`, padded for PAD SPACE collations, so collation-equal strings get equal keys. `DECIMAL`, `DATETIME`, `TIMESTAMP` and `TIME` keys use their fixed-width binary images, which sort correctly as bytes. Keys may not exceed 65,535 bytes nor rows 16 MiB. Prefix, full-text and spatial indexes, and tables over 63 columns, are rejected at CREATE.

Phantom tracking groups keys into buckets by the encoding of their leading numeric parts, 9 bytes each, so a TPC-C range within one district claims only that district. A key that starts with any other type puts the whole table or index in one bucket.

## Transactions and statements

Each connection has one core transaction, which takes its snapshot at its first statement or at `START TRANSACTION WITH CONSISTENT SNAPSHOT`, whatever `transaction_isolation` says. A finished transaction's allocations carry over to the connection's next one.

A statement sees the transaction's writes as they stood when it began, so an `INSERT … SELECT` never reads its own output. Each statement starts at a savepoint, and a failed statement rolls back to it; named savepoints work the same way.

A primary-key lookup reads one row. Every other read materializes the rows of its key range and records the range's buckets, so a later insert or delete inside the range conflicts. `records_in_range` counts committed keys in the range, up to 1,000, for the optimizer.

A commit validates, merges and installs under one global commit lock, then waits until the store has synced it. A transaction that wrote nothing commits at its snapshot without the lock.

## Column tracking and deltas

A statement reads the columns in `read_set` and `write_set`. Assigned columns count as read, because MySQL compares a row's old and new values to skip unchanged rows. Columns outside both sets return their values without recording a read.

In `cell+delta` mode an assignment `c = c + k`, `c = k + c` or `c = c - k`, in an UPDATE or an `ON DUPLICATE KEY UPDATE` clause, becomes a delta when `c` is an integer column, `k` is an integer literal or parameter, `c` appears nowhere else in the statement (an UPDATE's WHERE and ORDER BY, other assignments, subqueries), and `c` belongs to no index and no untranslatable CHECK. `update_row` then adds `new − old` to the cell without reading it, so concurrent deltas commute. `UPDATE IGNORE`, tables with triggers or generated columns, and every other statement read and write whole columns.

A failed statement may have decided its error from a delta's untracked base value, for example an out-of-range sum. Rolling back such a statement records a read of each delta column it touched, so the error rests on validated data.

CHECK constraints are translated into core predicates on first use: comparisons, arithmetic, AND, OR, NOT, IS NULL, constants and columns. The core rechecks them against each merged row at commit. A CHECK outside that subset makes its columns final: they take no deltas, and MySQL's statement-time check is all they get.

## SELECT … FOR UPDATE

A primary-key read in `SELECT … FOR UPDATE` takes an advisory row lock. A transaction holding no lock waits for it, up to `celltnx_lock_wait_ms`; a transaction holding one never waits, so no wait can close a cycle. Once it holds the lock, if it waited or the row has changed since its snapshot, the transaction refreshes: it validates every read so far and moves its snapshot to the latest commit. A failed refresh returns 1213 at once and rolls the transaction back. Locks are released when the commit installs, before the durability wait, or at rollback.

A hot read-modify-write, such as NewOrder's `D_NEXT_O_ID` increment, therefore queues behind the previous one instead of rerunning after a commit-time conflict.

A delta committing inside such a read-modify-write would fail its validation, so an UPDATE assigning deltas takes the row's lock too when the lock's holder or waiters use a delta column. Each lock remembers the columns its holders and waiters read or assign until it falls idle. Deltas on other columns, such as Payment's `D_YTD` beside NewOrder's lock on the same district row, and deltas on rows no one locks, stay lock-free and commute.

Commit validation alone decides serializability: a wait that times out, a `NOWAIT` or `SKIP LOCKED` read, and a read by a transaction already holding a lock all proceed without the lock and may conflict at commit. A transaction that has read nothing moves its snapshot without validating, and so without the commit lock.

`LOCK IN SHARE MODE` is a plain tracked read, and table locks never block.

## Errors

| Cause | Error |
| --- | --- |
| A conflict at commit, or a failed refresh after a FOR UPDATE wait | 1213 `ER_LOCK_DEADLOCK` |
| A merged row fails a CHECK at commit | 3819 `ER_CHECK_CONSTRAINT_VIOLATED` |
| A delta overflows at commit | 1690 `ER_DATA_OUT_OF_RANGE` |
| A row, or a merged row at commit, exceeds 16 MiB | 1118 `ER_TOO_BIG_ROWSIZE` |
| A duplicate primary or unique key | 1062 `ER_DUP_ENTRY` |

MySQL keeps a statement's first error, so a failed COMMIT reports one of these with 1180 as a warning, and the server rolls the transaction back.

## Durability and recovery

A writer thread drains the commit queue. Each flush writes every queued commit's row images, keyed by `0x01`, the table's durable ID and the primary key, together with the newest commit timestamp, as one atomic `lt_write`, then calls `lt_sync`. Flushes land in order, so a crash keeps a prefix of the commits. The store opens lionturtle with `preallocate_log`, which zero-fills each log segment ahead of its writes: on ext4 a sync that overwrites allocated space takes about half as long as one that grows the file. Commits arriving during a sync share the next one. Clients acknowledged by one sync commit again just after the next has begun without them, so on their own the commits split into two groups that alternate between syncs, each waiting through two. After each sync the writer therefore waits until as many new commits as the sync carried have joined those already queued, merging the groups. The wait pays only while it is shorter than the sync it saves, so it lasts at most `celltnx_group_wait_us` or as long as the previous flush took, whichever is less: the full 500 µs on a drive that empties a volatile cache on each sync (a 4 KiB `fdatasync` takes 2 ms on the desktop drive in `RESULTS-MYSQL.md`), and next to nothing on one with power-loss protection (18 µs on EC2 instance store), where the full wait cost up to 20% of throughput. Clients with long transactions never return in time: after two waits in a row in which fewer than half of them did, the writer skips waiting for 64 flushes, then tries again. `celltnx_commit_delay_us` adds a fixed wait before every flush. With `celltnx_sync` OFF, a commit is acknowledged once visible, and a crash may lose the latest acknowledged commits, as with InnoDB's `innodb_flush_log_at_trx_commit` at 0. A failed write or sync stops the store for good, and every later commit fails.

At startup the engine opens the store and resumes the commit clock. Each table loads its rows and rebuilds its secondary indexes when first opened, before any session can reach it. Auto-increment resumes past the largest loaded value, and hidden keys past the largest hidden key.

A table's durable ID is its data-dictionary object ID: MySQL stores a table in the dictionary before asking an engine without atomic DDL to create it, and RENAME keeps the ID. CREATE clears any rows left under the ID, and DROP deletes the table's rows and syncs. TRUNCATE drops and recreates the table. DDL is not atomic: a crash between the dictionary's change and the engine's can leave a created table empty or a dropped table's rows unreachable in the store. Temporary tables live in memory only.

## Out of scope

XA, transactions that write both `celltnx` and InnoDB tables, foreign keys, partitioning, replication, online backup, and evicting rows from memory.

## Testing

The MTR suite in `cpp/engine/mysql-test` covers commit-time errors, column tracking and deltas, CHECKs at commit, FOR UPDATE waits and refreshes, and restarts with rows, indexes, CHECKs, auto-increment, renames, TRUNCATE and DROP:

```
cd ~/.cache/celltnx/build-release/mysql-test && ./mtr --suite=celltnx
```

The core, the checker, the workloads and the store have their own tests and the deterministic simulator, described in `cpp/` and `PLAN-MYSQL.md`.
