// The commit protocol: validate, merge, derive index entries, enqueue for
// the writer, install, publish.
#include <deque>

#include "internal.h"

namespace celltnx {

Status Txn::Commit() {
  if (Status st = Install(); !st.ok()) return st;
  return WaitDurable();
}

Status Txn::WaitDurable() {
  if (bugReadOnlyNoWait && !Wrote()) return {};
  return db_->WaitDurable(serial_);
}

Status Txn::Install() {
  if (done_) return Code::kDone;
  done_ = true;
  DB *db = db_;
  std::vector<WSlot> ws;
  for (const WSlot &s : writes_)
    if (s.w != nullptr) ws.push_back(s);
  if (ws.empty()) {
    serial_ = read_ts_;
    EndSnapshot();
    return {};
  }
  const Options &o = db->opts_;
  bool timed = o.now && o.on_commit;
  int64_t t0 = 0, t1 = 0;
  if (timed) t0 = o.now();
  db->Lock();
  if (timed) t1 = o.now();
  Status st;
  try {
    st = CommitLocked(ws);
  } catch (...) {
    db->Unlock();
    EndSnapshot();
    throw;
  }
  if (timed) {
    int64_t t2 = o.now();
    db->Unlock();
    o.on_commit(t1 - t0, t2 - t1);
  } else {
    db->Unlock();
  }
  EndSnapshot();
  return st;
}

// InstallRec is one version to append: a merged row or a derived index
// entry.
struct Txn::InstallRec {
  Table *tb;
  std::string_view key;
  RowNode *n;
  Version *v;
  bool bucket;  // an insert or delete, which changes the key's bucket
};

// Trim drops versions that no active snapshot can see: it keeps every
// version newer than horizon plus the newest one at or below it, counts
// them into *kept, and returns the detached rest.
static Version *Trim(Version *v, uint64_t horizon, uint32_t *kept) {
  uint32_t k = 0;
  for (; v != nullptr; v = v->next.load(std::memory_order_relaxed)) {
    k++;
    if (v->ts <= horizon) break;
  }
  *kept = k;
  if (v == nullptr) return nullptr;
  return v->next.exchange(nullptr, std::memory_order_release);
}

Status Txn::CommitLocked(std::vector<WSlot> &ws) {
  DB *db = db_;
  uint64_t ts = db->last_.load(std::memory_order_relaxed) + 1;
  if (Status st = Validate(); !st.ok()) return st;
  std::vector<InstallRec> ins;
  ins.reserve(ws.size());
  std::deque<std::string> entry_keys;  // index entry keys, at stable addresses
  FlatMap<RowKey, int, RowKeyHash> entries;  // index entry to its position in ins
  auto discard = [&] {
    for (InstallRec &in : ins) Version::Free(in.v);
  };
  auto entry = [&](Table *tb, std::string key, Version *v) {
    RowKey k{tb->id, key};
    if (int *i = entries.Find(k)) {
      // Within one commit an entry may be both vacated and claimed; the
      // claim wins.
      if (!v->tombstone) {
        Version::Free(ins[*i].v);
        ins[*i].v = v;
      } else {
        Version::Free(v);
      }
      return;
    }
    entry_keys.push_back(std::move(key));
    std::string_view kv = entry_keys.back();
    entries.Insert(RowKey{tb->id, kv}, int(ins.size()));
    ins.push_back(InstallRec{tb, kv, tb->rows.Get(kv, nullptr), v, true});
  };
  std::string ok, nk;
  for (const WSlot &s : ws) {
    Table *tb = table(s.k.tbl);
    RowNode *n = s.n != nullptr ? s.n : tb->rows.Get(s.k.pk, nullptr);
    Row old;
    if (n != nullptr) old = n->val.head.load(std::memory_order_relaxed)->row();
    Version *v;
    if (Status st = Merge(tb, s, old, ts, &v); !st.ok()) {
      discard();
      return st;
    }
    ins.push_back(InstallRec{tb, s.k.pk, n, v, s.w->kind != Kind::kUpdate});
    if (s.w->kind == Kind::kUpdate && (v->mask & tb->indexed()) == 0) continue;  // no entry changed
    for (IndexDef *ix : tb->indexes()) {
      if (!old.empty()) ix->EntryKey(old, s.k.pk, &ok);
      if (!v->tombstone) ix->EntryKey(v->row(), s.k.pk, &nk);
      if (!old.empty() && !v->tombstone && ok == nk) continue;
      if (!old.empty()) entry(ix->tbl, ok, Version::Make(ts, ix->tbl->all | kExists, nullptr));
      if (!v->tombstone) {
        Value pkv = Value::Str(s.k.pk);
        Row r(&pkv, 1);
        entry(ix->tbl, nk, Version::Make(ts, ix->tbl->all | kExists, &r));
      }
    }
  }
  if (on_install) on_install(ts);
  uint64_t horizon = db->readers_->Horizon(ts - 1);
  if (bugGCEager) horizon = ts - 1;
  if (db->opts_.store != nullptr) {
    struct Commit c;
    c.ts = ts;
    size_t bytes = 0;
    for (const InstallRec &in : ins)
      if (!in.tb->is_index) bytes += in.key.size();
    c.keys.resize(bytes);
    char *p = c.keys.data();
    for (const InstallRec &in : ins) {
      if (in.tb->is_index) continue;
      if (!in.key.empty()) memcpy(p, in.key.data(), in.key.size());
      c.writes.push_back(WriteRef{in.tb->id, std::string_view(p, in.key.size()), in.v});
      p += in.key.size();
    }
    db->Enqueue(std::move(c));
  }
  if (bugPublishEarly) db->last_.store(ts, std::memory_order_release);
  std::vector<Version *> tails;
  for (InstallRec &in : ins) {
    db->Interleave();
    RowNode *n = in.n != nullptr ? in.n : in.tb->rows.GetOrInsert(in.key, db->interleave_hook());
    Chain &c = n->val;
    Version *prev = c.head.load(std::memory_order_relaxed);
    if (prev != nullptr && prev->mask == in.v->mask) in.v->run_ts = prev->run_ts;
    in.v->next.store(prev, std::memory_order_relaxed);
    c.head.store(in.v, std::memory_order_release);
    if (++c.installs >= c.kept) {
      c.installs = 0;
      if (Version *tail = Trim(in.v, horizon, &c.kept)) tails.push_back(tail);
    }
    if (in.bucket) {
      in.tb->buckets.GetOrInsert(in.tb->BucketOf(in.key, db->opts_.bucket_bits), nullptr)->val = ts;
      in.tb->live.fetch_add(in.v->tombstone ? -1 : 1, std::memory_order_relaxed);
    }
  }
  db->Interleave();
  db->last_.store(ts, std::memory_order_release);
  serial_ = ts;
  db->Retire(ts, tails);
  return {};
}

Status Txn::Validate() {
  DB *db = db_;
  for (const Read &r : reads_) {
    RowNode *n = r.n;
    if (n == nullptr) {
      // The key was absent when read; a later commit may have added it.
      if ((n = table(r.k.tbl)->rows.Get(r.k.pk, nullptr)) == nullptr) continue;
    }
    for (Version *v = n->val.head.load(std::memory_order_relaxed); v != nullptr && v->ts > read_ts_;
         v = v->next.load(std::memory_order_relaxed)) {
      if (ColMask c = v->mask & r.mask)
        return Error{.code = Code::kConflict, .table = r.k.tbl, .pk = std::string(r.k.pk), .cols = c};
      if (v->run_ts <= read_ts_) break;  // the rest down to read_ts_ share v's mask
    }
  }
  if (bugSkipBuckets) return {};
  for (const ScanRange &s : scans_)
    if (db->table(s.tbl)->ChangedSince(s.lo, s.hi, read_ts_))
      return Error{.code = Code::kConflict, .table = s.tbl, .bucket = true};
  return {};
}

Status Txn::Merge(Table *tb, const WSlot &s, Row old, uint64_t ts, Version **out) {
  const Write *w = s.w;
  ColMask checks = 0;
  Version *v = nullptr;
  auto violated = [&](std::string check) -> Status {
    if (v != nullptr) Version::Free(v);
    return Error{.code = Code::kConstraint, .table = tb->id, .pk = std::string(s.k.pk), .check = std::move(check),
                 .after_ts = ts - 1};
  };
  switch (w->kind) {
    case Kind::kDelete:
      *out = Version::Make(ts, tb->all | kExists, nullptr);
      return {};
    case Kind::kInsert: {
      Row row(w->vals, w->width);
      v = Version::Make(ts, tb->all | kExists, &row);
      checks = tb->all;
      break;
    }
    case Kind::kUpdate: {
      db_->Invariant(!old.empty(), "update of missing row in table " + tb->schema.name);
      Value *row = arena_.Array<Value>(tb->width);
      for (int c = 0; c < tb->width; c++) row[c] = size_t(c) < old.size() ? old[c] : Value();
      EachCol(w->set, [&](int c) { row[c] = w->vals[c]; });
      Code err = Code::kOk;
      EachCol(w->delta, [&](int c) {
        if (err == Code::kOk) err = AddValue(row[c], w->deltas[c], &row[c]);
      });
      if (err != Code::kOk) return violated(Error{.code = err}.Message());
      Row r(row, tb->width);
      v = Version::Make(ts, w->set | w->delta, &r);
      checks = v->mask;
      if (bugCheckSkipDelta && w->set == 0) checks = 0;
      break;
    }
  }
  if (size_t max = db_->opts_.max_row_bytes; max != 0 && EncodedSize(v->row()) > max)
    return violated("row size limit");
  for (const Check &c : tb->checks)
    if ((c.cols & checks) != 0 && !c.ok(v->row())) return violated(c.name);
  *out = v;
  return {};
}

}  // namespace celltnx
