#include "db.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <set>

#include "internal.h"

namespace celltnx {

const char *ModeName(Mode m) {
  switch (m) {
    case Mode::kRow:
      return "row";
    case Mode::kCell:
      return "cell";
    case Mode::kCellDelta:
      return "cell+delta";
  }
  return "?";
}

static std::string Hex(std::string_view s) {
  std::string out;
  char buf[3];
  for (unsigned char c : s) {
    snprintf(buf, sizeof buf, "%02x", c);
    out += buf;
  }
  return out;
}

std::string Error::Message() const {
  char buf[64];
  switch (code) {
    case Code::kOk:
      return "ok";
    case Code::kNotFound:
      return "txn: row not found";
    case Code::kDuplicateKey:
      return "txn: duplicate key " + Hex(key) + " in table " + std::to_string(table) + " index " + index;
    case Code::kOverflow:
      return "txn: integer overflow";
    case Code::kNotInteger:
      return "txn: delta on a non-integer value";
    case Code::kDone:
      return "txn: transaction already finished";
    case Code::kConflict:
      if (bucket) return "txn: conflict on a scanned range of table " + std::to_string(table);
      snprintf(buf, sizeof buf, "%#" PRIx64, uint64_t(cols));
      return "txn: conflict on table " + std::to_string(table) + " row " + Hex(pk) + " cols " + buf;
    case Code::kConstraint:
      return "txn: table " + std::to_string(table) + " row " + Hex(pk) + " violates " + check;
    case Code::kClosed:
      return "txn: database closed";
    case Code::kTooLarge:
      return "txn: key or row too large";
    case Code::kIo:
      return "txn: store failed: " + detail;
    case Code::kInternal:
      return detail;
  }
  return "txn: unknown error";
}

Code AddValue(const Value &v, int64_t delta, Value *out) {
  switch (v.type) {
    case Type::kInt: {
      int64_t s = int64_t(uint64_t(v.i) + uint64_t(delta));
      if ((s > v.i) != (delta > 0)) return Code::kOverflow;
      *out = Value::Int(s);
      return Code::kOk;
    }
    case Type::kUint: {
      uint64_t s = v.u + uint64_t(delta);
      if ((delta >= 0 && s < v.u) || (delta < 0 && s > v.u)) return Code::kOverflow;
      *out = Value::Uint(s);
      return Code::kOk;
    }
    default:
      return Code::kNotInteger;
  }
}

bool AddDelta(int64_t a, int64_t b, int64_t *out) {
  int64_t s = int64_t(uint64_t(a) + uint64_t(b));
  *out = s;
  return (s > a) == (b > 0);
}

Version *Version::Make(uint64_t ts, ColMask mask, const Row *row) {
  size_t width = row ? row->size() : 0;
  size_t size = sizeof(Version) + width * sizeof(Value) + (row ? StringBytes(*row) : 0);
  void *mem = malloc(size);
  if (mem == nullptr) throw std::bad_alloc();
  Version *v = new (mem) Version();
  v->ts = ts;
  v->mask = mask;
  v->run_ts = ts;
  if (row != nullptr) {
    v->width = uint32_t(width);
    v->tombstone = false;
    CopyRow(*row, v->vals(), reinterpret_cast<char *>(v->vals() + width));
  }
  return v;
}

void Version::Free(Version *v) {
  v->~Version();
  free(v);
}

void Version::FreeChain(Version *v) {
  while (v != nullptr) {
    Version *next = v->next.load(std::memory_order_relaxed);
    Free(v);
    v = next;
  }
}

// --- tables and indexes ---

bool IndexDef::EntryKey(Row row, std::string_view pk, std::string *out) const {
  out->clear();
  bool u = def.key(row, out);
  if (def.unique && u) return true;
  out->append(pk);
  return false;
}

Table::Table(int id, Schema s, int prefix)
    : id(id), schema(std::move(s)), width(int(schema.cols.size())), all(Col(width) - 1),
      prefix(prefix == 0 ? 8 : prefix), checks(schema.checks) {
  if (width == 0 || width > kMaxCols)
    throw InvariantError("txn: table " + schema.name + " has " + std::to_string(width) + " columns");
  index_sets.push_back(std::make_unique<IndexSet>());
  ixs.store(index_sets.back().get());
}

Table::~Table() {
  for (RowNode *n = rows.First(); n != nullptr; n = n->Next()) Version::FreeChain(n->val.head.load());
}

std::string Table::BucketOf(std::string_view key, unsigned bits) const {
  std::string b(prefix, '\0');
  memcpy(b.data(), key.data(), std::min<size_t>(prefix, key.size()));
  for (int i = prefix - 1; i >= 0 && bits > 0; i--) {
    if (bits >= 8) {
      b[i] = 0;
      bits -= 8;
    } else {
      b[i] = char(uint8_t(b[i]) & ~uint8_t((1u << bits) - 1));
      bits = 0;
    }
  }
  return b;
}

bool Table::ChangedSince(std::string_view lo, std::string_view hi, uint64_t ts) const {
  for (auto *n = buckets.Seek(lo, nullptr, nullptr); n != nullptr && (hi.empty() || n->key() <= hi); n = n->Next())
    if (n->val > ts) return true;
  return false;
}

// --- the snapshot registry ---

uint64_t DB::Registry::Begin(const std::atomic<uint64_t> &last) {
  std::lock_guard g(mu);
  uint64_t ts = last.load(std::memory_order_acquire);
  if (size_t k = slots.size(); k > head && slots[k - 1].ts == ts)
    slots[k - 1].n++;
  else
    slots.push_back({ts, 1});
  return ts;
}

void DB::Registry::End(uint64_t ts) {
  std::lock_guard g(mu);
  auto live = slots.begin() + head;
  auto it = std::lower_bound(live, slots.end(), ts, [](const Slot &s, uint64_t t) { return s.ts < t; });
  if (it == slots.end() || it->ts != ts || it->n == 0) throw InvariantError("txn: registry end without begin");
  it->n--;
  while (head < slots.size() && slots[head].n == 0) head++;
  if (head > 64 && head * 2 > slots.size()) {
    slots.erase(slots.begin(), slots.begin() + head);
    head = 0;
  }
}

uint64_t DB::Registry::Horizon(uint64_t last) {
  std::lock_guard g(mu);
  return head == slots.size() ? last : slots[head].ts;
}

Status DB::Registry::Check() {
  std::lock_guard g(mu);
  for (size_t i = head + 1; i < slots.size(); i++)
    if (slots[i - 1].ts >= slots[i].ts)
      return Error{.code = Code::kInternal, .detail = "txn: registry slots out of order at " + std::to_string(i)};
  return {};
}

// --- the DB ---

DB::DB(Options opts, std::vector<Schema> schemas)
    : opts_(std::move(opts)), readers_(std::make_unique<Registry>()), dur_(std::make_unique<Durability>()) {
  table_lists_.push_back(std::make_unique<std::vector<Table *>>());
  tables_.store(table_lists_.back().get());
  for (size_t i = 0; i < schemas.size(); i++) {
    int prefix = schemas[i].bucket_prefix;
    AppendTable(new Table(int(i), schemas[i], prefix));
  }
  for (size_t i = 0; i < schemas.size(); i++)
    for (auto &d : schemas[i].indexes) AddIndex(int(i), d);
}

DB::~DB() {
  for (Table *t : *tables_.load()) delete t;
  for (auto &[ts, tail] : dur_->limbo) Version::FreeChain(tail);
  for (Version *v : kept_) Version::FreeChain(v);
}

std::unique_ptr<Txn> DB::Begin(std::unique_ptr<Txn> reuse) {
  if (reuse == nullptr || reuse->db_ != this) return std::unique_ptr<Txn>(new Txn(this, readers_->Begin(last_)));
  reuse->Abort();
  reuse->Reset(readers_->Begin(last_));
  return reuse;
}

void DB::AppendTable(Table *t) {
  auto next = std::make_unique<std::vector<Table *>>(*tables_.load());
  next->push_back(t);
  tables_.store(next.get(), std::memory_order_release);
  table_lists_.push_back(std::move(next));
}

IndexDef *DB::AddIndex(int tbl, Index d) {
  Table *t = table(tbl);
  int id = int(tables_.load()->size());
  auto *it = new Table(id, Schema{.name = t->schema.name + "." + d.name, .cols = {"pk"}}, d.bucket_prefix);
  it->is_index = true;
  AppendTable(it);
  t->index_defs.push_back(std::make_unique<IndexDef>(IndexDef{std::move(d), it}));
  IndexDef *ix = t->index_defs.back().get();
  const IndexSet *old = t->ixs.load();
  auto set = std::make_unique<IndexSet>(*old);
  set->list.push_back(ix);
  set->indexed |= ix->def.cols;
  t->ixs.store(set.get(), std::memory_order_release);
  t->index_sets.push_back(std::move(set));
  return ix;
}

int DB::CreateTable(Schema s) {
  Lock();
  int id = int(tables_.load()->size());
  int prefix = s.bucket_prefix;
  std::vector<Index> indexes = s.indexes;
  AppendTable(new Table(id, std::move(s), prefix));
  for (auto &d : indexes) AddIndex(id, d);
  Unlock();
  return id;
}

Status DB::CreateIndex(int tbl, Index d) {
  Lock();
  Table *t = table(tbl);
  uint64_t last = last_.load();
  IndexDef tmp{d, nullptr};
  std::set<std::string> seen;
  std::vector<std::pair<std::string, std::string>> ents;
  std::string k;
  for (RowNode *n = t->rows.First(); n != nullptr; n = n->Next()) {
    if (Version *v = Visible(n, last); v != nullptr && !v->tombstone) {
      tmp.EntryKey(v->row(), n->key(), &k);
      if (!seen.insert(k).second) {
        Unlock();
        return Error{.code = Code::kDuplicateKey, .table = tbl, .index = d.name, .key = k};
      }
      ents.emplace_back(k, std::string(n->key()));
    }
  }
  IndexDef *ix = AddIndex(tbl, std::move(d));
  for (auto &[key, pk] : ents) {
    Value pkv = Value::Str(pk);
    Row r(&pkv, 1);
    ix->tbl->rows.GetOrInsert(key, nullptr)->val.head.store(Version::Make(last, ix->tbl->all | kExists, &r));
  }
  Unlock();
  return {};
}

void DB::DropIndex(int tbl, int idx) {
  Lock();
  Table *t = table(tbl);
  const IndexSet *old = t->ixs.load();
  auto set = std::make_unique<IndexSet>();
  for (size_t i = 0; i < old->list.size(); i++)
    if (int(i) != idx) set->list.push_back(old->list[i]);
  for (IndexDef *ix : set->list) set->indexed |= ix->def.cols;
  t->ixs.store(set.get(), std::memory_order_release);
  t->index_sets.push_back(std::move(set));
  Unlock();
}

void DB::SetChecks(int tbl, std::vector<Check> checks) {
  Lock();
  table(tbl)->checks = std::move(checks);
  Unlock();
}

std::optional<int> DB::TableID(std::string_view name) const {
  for (Table *t : *tables_.load(std::memory_order_acquire))
    if (!t->is_index && !t->dropped && t->schema.name == name) return t->id;
  return std::nullopt;
}

void DB::DropTable(int tbl) {
  Lock();
  Table *t = table(tbl);
  t->dropped = true;
  for (IndexDef *ix : t->indexes()) ix->tbl->dropped = true;
  Unlock();
}

uint64_t DB::ApproxRows(int tbl) const {
  int64_t n = table(tbl)->live.load(std::memory_order_relaxed);
  return n < 0 ? 0 : uint64_t(n);
}

uint64_t DB::CountRange(int tbl, int idx, std::string_view lo, std::string_view hi, uint64_t cap) const {
  Table *t = table(tbl);
  if (idx >= 0) t = t->indexes()[idx]->tbl;
  // Deleted keys keep their nodes; bound the walk over them too.
  uint64_t n = 0, steps = 0;
  for (auto *x = t->rows.Seek(lo, nullptr, nullptr); x != nullptr && (hi.empty() || x->key() < hi) && n < cap && steps < 4 * cap;
       x = x->Next(), steps++)
    if (!x->val.head.load(std::memory_order_acquire)->tombstone) n++;
  return n;
}

uint64_t DB::NextID(int tbl) { return table(tbl)->next_id.fetch_add(1) + 1; }
uint64_t DB::NextHidden(int tbl) { return table(tbl)->hidden.fetch_add(1) + 1; }
uint64_t DB::PeekID(int tbl) const { return table(tbl)->next_id.load(); }
void DB::SetID(int tbl, uint64_t id) { table(tbl)->next_id.store(id); }

void DB::BumpID(int tbl, uint64_t id) {
  auto &c = table(tbl)->next_id;
  for (uint64_t cur = c.load(); cur < id && !c.compare_exchange_weak(cur, id);) {
  }
}

void DB::Lock() {
  if (!opts_.wait) {
    mu_.lock();
    return;
  }
  opts_.wait([this] { return !committing_; });
  committing_ = true;
}

void DB::Unlock() {
  if (!opts_.wait) {
    mu_.unlock();
    return;
  }
  committing_ = false;
}

void DB::Invariant(bool ok, const std::string &what) const {
  if (!ok && !opts_.no_invariants) throw InvariantError("txn: invariant: " + what);
}

std::vector<OwnedRec> DB::Dump(int tbl) const {
  uint64_t ts = last_.load();
  std::vector<OwnedRec> out;
  for (RowNode *n = table(tbl)->rows.First(); n != nullptr; n = n->Next())
    if (Version *v = Visible(n, ts); v != nullptr && !v->tombstone)
      out.push_back(OwnedRec{std::string(n->key()), OwnedRow(v->row())});
  return out;
}

int DB::ChainLength(int tbl, std::string_view pk) const {
  int n = 0;
  if (RowNode *node = table(tbl)->rows.Get(pk, nullptr))
    for (Version *v = node->val.head.load(); v != nullptr; v = v->next.load()) n++;
  return n;
}

static Status Broken(std::string what) { return Error{.code = Code::kInternal, .detail = "txn: " + std::move(what)}; }

Status DB::CheckInvariants() {
  Lock();
  Status st = [&]() -> Status {
    uint64_t last = last_.load();
    auto &tables = *tables_.load();
    for (Table *t : tables) {
      if (!t->rows.Check() || !t->buckets.Check()) return Broken("table " + t->schema.name + ": skiplist out of order");
      for (RowNode *n = t->rows.First(); n != nullptr; n = n->Next()) {
        Version *v = n->val.head.load();
        if (v == nullptr) return Broken("table " + t->schema.name + " row " + Hex(n->key()) + " has no versions");
        if (v->ts > last)
          return Broken("table " + t->schema.name + " row " + Hex(n->key()) + " has version " + std::to_string(v->ts) +
                        " past last commit " + std::to_string(last));
        for (Version *w = v->next.load(); w != nullptr; v = w, w = w->next.load())
          if (w->ts >= v->ts) return Broken("table " + t->schema.name + " row " + Hex(n->key()) + " versions out of order");
      }
      for (auto *n = t->buckets.First(); n != nullptr; n = n->Next())
        if (n->val > last)
          return Broken("table " + t->schema.name + " bucket " + Hex(n->key()) + " ts " + std::to_string(n->val) +
                        " past last commit " + std::to_string(last));
    }
    std::string k;
    for (Table *t : tables) {
      for (IndexDef *ix : t->indexes()) {
        std::map<std::string, std::string> want;
        for (RowNode *n = t->rows.First(); n != nullptr; n = n->Next()) {
          if (Version *v = Visible(n, last); v != nullptr && !v->tombstone) {
            ix->EntryKey(v->row(), n->key(), &k);
            if (want.count(k)) return Broken("index " + ix->tbl->schema.name + " has duplicate key " + Hex(k));
            want[k] = n->key();
          }
        }
        size_t got = 0;
        for (RowNode *n = ix->tbl->rows.First(); n != nullptr; n = n->Next()) {
          if (Version *v = Visible(n, last); v != nullptr && !v->tombstone) {
            got++;
            std::string_view to = v->row()[0].str();
            auto it = want.find(std::string(n->key()));
            if (it == want.end() || it->second != to)
              return Broken("index " + ix->tbl->schema.name + " entry " + Hex(n->key()) + " points to " + Hex(to) +
                            ", want " + Hex(it == want.end() ? "" : it->second));
          }
        }
        if (got != want.size())
          return Broken("index " + ix->tbl->schema.name + " has " + std::to_string(got) + " entries, table has " +
                        std::to_string(want.size()) + " rows");
      }
    }
    return {};
  }();
  Unlock();
  if (!st.ok()) return st;
  if (uint64_t d = DurableTs(); d > last_.load())
    return Broken("durable ts " + std::to_string(d) + " past last commit " + std::to_string(last_.load()));
  return readers_->Check();
}

// --- durability ---

void DB::Wait(std::condition_variable &cv, const std::function<bool()> &cond) {
  Durability &d = *dur_;
  if (opts_.wait) {
    opts_.wait([&] {
      std::lock_guard g(d.mu);
      return cond();
    });
    return;
  }
  std::unique_lock g(d.mu);
  cv.wait(g, cond);
}

void DB::Enqueue(Commit c) {
  {
    std::lock_guard g(dur_->mu);
    dur_->queue.push_back(std::move(c));
  }
  dur_->work.notify_one();
}

void DB::Retire(uint64_t ts, std::vector<Version *> &tails) {
  if (tails.empty()) return;
  if (opts_.keep_trimmed) {
    kept_.insert(kept_.end(), tails.begin(), tails.end());
  } else if (opts_.store != nullptr) {
    std::lock_guard g(dur_->mu);
    for (Version *t : tails) dur_->limbo.emplace_back(ts, t);
  } else {
    for (Version *t : tails) Version::FreeChain(t);
  }
  tails.clear();
}

Status DB::WaitDurable(uint64_t ts) {
  if (opts_.store == nullptr || opts_.no_sync || bugAckBeforeSync) return {};
  bool closed = false;
  Wait(dur_->durable_cond, [&] {
    if (dur_->failed || dur_->closed) {
      closed = true;
      return true;
    }
    return dur_->durable >= ts;
  });
  return closed ? Status(Code::kClosed) : Status();
}

uint64_t DB::DurableTs() {
  std::lock_guard g(dur_->mu);
  return dur_->durable;
}

bool DB::WaitWork() {
  bool open = true;
  Wait(dur_->work, [&] {
    open = !dur_->closed && !dur_->failed;
    return !open || !dur_->queue.empty();
  });
  return open;
}

Status DB::Flush() {
  Durability &d = *dur_;
  std::vector<Commit> batch;
  {
    std::lock_guard g(d.mu);
    batch.swap(d.queue);
  }
  if (batch.empty()) return {};
  {
    std::lock_guard g(d.mu);
    d.last_batch = batch.size();
  }
  bool timed = opts_.gather && opts_.now;
  int64_t t0 = timed ? opts_.now() : 0;
  Status st = opts_.store->Write(batch);
  if (st.ok()) st = opts_.store->Sync();
  int64_t took = timed ? opts_.now() - t0 : 0;
  std::vector<Version *> freed;
  {
    std::lock_guard g(d.mu);
    d.last_flush_ns = took;
    if (!st.ok()) {
      d.failed = true;
    } else {
      uint64_t flushed = batch.back().ts;
      d.durable = flushed;
      if (bugAckJoined && !d.queue.empty()) d.durable = d.queue.back().ts;
      // A chain trimmed by the commit at ts holds versions older than ts,
      // which are written once everything through ts-1 is.
      size_t i = 0;
      while (i < d.limbo.size() && d.limbo[i].first <= flushed + 1) freed.push_back(d.limbo[i++].second);
      d.limbo.erase(d.limbo.begin(), d.limbo.begin() + i);
    }
  }
  d.durable_cond.notify_all();
  for (Version *v : freed) Version::FreeChain(v);
  return st;
}

Status DB::RunWriter() {
  while (WaitWork()) {
    if (opts_.commit_delay) opts_.commit_delay();
    if (opts_.gather) {
      size_t last;
      int64_t took;
      {
        std::lock_guard g(dur_->mu);
        last = dur_->last_batch;
        took = dur_->last_flush_ns;
      }
      opts_.gather(last, took, [this] {
        std::lock_guard g(dur_->mu);
        return dur_->queue.size();
      });
    }
    if (Status st = Flush(); !st.ok()) return st;
  }
  return {};
}

void DB::Close() {
  {
    std::lock_guard g(dur_->mu);
    dur_->closed = true;
  }
  dur_->work.notify_all();
  dur_->durable_cond.notify_all();
}

// Restore installs a recovered row at ts, with its index entries, and moves
// the table's counters past an 8-byte key.
static void Restore(Table *t, uint64_t ts, std::string_view key, Row row, std::string *scratch) {
  Version *v = Version::Make(ts, t->all | kExists, &row);
  RowNode *n = t->rows.GetOrInsert(key, nullptr);
  if (Version *old = n->val.head.exchange(v))
    Version::FreeChain(old);
  else
    t->live.fetch_add(1, std::memory_order_relaxed);
  Value pkv = Value::Str(key);
  Row pkrow(&pkv, 1);
  for (IndexDef *ix : t->indexes()) {
    ix->EntryKey(row, key, scratch);
    Version *e = Version::Make(ts, ix->tbl->all | kExists, &pkrow);
    Version::FreeChain(ix->tbl->rows.GetOrInsert(*scratch, nullptr)->val.head.exchange(e));
  }
  if (key.size() == 8) {
    uint64_t id = 0;
    for (unsigned char c : key) id = id << 8 | c;
    if (id > t->next_id.load()) {
      t->next_id.store(id);
      t->hidden.store(id);
    }
  }
}

Status DB::LoadRows(int tbl, const std::function<Status(const RowPut &)> &load) {
  Table *t = table(tbl);
  uint64_t ts = recovered_;
  std::string k;
  return load([&](std::string_view key, Row row) { Restore(t, ts, key, row, &k); });
}

Status DB::Recover(uint64_t last, const std::function<Status(const Put &)> &load) {
  if (last_.load() != 0) return Broken("recover into a non-empty database");
  std::string k;
  Status st = load([&](int tbl, std::string_view key, Row row) { Restore(table(tbl), last, key, row, &k); });
  if (!st.ok()) return st;
  recovered_ = last;
  last_.store(last, std::memory_order_release);
  std::lock_guard g(dur_->mu);
  dur_->durable = last;
  return {};
}

}  // namespace celltnx
