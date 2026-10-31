// Structures private to the core's implementation files.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "db.h"

namespace celltnx {

// Visible returns the newest version of n at or below ts, or nullptr.
inline Version *Visible(const RowNode *n, uint64_t ts) {
  if (n == nullptr) return nullptr;
  for (Version *v = n->val.head.load(std::memory_order_acquire); v != nullptr;
       v = v->next.load(std::memory_order_acquire))
    if (v->ts <= ts) return v;
  return nullptr;
}

// IndexDef is a secondary index, stored as a hidden table whose rows are
// [primary key] under the entry key.
struct IndexDef {
  Index def;
  Table *tbl;
  // EntryKey writes the entry key for row with primary key pk and reports
  // whether it is a unique claim, which omits the primary key.
  bool EntryKey(Row row, std::string_view pk, std::string *out) const;
};

struct IndexSet {
  std::vector<IndexDef *> list;
  ColMask indexed = 0;  // columns covered by any index
};

struct Table {
  Table(int id, Schema s, int prefix);
  ~Table();

  int id;
  Schema schema;
  int width;
  ColMask all;  // every column
  Skiplist<Chain> rows;
  Skiplist<uint64_t> buckets;  // bucket key to lastTs; committer only
  int prefix;
  std::atomic<uint64_t> next_id{0};
  std::atomic<uint64_t> hidden{0};  // keys for tables without a primary key
  bool is_index = false;
  bool dropped = false;
  // live counts inserts minus deletes, for planning estimates.
  std::atomic<int64_t> live{0};
  std::vector<Check> checks;  // guarded by the commit lock
  // ixs holds the secondary indexes; DDL replaces it and keeps the old
  // sets, and every IndexDef, until destruction.
  std::atomic<const IndexSet *> ixs;
  std::vector<std::unique_ptr<IndexSet>> index_sets;
  std::vector<std::unique_ptr<IndexDef>> index_defs;

  const std::vector<IndexDef *> &indexes() const { return ixs.load(std::memory_order_acquire)->list; }
  ColMask indexed() const { return ixs.load(std::memory_order_acquire)->indexed; }

  // BucketOf maps a key to its bucket: the first prefix bytes, zero-padded,
  // with the low bits cleared. It is monotone, so a key range maps to a
  // contiguous bucket range.
  std::string BucketOf(std::string_view key, unsigned bits) const;
  // ChangedSince reports whether any bucket in [lo, hi] saw an insert or
  // delete after ts; an empty hi is unbounded.
  bool ChangedSince(std::string_view lo, std::string_view hi, uint64_t ts) const;
};

// Registry tracks the readTs of every active transaction so GC knows the
// oldest snapshot still in use. Begin loads lastCommitTs and registers under
// one mutex, so GC never computes a horizon that skips a starting
// transaction.
struct DB::Registry {
  struct Slot {
    uint64_t ts;
    int n;
  };
  std::mutex mu;
  std::vector<Slot> slots;  // ascending ts; slots[head:] are live
  size_t head = 0;

  uint64_t Begin(const std::atomic<uint64_t> &last);
  void End(uint64_t ts);
  // Horizon returns the oldest active readTs, or last when none is active.
  uint64_t Horizon(uint64_t last);
  Status Check();
};

// Durability is the commit queue between the committer and the writer.
struct DB::Durability {
  std::mutex mu;
  // work wakes the writer when commits queue; durable wakes the commits
  // waiting for a flush. Kept apart, a commit wakes only the writer.
  std::condition_variable work, durable_cond;
  std::vector<Commit> queue;
  size_t last_batch = 0;      // commits in the latest flush
  int64_t last_flush_ns = 0;  // how long its write and sync took, when gather and now are set
  uint64_t durable = 0;  // every commit at or below is synced
  bool closed = false;
  bool failed = false;  // a write or sync failed; the process must stop
  // limbo holds chains trimmed by the commit at the paired ts, freed once
  // the writer has persisted every version in them.
  std::deque<std::pair<uint64_t, Version *>> limbo;
};

}  // namespace celltnx
