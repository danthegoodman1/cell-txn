// Transaction reads and writes. Every interleave and yield call here sits
// where the Go core makes it, in the same order: the simulator draws its
// randomness at each one, so traces match only if they do.
#include <algorithm>

#include "internal.h"

namespace celltnx {

size_t Txn::RowKeyHash::operator()(const RowKey &k) const {
  uint64_t h = std::hash<std::string_view>{}(k.pk) ^ (uint64_t(k.tbl) * 0x9e3779b97f4a7c15ull);
  h ^= h >> 29;
  return size_t(h);
}

Txn::~Txn() { Abort(); }

void Txn::Record(const RowKey &k, RowNode *n, ColMask mask) {
  if (db_->opts_.mode == Mode::kRow) mask = table(k.tbl)->all | kExists;
  if (int *i = rpos_.Find(k)) {
    Read &r = reads_[*i];
    r.mask |= mask;
    if (r.n == nullptr) r.n = n;
    return;
  }
  RowKey kk{k.tbl, arena_.Copy(k.pk)};
  rpos_.Insert(kk, int(reads_.size()));
  reads_.push_back({kk, n, mask});
}

Txn::Write *Txn::Pending(const RowKey &k) const {
  const int *i = wpos_.Find(k);
  return i ? writes_[*i].w : nullptr;
}

Txn::Write *Txn::Seen(int i) const {
  const WSlot &s = writes_[i];
  return s.stmt_epoch == stmt_epoch_ ? s.stmt_prev : s.w;
}

Txn::Write *Txn::SeenPending(const RowKey &k) const {
  const int *i = wpos_.Find(k);
  return i ? Seen(*i) : nullptr;
}

void Txn::BeginStatement() {
  if (stmt_ == 0) stmt_epoch_++;
  stmt_++;
}

void Txn::EndStatement() {
  if (stmt_ > 0 && --stmt_ == 0) stmt_epoch_++;
}

void Txn::Put(const RowKey &k, RowNode *n, Write *w) {
  int i;
  if (int *p = wpos_.Find(k)) {
    i = *p;
  } else {
    i = int(writes_.size());
    RowKey kk{k.tbl, arena_.Copy(k.pk)};
    wpos_.Insert(kk, i);
    writes_.push_back(WSlot{.k = kk});
  }
  WSlot &s = writes_[i];
  if (s.n == nullptr) s.n = n;
  if (stmt_ > 0 && s.stmt_epoch != stmt_epoch_) {
    s.stmt_prev = s.w;
    s.stmt_epoch = stmt_epoch_;
  }
  undo_.push_back({i, s.w});
  s.w = w;
}

Row Txn::Snapshot(RowNode *n) const {
  if (Version *v = Visible(n, read_ts_); v != nullptr) return v->row();
  return {};
}

Status Txn::View(RowNode *n, const Write *w, Row *out) {
  if (w == nullptr) {
    *out = Snapshot(n);
    return {};
  }
  switch (w->kind) {
    case Kind::kInsert:
      *out = Row(w->vals, w->width);
      return {};
    case Kind::kDelete:
      *out = {};
      return {};
    case Kind::kUpdate:
      break;
  }
  Row base = Snapshot(n);
  db_->Invariant(!base.empty(), "pending update on a row missing from the snapshot");
  Value *row = arena_.Array<Value>(w->width);
  for (uint32_t c = 0; c < w->width; c++) row[c] = c < base.size() ? base[c] : Value();
  EachCol(w->set, [&](int c) { row[c] = w->vals[c]; });
  Code err = Code::kOk;
  EachCol(w->delta, [&](int c) {
    if (err == Code::kOk) err = AddValue(row[c], w->deltas[c], &row[c]);
  });
  if (err != Code::kOk) return err;
  *out = Row(row, w->width);
  return {};
}

ColMask Txn::ReadMask(ColMask cols, const Write *w) const {
  if (w == nullptr) return cols | kExists;
  if (w->kind != Kind::kUpdate) return 0;
  if (bugDeltaReadUntracked) return (cols & ~(w->set | w->delta)) | kExists;
  return (cols & ~w->set) | kExists;
}

static void Project(Row row, ColMask cols, std::vector<Value> *out) {
  out->assign(row.size(), Value());
  EachCol(cols, [&](int c) { (*out)[c] = row[c]; });
}

void Txn::CheckCols(int tbl, ColMask cols) const {
  if (cols & ~table(tbl)->all) throw InvariantError("txn: table " + std::to_string(tbl) + " has no such columns");
}

static bool InRange(std::string_view k, std::string_view lo, std::string_view hi) {
  return k >= lo && (hi.empty() || k < hi);
}

Status Txn::Get(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found) {
  return GetImpl(tbl, pk, cols, true, false, false, row, found);
}

Status Txn::GetUntracked(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found) {
  return GetImpl(tbl, pk, cols, false, false, false, row, found);
}

Status Txn::GetRow(int tbl, std::string_view pk, ColMask track, std::vector<Value> *row, bool *found) {
  return GetImpl(tbl, pk, track, true, true, false, row, found);
}

Status Txn::CurrentRow(int tbl, std::string_view pk, ColMask track, std::vector<Value> *row, bool *found) {
  return GetImpl(tbl, pk, track, true, true, true, row, found);
}

Status Txn::GetImpl(int tbl, std::string_view pk, ColMask cols, bool track, bool full, bool current,
                    std::vector<Value> *row, bool *found) {
  *found = false;
  row->clear();
  if (done_) return Code::kDone;
  cols &= ~kExists;
  CheckCols(tbl, cols);
  RowKey k{tbl, pk};
  Write *w = current ? Pending(k) : SeenPending(k);
  RowNode *n = table(tbl)->rows.Get(pk, db_->interleave_hook());
  Row r;
  if (Status st = View(n, w, &r); !st.ok()) return st;
  if (track) {
    if (!r.empty()) {
      if (ColMask m = ReadMask(cols, w)) Record(k, n, m);
    } else if (w == nullptr && !bugNoAbsentRead) {
      Record(k, n, kExists);
    }
  }
  if (r.empty()) return {};
  *found = true;
  if (full)
    row->assign(r.begin(), r.end());
  else
    Project(r, cols, row);
  return {};
}

void Txn::Visit(const RowKey &k, RowNode *n, const Write *w, Row row, const ScanSpec &sp, std::vector<Rec> *out) {
  if (ColMask m = ReadMask(sp.pred, w)) Record(k, n, m);
  bool matched = true;
  if (sp.match != nullptr && *sp.match) {
    std::vector<Value> p;
    Project(row, sp.pred, &p);
    matched = (*sp.match)(k.pk, p);
  }
  if (!matched) return;
  if (ColMask m = ReadMask(sp.proj, w)) Record(k, n, m);
  Rec rec{k.pk, {}};
  if (sp.full)
    rec.row.assign(row.begin(), row.end());
  else
    Project(row, sp.pred | sp.proj, &rec.row);
  out->push_back(std::move(rec));
}

Status Txn::Own(int tbl, const std::function<std::string_view(std::string_view pk, Row row)> &key,
                std::string_view lo, std::string_view hi, std::vector<OwnRow> *out) {
  out->clear();
  for (size_t i = 0; i < writes_.size(); i++) {
    WSlot s = writes_[i];
    s.w = Seen(int(i));
    if (s.k.tbl != tbl || s.w == nullptr) continue;
    Row row;
    if (Status st = View(s.n, s.w, &row); !st.ok()) return st;
    if (row.empty()) continue;
    if (std::string_view k = key(s.k.pk, row); InRange(k, lo, hi)) out->push_back(OwnRow{k, s, row});
  }
  // Keys are unique: primary keys, or entry keys that end in the primary
  // key unless a unique claim, which no two pending rows share.
  std::stable_sort(out->begin(), out->end(), [](const OwnRow &a, const OwnRow &b) { return a.key < b.key; });
  return {};
}

void Txn::RecordRange(Table *tb, std::string_view lo, std::string_view hi) {
  unsigned bits = db_->opts_.bucket_bits;
  ScanRange r{tb->id, arena_.Copy(tb->BucketOf(lo, bits)), {}};
  if (!hi.empty()) r.hi = arena_.Copy(tb->BucketOf(hi, bits));
  scans_.push_back(r);
}

Status Txn::Scan(int tbl, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj, const Match &match,
                 std::vector<Rec> *out) {
  return ScanImpl(tbl, lo, hi, ScanSpec{pred, proj, &match, false}, out);
}

Status Txn::ScanRows(int tbl, int idx, std::string_view lo, std::string_view hi, ColMask track,
                     std::vector<Rec> *out) {
  ScanSpec sp{track, 0, nullptr, true};
  if (idx < 0) return ScanImpl(tbl, lo, hi, sp, out);
  return IndexScanImpl(tbl, idx, lo, hi, sp, out);
}

Status Txn::ScanImpl(int tbl, std::string_view lo, std::string_view hi, ScanSpec sp, std::vector<Rec> *out) {
  out->clear();
  if (done_) return Code::kDone;
  sp.pred &= ~kExists;
  sp.proj &= ~kExists;
  CheckCols(tbl, sp.pred | sp.proj);
  Table *tb = table(tbl);
  RecordRange(tb, lo, hi);
  std::vector<OwnRow> own;
  if (Status st = Own(tbl, [](std::string_view pk, Row) { return pk; }, lo, hi, &own); !st.ok()) return st;
  RowNode *n = tb->rows.Seek(lo, nullptr, db_->interleave_hook());
  size_t oi = 0;
  for (;;) {
    while (n != nullptr && InRange(n->key(), lo, hi) && SeenPending(RowKey{tbl, n->key()}) != nullptr)
      n = n->Next();  // own writes come from the own list
    RowKey k;
    RowNode *nd;
    const Write *w = nullptr;
    Row row;
    if (n != nullptr && InRange(n->key(), lo, hi) && (oi == own.size() || n->key() < own[oi].key)) {
      k = RowKey{tbl, n->key()};
      nd = n;
      row = Snapshot(n);
      n = n->Next();
    } else if (oi < own.size()) {
      k = own[oi].s.k;
      nd = own[oi].s.n;
      w = own[oi].s.w;
      row = own[oi].row;
      oi++;
    } else {
      return {};
    }
    if (!row.empty()) Visit(k, nd, w, row, sp, out);
    db_->Point();
  }
}

Status Txn::IndexScan(int tbl, int idx, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj,
                      const Match &match, std::vector<Rec> *out) {
  return IndexScanImpl(tbl, idx, lo, hi, ScanSpec{pred, proj, &match, false}, out);
}

Status Txn::IndexScanImpl(int tbl, int idx, std::string_view lo, std::string_view hi, ScanSpec sp,
                          std::vector<Rec> *out) {
  out->clear();
  if (done_) return Code::kDone;
  sp.pred &= ~kExists;
  sp.proj &= ~kExists;
  CheckCols(tbl, sp.pred | sp.proj);
  Table *tb = table(tbl);
  IndexDef *ix = tb->indexes()[idx];
  if (!bugIndexNoRange) RecordRange(ix->tbl, lo, hi);
  std::vector<OwnRow> own;
  std::string scratch;
  auto entry = [&](std::string_view pk, Row row) {
    ix->EntryKey(row, pk, &scratch);
    return arena_.Copy(scratch);
  };
  if (Status st = Own(tbl, entry, lo, hi, &own); !st.ok()) return st;
  sp.pred |= ix->def.cols;
  RowNode *e = ix->tbl->rows.Seek(lo, nullptr, db_->interleave_hook());
  size_t oi = 0;
  for (;;) {
    Version *ent = nullptr;
    for (; e != nullptr && InRange(e->key(), lo, hi); e = e->Next()) {
      ent = Visible(e, read_ts_);
      if (ent != nullptr && !ent->tombstone && SeenPending(RowKey{tbl, ent->row()[0].str()}) == nullptr) break;
      ent = nullptr;  // missing at readTs, or a row with own writes, which come from the own list
    }
    RowKey k;
    RowNode *nd;
    const Write *w = nullptr;
    Row row;
    if (ent != nullptr && (oi == own.size() || e->key() < own[oi].key)) {
      k = RowKey{tbl, ent->row()[0].str()};
      nd = tb->rows.Get(k.pk, db_->interleave_hook());
      row = Snapshot(nd);
      db_->Invariant(!row.empty(), "index " + ix->tbl->schema.name + " entry has no row");
      e = e->Next();
    } else if (oi < own.size()) {
      k = own[oi].s.k;
      nd = own[oi].s.n;
      w = own[oi].s.w;
      row = own[oi].row;
      oi++;
    } else {
      return {};
    }
    if (!row.empty()) Visit(k, nd, w, row, sp, out);
    db_->Point();
  }
}

Txn::Write *Txn::Existing(const RowKey &k, RowNode **n, bool *exists) {
  *n = table(k.tbl)->rows.Get(k.pk, db_->interleave_hook());
  if (Write *w = Pending(k)) {
    *exists = w->kind != Kind::kDelete;
    return w;
  }
  Record(k, *n, kExists);
  *exists = !Snapshot(*n).empty();
  return nullptr;
}

Txn::Write *Txn::ExistingForSet(const RowKey &k, RowNode **n, bool *exists) {
  if (!bugSetNoExists) return Existing(k, n, exists);
  *n = table(k.tbl)->rows.Get(k.pk, db_->interleave_hook());
  if (Write *w = Pending(k)) {
    *exists = w->kind != Kind::kDelete;
    return w;
  }
  *exists = !Snapshot(*n).empty();
  return nullptr;
}

Status Txn::CheckUnique(Table *tb, std::string_view pk, Row row, ColMask written) {
  std::string k, k2;
  for (IndexDef *ix : tb->indexes()) {
    if (!ix->def.unique || (ix->def.cols & written) == 0) continue;
    k.clear();
    if (!ix->def.key(row, &k)) continue;
    RowNode *en = ix->tbl->rows.Get(k, db_->interleave_hook());
    if (!bugUniqueNoRead) Record(RowKey{ix->tbl->id, k}, en, kExists);
    std::string_view owner;
    if (Version *v = Visible(en, read_ts_); v != nullptr && !v->tombstone) {
      std::string_view o = v->row()[0].str();
      if (o != pk && Pending(RowKey{tb->id, o}) == nullptr) owner = o;
    }
    for (const WSlot &s : writes_) {
      if (!owner.empty()) break;
      if (s.k.tbl != tb->id || s.k.pk == pk || s.w == nullptr) continue;
      Row r;
      if (View(s.n, s.w, &r).ok() && !r.empty()) {
        k2.clear();
        if (ix->def.key(r, &k2) && k2 == k) owner = s.k.pk;
      }
    }
    if (!owner.empty())
      return Error{.code = Code::kDuplicateKey, .table = tb->id, .index = ix->def.name, .key = k, .owner = std::string(owner)};
  }
  return {};
}

Txn::Write *Txn::NewWrite(Kind kind, bool existed, uint32_t width) {
  Write *w = new (arena_.Alloc(sizeof(Write), alignof(Write))) Write{kind, existed};
  w->width = width;
  if (kind == Kind::kUpdate) {
    w->vals = arena_.Array<Value>(width);
    w->deltas = arena_.Array<int64_t>(width);
    for (uint32_t c = 0; c < width; c++) {
      new (&w->vals[c]) Value();
      w->deltas[c] = 0;
    }
  }
  return w;
}

Txn::Write *Txn::Clone(const Write *w) {
  Write *c = new (arena_.Alloc(sizeof(Write), alignof(Write))) Write(*w);
  if (w->vals != nullptr) {
    c->vals = arena_.Array<Value>(w->width);
    std::copy(w->vals, w->vals + w->width, c->vals);
  }
  if (w->deltas != nullptr) {
    c->deltas = arena_.Array<int64_t>(w->width);
    std::copy(w->deltas, w->deltas + w->width, c->deltas);
  }
  return c;
}

// Keep copies a caller's string into the arena, so a pending write never
// points at the caller's memory.
static Value Keep(Arena &a, const Value &v) {
  if (v.type != Type::kString) return v;
  return Value::Str(a.Copy(v.str()));
}

Status Txn::CheckSize(int tbl, std::string_view pk, Row row) const {
  const Options &o = db_->opts_;
  if (o.max_key_bytes != 0 && pk.size() > o.max_key_bytes) return Code::kTooLarge;
  if (o.max_row_bytes != 0 && EncodedSize(row) > o.max_row_bytes) return Code::kTooLarge;
  (void)tbl;
  return {};
}

Status Txn::Update(int tbl, std::string_view pk, ColMask written, const std::function<Status(Write *)> &fn) {
  if (done_) return Code::kDone;
  Table *tb = table(tbl);
  RowKey k{tbl, pk};
  RowNode *n;
  bool ok;
  Write *w = ExistingForSet(k, &n, &ok);
  if (!ok) return Code::kNotFound;
  w = w == nullptr ? NewWrite(Kind::kUpdate, true, uint32_t(tb->width)) : Clone(w);
  if (Status st = fn(w); !st.ok()) return st;
  if (written & tb->indexed()) {
    Row row;
    if (Status st = View(n, w, &row); !st.ok()) return st;
    if (Status st = CheckUnique(tb, pk, row, written); !st.ok()) return st;
  }
  if (db_->opts_.max_row_bytes != 0) {
    Row row;
    if (Status st = View(n, w, &row); !st.ok()) return st;
    if (Status st = CheckSize(tbl, pk, row); !st.ok()) return st;
  }
  Put(k, n, w);
  return {};
}

Status Txn::Set(int tbl, std::string_view pk, ColMask cols, Row vals) {
  cols &= ~kExists;
  CheckCols(tbl, cols);
  if (vals.size() != size_t(table(tbl)->width)) throw InvariantError("txn: Set vals have the wrong width");
  return Update(tbl, pk, cols, [&](Write *w) -> Status {
    if (w->kind == Kind::kUpdate) {
      // Overwriting a pending delta makes the statements that checked its
      // intermediate value depend on the base value.
      if (ColMask superseded = w->delta & cols)
        Record(RowKey{tbl, pk}, table(tbl)->rows.Get(pk, db_->interleave_hook()), superseded);
      w->set |= cols;
      w->delta &= ~cols;
    }
    EachCol(cols, [&](int c) { w->vals[c] = Keep(arena_, vals[c]); });
    return {};
  });
}

Status Txn::Add(int tbl, std::string_view pk, int col, int64_t delta) {
  CheckCols(tbl, Col(col));
  Table *tb = table(tbl);
  return Update(tbl, pk, Col(col), [&](Write *w) -> Status {
    if (w->kind == Kind::kUpdate && (w->set & Col(col)) == 0) {
      if (db_->opts_.mode != Mode::kCellDelta || (tb->indexed() & Col(col)))
        Record(RowKey{tbl, pk}, tb->rows.Get(pk, db_->interleave_hook()), Col(col));
      int64_t s;
      if (!AddDelta(w->deltas[col], delta, &s)) return Code::kOverflow;
      w->delta |= Col(col);
      w->deltas[col] = s;
      return {};
    }
    Value v;
    Code c = AddValue(w->vals[col], delta, &v);
    w->vals[col] = v;
    return c;
  });
}

Status Txn::Insert(int tbl, std::string_view pk, Row row) {
  if (done_) return Code::kDone;
  Table *tb = table(tbl);
  if (row.size() != size_t(tb->width)) throw InvariantError("txn: Insert row has the wrong width");
  if (Status st = CheckSize(tbl, pk, row); !st.ok()) return st;
  RowKey k{tbl, pk};
  RowNode *n;
  bool ok;
  Write *w = Existing(k, &n, &ok);
  if (ok)
    return Error{.code = Code::kDuplicateKey, .table = tbl, .index = "PRIMARY", .key = std::string(pk), .owner = std::string(pk)};
  Value *vals = arena_.Array<Value>(row.size());
  for (size_t c = 0; c < row.size(); c++) vals[c] = Keep(arena_, row[c]);
  if (Status st = CheckUnique(tb, pk, Row(vals, row.size()), tb->all); !st.ok()) return st;
  Write *nw = NewWrite(Kind::kInsert, w != nullptr && w->existed, uint32_t(row.size()));
  nw->vals = vals;
  Put(k, n, nw);
  return {};
}

Status Txn::Delete(int tbl, std::string_view pk) {
  if (done_) return Code::kDone;
  RowKey k{tbl, pk};
  RowNode *n;
  bool ok;
  Write *w = Existing(k, &n, &ok);
  if (!ok) return Code::kNotFound;
  if (w != nullptr && w->kind == Kind::kInsert && !w->existed) {
    Put(k, n, nullptr);
    return {};
  }
  if (w != nullptr && w->kind == Kind::kUpdate && w->delta != 0) {
    // Deleting the row discards pending deltas; the statements that checked
    // their intermediate values depend on the base value.
    Record(k, n, w->delta);
  }
  Put(k, n, NewWrite(Kind::kDelete, true, 0));
  return {};
}

void Txn::RollbackTo(int sp) {
  while (int(undo_.size()) > sp) {
    Undo u = undo_.back();
    undo_.pop_back();
    writes_[u.i].w = u.prev;
  }
}

void Txn::Reset(uint64_t read_ts) {
  read_ts_ = read_ts;
  serial_ = 0;
  done_ = false;
  pinned_ = false;
  arena_.Reset();
  // Like the maps, the vectors keep small allocations and release large
  // ones.
  auto reset = [](auto &v) {
    if (v.capacity() > 4096)
      std::remove_reference_t<decltype(v)>().swap(v);
    else
      v.clear();
  };
  reset(reads_);
  rpos_.Reset();
  reset(scans_);
  reset(writes_);
  wpos_.Reset();
  reset(undo_);
  stmt_ = 0;
  stmt_epoch_++;
  on_install = nullptr;
}

void Txn::Abort() {
  if (!done_) {
    done_ = true;
    EndSnapshot();
  }
}

void Txn::EndSnapshot() {
  db_->readers_->End(read_ts_);
  if (pinned_) db_->readers_->End(pinned_ts_);
}

bool Txn::Changed(int tbl, std::string_view pk) const {
  RowNode *n = table(tbl)->rows.Get(pk, nullptr);
  return n != nullptr && n->val.head.load(std::memory_order_acquire)->ts > read_ts_;
}

Status Txn::Refresh() {
  if (done_) return Code::kDone;
  DB *db = db_;
  Status st;
  uint64_t ts = 0;
  if (reads_.empty() && scans_.empty()) {
    // Nothing read means nothing to validate: every write records a read,
    // so the snapshot moves as a new transaction's would.
    ts = db->readers_->Begin(db->last_);
  } else {
    // Buckets change only under the commit lock, and holding it keeps the
    // latest commit fixed while the reads validate against it.
    db->Lock();
    try {
      if (!bugRefreshNoValidate) st = Validate();
      if (st.ok()) ts = db->readers_->Begin(db->last_);
    } catch (...) {
      db->Unlock();
      throw;
    }
    db->Unlock();
  }
  if (!st.ok()) return st;
  if (pinned_) {
    db->readers_->End(read_ts_);
  } else {
    pinned_ = true;
    pinned_ts_ = read_ts_;
  }
  read_ts_ = ts;
  return {};
}

}  // namespace celltnx
