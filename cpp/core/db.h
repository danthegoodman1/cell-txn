// celltnx's core: a serializable optimistic transaction layer that tracks
// conflicts per (row, column), merges rows at commit, and commits
// commutative integer deltas without conflicts. It is a port of the Go
// package txn and must behave identically; SPEC.md describes the design.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arena.h"
#include "flatmap.h"
#include "skiplist.h"
#include "value.h"

namespace celltnx {

// Mode selects how conflicts are tracked.
enum class Mode : uint8_t {
  kRow,        // every read covers the whole row and deltas read their column
  kCell,       // reads are tracked per column; deltas still read their column
  kCellDelta,  // reads are tracked per column and deltas commute
};

const char *ModeName(Mode m);

// Check is a CHECK constraint over the columns in cols. ok runs under the
// commit lock, so calls never overlap.
struct Check {
  std::string name;
  ColMask cols = 0;
  std::function<bool(Row)> ok;
};

// Index is a secondary index; its columns never take commuting deltas.
struct Index {
  std::string name;
  ColMask cols = 0;
  bool unique = false;
  // key encodes the row's indexed columns into *out (cleared first) as an
  // order-preserving, prefix-free key, reading only the columns in cols.
  // It returns false to exempt the row from uniqueness, as a NULL does in
  // MySQL.
  std::function<bool(Row, std::string *out)> key;
  int bucket_prefix = 0;
};

// Schema describes a table. Primary keys are byte strings whose order is
// the key order.
struct Schema {
  std::string name;
  std::vector<std::string> cols;
  std::vector<Check> checks;
  std::vector<Index> indexes;
  // bucket_prefix is how many leading key bytes BucketOf uses; default 8.
  int bucket_prefix = 0;
};

// A committed version, and the row image it carries, in one allocation.
// Everything but next is immutable once published; next changes only when
// GC trims the chain.
struct Version {
  uint64_t ts = 0;
  ColMask mask = 0;
  // run_ts is the ts of the oldest version in this version's run: the
  // consecutive versions, ending here, that share its mask. Validation
  // checks a run of deltas once instead of walking it.
  uint64_t run_ts = 0;
  uint32_t width = 0;
  bool tombstone = true;
  std::atomic<Version *> next{nullptr};

  Value *vals() { return reinterpret_cast<Value *>(this + 1); }
  // row is the image, empty for a tombstone.
  Row row() const { return tombstone ? Row() : Row(const_cast<Version *>(this)->vals(), width); }

  // Make copies row and its strings; a null row makes a tombstone.
  static Version *Make(uint64_t ts, ColMask mask, const Row *row);
  static void Free(Version *v);
  // FreeChain frees v and every version after it.
  static void FreeChain(Version *v);
};

// Chain is a row's version list, newest first. A commit trims the chain
// once the installs since its last trim reach the versions that trim kept,
// so the walk to the GC horizon costs O(1) hops per install, amortized, and
// a hot row's chain stays within about twice its live versions. The commit
// lock guards installs and kept.
struct Chain {
  std::atomic<Version *> head{nullptr};
  uint32_t installs = 0;  // since the last trim
  uint32_t kept = 0;      // versions the last trim kept
};

using RowNode = Node<Chain>;

// Store persists committed base-table writes. The writer calls Write and
// then Sync with batches in commit order; neither runs under the commit
// lock. A crash may lose a suffix of the commits written since the last
// Sync, but never part of a commit or a commit before a surviving one.
struct WriteRef {
  int table = 0;
  std::string_view key;  // into the commit's keys
  const Version *version = nullptr;
  // row is the new image, empty for a delete.
  Row row() const { return version->row(); }
  bool deleted() const { return version->tombstone; }
};

// Commit is one transaction's base-table writes at its commit timestamp.
// Index entries are derived from the rows on recovery. The versions stay
// alive until the commit is durable.
struct Commit {
  uint64_t ts = 0;
  std::vector<WriteRef> writes;
  std::vector<char> keys;  // a vector keeps its buffer when moved
};

class Status;

class Store {
 public:
  virtual ~Store() = default;
  virtual Status Write(std::span<const Commit> batch) = 0;
  virtual Status Sync() = 0;
};

// Options configures a DB.
struct Options {
  Mode mode = Mode::kCellDelta;
  // bucket_bits clears that many low bits of a key's bucket prefix, so each
  // bucket covers 2^bucket_bits adjacent prefixes.
  unsigned bucket_bits = 0;
  // yield, when set, is called at each interleaving point. The simulator
  // uses it to switch coroutines.
  std::function<void()> yield;
  // interleave, when set, is called at fine-grained points inside
  // lock-free reads and inside commits, where the simulator may switch
  // coroutines. Commits call it while holding the commit lock, so it
  // requires wait, which makes that lock cooperative.
  std::function<void()> interleave;
  // now and on_commit, when both set, report commit-lock wait and hold
  // times in now's units.
  std::function<int64_t()> now;
  std::function<void(int64_t wait, int64_t hold)> on_commit;
  // no_invariants turns off internal assertions, so simulator self-tests
  // must catch deliberate bugs through the checker alone.
  bool no_invariants = false;
  // store, when set, persists every commit. A commit is acknowledged once
  // it is synced, and a read-only commit once its snapshot is, unless
  // no_sync is set. The DB does not own it.
  Store *store = nullptr;
  bool no_sync = false;
  // commit_delay, when set, runs on the writer before each flush. Hosts
  // sleep there, so commits arriving meanwhile share the sync.
  std::function<void()> commit_delay;
  // gather, when set, runs on the writer before each flush, after
  // commit_delay: last is how many commits the previous flush carried,
  // flush_ns how long its write and sync took in now's units (0 without
  // now), and queued reports how many wait now. Hosts wait there for the
  // clients the previous flush acknowledged to commit again, so their
  // commits share the next sync instead of alternating with another
  // group's.
  std::function<void(size_t last, int64_t flush_ns, const std::function<size_t()> &queued)> gather;
  // wait, when set, replaces blocking waits: it must return once cond
  // holds. The simulator uses it to yield instead of block.
  std::function<void(const std::function<bool()> &cond)> wait;
  // keep_trimmed keeps versions GC trims until the DB is destroyed, as Go's
  // garbage collector would while anything still points at them. The
  // simulator sets it so deliberate GC bugs behave as they do in Go.
  bool keep_trimmed = false;
  // max_key_bytes and max_row_bytes, when nonzero, bound primary keys and
  // rows in EncodedSize terms: the durable store's limits.
  size_t max_key_bytes = 0;
  size_t max_row_bytes = 0;
};

enum class Code : uint8_t {
  kOk,
  kNotFound,      // the row is missing
  kDuplicateKey,  // a primary or unique key is taken
  kOverflow,      // integer overflow
  kNotInteger,    // a delta on a non-integer value
  kDone,          // the transaction already finished
  kConflict,      // validation failed
  kConstraint,    // a merged row failed a CHECK, overflowed or grew too large
  kClosed,        // the DB closed or its store failed
  kTooLarge,      // a key or row over the store's limits
  kIo,            // the store failed
  kInternal,      // an internal invariant check failed
};

// Error describes a failure. Fields beyond code and message depend on it:
// a conflict sets table, pk (for a row read), cols and bucket; a
// constraint sets table, pk, check and after_ts; a duplicate key sets
// table, index, key and owner.
struct Error {
  Code code = Code::kOk;
  int table = -1;
  std::string pk, index, key, owner, check, detail;
  ColMask cols = 0;
  bool bucket = false;
  uint64_t after_ts = 0;
  std::string Message() const;
};

class Status {
 public:
  Status() = default;
  Status(Error e) : e_(std::make_unique<Error>(std::move(e))) {}
  Status(Code c) : e_(c == Code::kOk ? nullptr : std::make_unique<Error>(Error{.code = c})) {}
  Status(Status &&) = default;
  Status &operator=(Status &&) = default;

  bool ok() const { return e_ == nullptr; }
  Code code() const { return e_ ? e_->code : Code::kOk; }
  const Error &error() const { return *e_; }
  std::string Message() const { return e_ ? e_->Message() : "ok"; }

 private:
  std::unique_ptr<Error> e_;
};

// InvariantError reports a broken internal invariant or API misuse.
struct InvariantError : std::logic_error {
  using std::logic_error::logic_error;
};

// A row returned by a scan or Dump. The key and values stay valid until the
// transaction ends.
struct Rec {
  std::string_view pk;
  std::vector<Value> row;
};

// An owned row, from Dump.
struct OwnedRec {
  std::string pk;
  OwnedRow row;
};

class DB;
class Txn;
struct Table;
struct IndexDef;

// Match sees a scanned row with only the predicate columns filled and
// reports whether it matches.
using Match = std::function<bool(std::string_view pk, Row row)>;

class DB {
 public:
  // Base tables take IDs 0..n-1 in schema order; their index tables follow.
  DB(Options opts, std::vector<Schema> schemas);
  ~DB();
  DB(const DB &) = delete;
  DB &operator=(const DB &) = delete;

  // Begin starts a transaction reading the latest committed snapshot. A
  // finished transaction passed as reuse is reset and returned, keeping
  // its allocations.
  std::unique_ptr<Txn> Begin(std::unique_ptr<Txn> reuse = nullptr);

  // CreateTable adds a table outside any transaction and returns its ID.
  int CreateTable(Schema s);
  // CreateIndex adds a secondary index outside any transaction, building it
  // from the latest committed rows. It fails if a unique index would hold
  // a duplicate.
  Status CreateIndex(int tbl, Index d);
  // DropIndex removes secondary index idx outside any transaction; later
  // indexes shift down one position.
  void DropIndex(int tbl, int idx);
  // SetChecks replaces a table's CHECK constraints for future commits.
  void SetChecks(int tbl, std::vector<Check> checks);
  // TableID returns the ID of the base table with the given name.
  std::optional<int> TableID(std::string_view name) const;
  // DropTable removes a base table and its indexes outside any transaction.
  // The caller guarantees no transaction still uses it; its memory lives
  // until the DB is destroyed, since queued commits may reference it.
  void DropTable(int tbl);
  // ApproxRows estimates a table's live rows, for query planning.
  uint64_t ApproxRows(int tbl) const;
  // CountRange counts the latest committed rows with key in [lo, hi) of the
  // primary key (idx < 0) or of secondary index idx, stopping at cap, for
  // query planning. An empty hi is unbounded.
  uint64_t CountRange(int tbl, int idx, std::string_view lo, std::string_view hi, uint64_t cap) const;

  Mode mode() const { return opts_.mode; }
  const Options &options() const { return opts_; }
  uint64_t LastCommitTs() const { return last_.load(std::memory_order_acquire); }

  // Auto-increment and hidden-key counters run outside transactions, so
  // aborted transactions leave gaps.
  uint64_t NextID(int tbl);
  uint64_t NextHidden(int tbl);
  uint64_t PeekID(int tbl) const;
  void SetID(int tbl, uint64_t id);
  void BumpID(int tbl, uint64_t id);

  // Committing reports whether a commit holds the cooperative commit lock;
  // the simulator checks invariants only between commits.
  bool Committing() const { return committing_; }

  // Dump returns every live row of a table at the latest commit, in key
  // order. It is meant for a quiescent DB.
  std::vector<OwnedRec> Dump(int tbl) const;

  // CheckInvariants verifies the store's internal structure and that every
  // secondary index matches its table at the latest commit.
  Status CheckInvariants();
  // ChainLength returns how many versions a row keeps, for tests of GC.
  int ChainLength(int tbl, std::string_view pk) const;

  // Durability. DurableTs is the newest synced commit timestamp. WaitWork
  // blocks until the writer has commits to flush or the DB closes, and
  // reports whether to keep running. Flush writes and syncs every queued
  // commit; a failure is permanent. RunWriter flushes until the DB closes
  // or a flush fails. Close stops the writer and fails pending waits.
  uint64_t DurableTs();
  bool WaitWork();
  Status Flush();
  Status RunWriter();
  void Close();

  // Recover loads the newest image of every row, as of commit last, into an
  // empty DB: load calls put for each row. Index entries are rebuilt from
  // the rows, and each table's counters resume past its largest 8-byte key.
  using Put = std::function<void(int tbl, std::string_view key, Row row)>;
  Status Recover(uint64_t last, const std::function<Status(const Put &)> &load);
  // LoadRows loads one table's rows after Recover, as of the recovered
  // commit, for hosts that restore each table when it is first used. The
  // table must be unreachable by transactions until it returns.
  using RowPut = std::function<void(std::string_view key, Row row)>;
  Status LoadRows(int tbl, const std::function<Status(const RowPut &)> &load);

 private:
  friend class Txn;
  struct Durability;
  struct Registry;

  Table *table(int i) const { return (*tables_.load(std::memory_order_acquire))[i]; }
  void AppendTable(Table *t);
  IndexDef *AddIndex(int tbl, Index d);
  void Lock();
  void Unlock();
  Hook interleave_hook() const { return opts_.interleave ? &opts_.interleave : nullptr; }
  void Interleave() const {
    if (opts_.interleave) opts_.interleave();
  }
  void Point() const {
    if (opts_.yield) opts_.yield();
  }
  void Invariant(bool ok, const std::string &what) const;
  void Wait(std::condition_variable &cv, const std::function<bool()> &cond);
  void Enqueue(Commit c);
  // Retire disposes of the chains a commit at ts trimmed: kept with
  // keep_trimmed, held until durable when a store may still be writing
  // them, and freed otherwise.
  void Retire(uint64_t ts, std::vector<Version *> &tails);
  Status WaitDurable(uint64_t ts);

  Options opts_;
  // tables_ lists every table by ID, base and index alike; DDL replaces the
  // vector under the commit lock and keeps the old one until destruction.
  std::atomic<std::vector<Table *> *> tables_;
  std::vector<std::unique_ptr<std::vector<Table *>>> table_lists_;
  std::mutex mu_;           // the global commit lock
  bool committing_ = false;  // the commit lock, when wait makes it cooperative
  alignas(64) std::atomic<uint64_t> last_{0};
  uint64_t recovered_ = 0;  // the commit Recover restored
  std::unique_ptr<Registry> readers_;
  std::unique_ptr<Durability> dur_;
  std::vector<Version *> kept_;  // trimmed versions, with keep_trimmed
};

// Txn is a transaction, used by one thread at a time. Rows and keys it
// returns stay valid until it ends.
class Txn {
 public:
  ~Txn();
  Txn(const Txn &) = delete;
  Txn &operator=(const Txn &) = delete;

  uint64_t ReadTs() const { return read_ts_; }
  // SerialTs is the serialization point after a successful commit: the
  // commitTs, or the readTs if the transaction wrote nothing.
  uint64_t SerialTs() const { return serial_; }
  // Wrote reports whether the committed transaction installed writes.
  bool Wrote() const { return serial_ != 0 && serial_ != read_ts_; }

  // Get reads cols of a row into *row, full width with only cols filled.
  // It records a read of cols and Exists; a missing row records an
  // absent-key read.
  Status Get(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found);
  // GetUntracked reads cols from the transaction's view without recording.
  Status GetUntracked(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found);
  // GetRow returns the whole row but records a read of only track and
  // Exists. The caller must not let untracked columns influence anything it
  // writes or returns.
  Status GetRow(int tbl, std::string_view pk, ColMask track, std::vector<Value> *row, bool *found);
  // CurrentRow is GetRow including the open statement's own writes, for
  // write-path lookups such as the row a duplicate key collided with.
  Status CurrentRow(int tbl, std::string_view pk, ColMask track, std::vector<Value> *row, bool *found);

  // Scan visits every row with key in [lo, hi) in key order; an empty hi is
  // unbounded. match sees each row with only pred filled. Scan returns the
  // matches with pred and proj filled. It records every bucket overlapping
  // the range, pred and Exists for every row visited, and proj for every
  // match.
  Status Scan(int tbl, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj, const Match &match,
              std::vector<Rec> *out);
  // IndexScan visits the rows whose entries in secondary index idx fall in
  // [lo, hi), in entry order, recording as Scan does plus a read of the
  // indexed columns.
  Status IndexScan(int tbl, int idx, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj,
                   const Match &match, std::vector<Rec> *out);
  // ScanRows returns whole rows in [lo, hi) of the primary key (idx < 0) or
  // of secondary index idx, recording reads of only track, Exists and the
  // indexed columns.
  Status ScanRows(int tbl, int idx, std::string_view lo, std::string_view hi, ColMask track, std::vector<Rec> *out);

  // Set assigns cols of an existing row from the full-width vals. It
  // records a read of Exists only.
  Status Set(int tbl, std::string_view pk, ColMask cols, Row vals);
  // Add adds delta to an integer column of an existing row. In kCellDelta
  // mode it records only a read of Exists, so concurrent Adds commute; in
  // the other modes, and on indexed columns, it also reads the column.
  Status Add(int tbl, std::string_view pk, int col, int64_t delta);
  // Insert creates a row, recording a read of Exists so a concurrent
  // insert of the same key conflicts.
  Status Insert(int tbl, std::string_view pk, Row row);
  // Delete removes an existing row.
  Status Delete(int tbl, std::string_view pk);

  // BeginStatement opens a statement: until the matching EndStatement,
  // reads see the transaction's writes as of now. Statements nest; the
  // outermost one sets the view.
  void BeginStatement();
  void EndStatement();

  // Savepoint marks the write state for RollbackTo, which discards later
  // writes. Recorded reads stay, since the caller may have used them.
  int Savepoint() const { return int(undo_.size()); }
  void RollbackTo(int sp);

  // Commit installs the transaction and waits until it is durable.
  Status Commit();
  // Install validates, merges and installs at a new timestamp. A
  // transaction that wrote nothing serializes at its readTs.
  Status Install();
  // WaitDurable blocks until the serialization point is durable.
  Status WaitDurable();
  // Abort ends the transaction without installing anything.
  void Abort();

  // Refresh moves the snapshot to the latest commit, as if the transaction
  // had begun there. It fails with a conflict, keeping the snapshot, when a
  // commit since the snapshot wrote a cell, or changed a bucket, that the
  // transaction read. Rows and keys returned earlier stay valid.
  Status Refresh();
  // Changed reports whether a commit since the snapshot changed row pk.
  bool Changed(int tbl, std::string_view pk) const;

  // on_install, when set, runs under the commit lock once the commit is
  // certain, before it is visible or handed to the writer.
  std::function<void(uint64_t ts)> on_install;

 private:
  friend class DB;
  Txn(DB *db, uint64_t read_ts) : db_(db), read_ts_(read_ts) {}
  void Reset(uint64_t read_ts);

  enum class Kind : uint8_t { kUpdate = 1, kInsert, kDelete };
  // Write is a pending change to one row. It is replaced, never mutated, so
  // undo records can hold old versions; all of it lives in the arena.
  struct Write {
    Kind kind;
    bool existed;     // the row existed in the snapshot
    ColMask set = 0;    // update: columns assigned
    ColMask delta = 0;  // update: columns with pending deltas
    Value *vals = nullptr;     // update: assigned values; insert: the row
    int64_t *deltas = nullptr;  // update: pending delta sums
    uint32_t width = 0;
  };
  struct RowKey {
    int tbl;
    std::string_view pk;
    friend bool operator==(const RowKey &a, const RowKey &b) { return a.tbl == b.tbl && a.pk == b.pk; }
  };
  struct RowKeyHash {
    size_t operator()(const RowKey &k) const;
  };
  struct Read {
    RowKey k;
    RowNode *n;
    ColMask mask;
  };
  struct ScanRange {
    int tbl;
    std::string_view lo, hi;  // bucket keys; an empty hi is unbounded
  };
  struct WSlot {
    RowKey k;
    RowNode *n = nullptr;  // the row's node, when it existed at write time
    Write *w = nullptr;    // nullptr when the row has no pending change
    Write *stmt_prev = nullptr;  // the write reads see during the statement
    uint64_t stmt_epoch = 0;     // stmt_prev is set when this equals stmt_epoch_
  };
  struct Undo {
    int i;
    Write *prev;
  };
  struct ScanSpec {
    ColMask pred = 0, proj = 0;
    const Match *match = nullptr;
    bool full = false;  // return whole rows
  };
  struct OwnRow {
    std::string_view key;
    WSlot s;
    Row row;
  };
  struct InstallRec;

  Table *table(int tbl) const { return db_->table(tbl); }
  RowKey Key(int tbl, std::string_view pk);
  void Record(const RowKey &k, RowNode *n, ColMask mask);
  Write *Pending(const RowKey &k) const;
  Write *Seen(int i) const;
  Write *SeenPending(const RowKey &k) const;
  void Put(const RowKey &k, RowNode *n, Write *w);
  Row Snapshot(RowNode *n) const;
  Status View(RowNode *n, const Write *w, Row *out);
  ColMask ReadMask(ColMask cols, const Write *w) const;
  void CheckCols(int tbl, ColMask cols) const;
  Status GetImpl(int tbl, std::string_view pk, ColMask cols, bool track, bool full, bool current,
                 std::vector<Value> *row, bool *found);
  void Visit(const RowKey &k, RowNode *n, const Write *w, Row row, const ScanSpec &sp, std::vector<Rec> *out);
  Status Own(int tbl, const std::function<std::string_view(std::string_view pk, Row row)> &key, std::string_view lo,
             std::string_view hi, std::vector<OwnRow> *out);
  void RecordRange(Table *tb, std::string_view lo, std::string_view hi);
  Status ScanImpl(int tbl, std::string_view lo, std::string_view hi, ScanSpec sp, std::vector<Rec> *out);
  Status IndexScanImpl(int tbl, int idx, std::string_view lo, std::string_view hi, ScanSpec sp,
                       std::vector<Rec> *out);
  Write *Existing(const RowKey &k, RowNode **n, bool *exists);
  Write *ExistingForSet(const RowKey &k, RowNode **n, bool *exists);
  Status CheckUnique(Table *tb, std::string_view pk, Row row, ColMask written);
  Status Update(int tbl, std::string_view pk, ColMask written, const std::function<Status(Write *)> &fn);
  Write *NewWrite(Kind kind, bool existed, uint32_t width);
  Write *Clone(const Write *w);
  Status CheckSize(int tbl, std::string_view pk, Row row) const;
  Status CommitLocked(std::vector<WSlot> &ws);
  Status Validate();
  Status Merge(Table *tb, const WSlot &s, Row old, uint64_t ts, Version **out);
  void Finish();

  // EndSnapshot releases the transaction's registrations.
  void EndSnapshot();

  DB *db_;
  uint64_t read_ts_;
  uint64_t serial_ = 0;
  bool done_ = false;
  // After a refresh the first snapshot stays registered, since rows read
  // from it may still be in use; its versions survive until the end.
  bool pinned_ = false;
  uint64_t pinned_ts_ = 0;
  Arena arena_;

  std::vector<Read> reads_;  // first-read order, for deterministic validation
  FlatMap<RowKey, int, RowKeyHash> rpos_;
  std::vector<ScanRange> scans_;

  std::vector<WSlot> writes_;  // first-write order, for deterministic commits
  FlatMap<RowKey, int, RowKeyHash> wpos_;
  std::vector<Undo> undo_;

  // While a statement is open, reads see this transaction's writes as they
  // stood when it began: a slot whose stmt_epoch matches holds its write
  // from before the statement first changed it. Write-path checks
  // (existence, uniqueness) still see every write.
  int stmt_ = 0;
  uint64_t stmt_epoch_ = 1;
};

// AddValue returns v+delta for an integer v, or kOverflow or kNotInteger.
Code AddValue(const Value &v, int64_t delta, Value *out);
// AddDelta sums two pending deltas, reporting false on overflow.
bool AddDelta(int64_t a, int64_t b, int64_t *out);

}  // namespace celltnx
