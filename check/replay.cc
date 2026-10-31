#include "replay.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "format.h"

namespace celltnx::check {

namespace {

// MRow is a model row. Rows are immutable once stored, so undo records and
// readers share them; nullptr is an absent row.
using MRow = std::shared_ptr<const OwnedRow>;

struct Key {
  int tbl;
  std::string pk;
  friend bool operator==(const Key &, const Key &) = default;
  friend auto operator<=>(const Key &, const Key &) = default;
};

struct Undo {
  Key k;
  MRow prev;
};

Status Fail(std::string msg) { return Error{.code = Code::kInternal, .detail = std::move(msg)}; }

std::string Hex(std::string_view s) {
  std::string out;
  char buf[3];
  for (unsigned char c : s) {
    snprintf(buf, sizeof buf, "%02x", c);
    out += buf;
  }
  return out;
}

// Format prints a value, with <nil> for NULL, and a row as [a b c].
std::string Format(const Value &v) {
  switch (v.type) {
    case Type::kNull:
      return "<nil>";
    case Type::kInt:
      return std::to_string(v.i);
    case Type::kUint:
      return std::to_string(v.u);
    case Type::kFloat:
      return FormatFloat(v.f);
    case Type::kString:
      return std::string(v.str());
  }
  return "?";
}

std::string Format(Row r) {
  std::string out = "[";
  for (size_t i = 0; i < r.size(); i++) {
    if (i > 0) out += ' ';
    out += Format(r[i]);
  }
  return out + "]";
}

std::string Format(const MRow &r) { return r ? Format(r->row()) : "[]"; }

// At indexes a row, throwing an InvariantError when c is out of range.
const Value &At(Row r, int c) {
  if (c < 0 || size_t(c) >= r.size())
    throw InvariantError("check: column " + std::to_string(c) + " out of range for a row of " +
                         std::to_string(r.size()));
  return r[c];
}

Row RowOf(const MRow &r) { return r ? r->row() : Row(); }

// RowsEqual is slices.Equal: an absent row equals an empty one.
bool RowsEqual(Row a, Row b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (!(a[i] == b[i])) return false;
  return true;
}

bool ColsEqual(ColMask cols, Row got, Row want) {
  bool ok = true;
  EachCol(cols, [&](int c) { ok = ok && At(got, c) == At(want, c); });
  return ok;
}

bool InRange(std::string_view k, std::string_view lo, std::string_view hi) {
  return k >= lo && (hi.empty() || k < hi);
}

// AddChecked mirrors the store's checked integer addition, storing the
// wrapped sum and returning whether it fit.
bool AddChecked(Value v, int64_t d, Value *out) {
  switch (v.type) {
    case Type::kInt: {
      int64_t s = int64_t(uint64_t(v.i) + uint64_t(d));
      *out = Value::Int(s);
      return (s > v.i) == (d > 0);
    }
    case Type::kUint: {
      uint64_t s = v.u + uint64_t(d);
      *out = Value::Uint(s);
      return !((d >= 0 && s < v.u) || (d < 0 && s > v.u));
    }
    default:
      *out = v;
      return false;
  }
}

// EntryKey mirrors the store's index entry key.
std::string EntryKey(const Index &ix, Row row, std::string_view pk) {
  std::string k;
  bool u = ix.key(row, &k);
  if (ix.unique && u) return k;
  k.append(pk);
  return k;
}

// Model is the reference state: ordered maps, changed one transaction at
// a time.
class Model {
 public:
  explicit Model(const std::vector<Schema> &schemas) : schemas_(schemas), tables_(schemas.size()) {}

  Status Run(const Log &l);
  Status Compare(int tbl, const std::vector<OwnedRec> &dump);

 private:
  using Table = std::map<std::string, MRow, std::less<>>;

  Table &table(int tbl) {
    if (tbl < 0 || size_t(tbl) >= tables_.size()) throw InvariantError("check: no table " + std::to_string(tbl));
    return tables_[tbl];
  }
  MRow Get(const Key &k) {
    Table &t = table(k.tbl);
    auto it = t.find(k.pk);
    return it == t.end() ? nullptr : it->second;
  }
  void Set(const Key &k, MRow row) {
    if (row)
      table(k.tbl)[k.pk] = std::move(row);
    else
      table(k.tbl).erase(k.pk);
  }
  void Put(const Key &k, MRow row, std::vector<Undo> *log) {
    log->push_back(Undo{k, Get(k)});
    Set(k, std::move(row));
  }
  void Rollback(std::vector<Undo> *log, size_t to) {
    while (log->size() > to) {
      Undo u = std::move(log->back());
      log->pop_back();
      Set(u.k, std::move(u.prev));
    }
  }
  // Before returns k's row before the first logged change to it.
  MRow Before(const Key &k, const std::vector<Undo> &log) {
    for (const Undo &u : log)
      if (u.k == k) return u.prev;
    return Get(k);
  }
  std::vector<std::string> RangeKeys(int tbl, std::string_view lo, std::string_view hi);
  std::vector<std::string> IndexRange(int tbl, int idx, std::string_view lo, std::string_view hi);
  bool Duplicates(int tbl, std::string_view pk, Row row, ColMask written);

  const std::vector<Schema> &schemas_;
  std::vector<Table> tables_;
};

std::vector<std::string> Model::RangeKeys(int tbl, std::string_view lo, std::string_view hi) {
  std::vector<std::string> keys;
  Table &t = table(tbl);
  for (auto it = t.lower_bound(lo); it != t.end() && InRange(it->first, lo, hi); ++it) keys.push_back(it->first);
  return keys;
}

// IndexRange returns the primary keys whose entries in index idx fall in
// [lo, hi), in entry order. Entry keys are unique (a unique claim, or the
// key followed by the primary key), so the order is total; the primary key
// breaks ties anyway, so the result never depends on map order.
std::vector<std::string> Model::IndexRange(int tbl, int idx, std::string_view lo, std::string_view hi) {
  const Index &ix = schemas_.at(tbl).indexes.at(idx);
  std::vector<std::pair<std::string, std::string>> es;
  for (const auto &[pk, row] : table(tbl)) {
    std::string ek = EntryKey(ix, row->row(), pk);
    if (InRange(ek, lo, hi)) es.emplace_back(std::move(ek), pk);
  }
  std::sort(es.begin(), es.end());
  std::vector<std::string> pks;
  pks.reserve(es.size());
  for (auto &e : es) pks.push_back(std::move(e.second));
  return pks;
}

// Duplicates reports whether row, the new image of pk, collides with
// another row on a unique index over the written columns.
bool Model::Duplicates(int tbl, std::string_view pk, Row row, ColMask written) {
  for (const Index &ix : schemas_[tbl].indexes) {
    if (!ix.unique || (ix.cols & written) == 0) continue;
    std::string k;
    if (!ix.key(row, &k)) continue;
    for (const auto &[pk2, r2] : table(tbl)) {
      std::string k2;
      bool u2 = ix.key(r2->row(), &k2);
      if (pk2 != pk && u2 && k2 == k) return true;
    }
  }
  return false;
}

Status Model::Run(const Log &l) {
  std::vector<Undo> log;
  std::map<int, size_t> sps;
  bool overflow = false;
  for (size_t i = 0; i < l.Ops.size(); i++) {
    const Op &op = l.Ops[i];
    Key k{op.Table, op.PK};
    MRow row = Get(k);
    auto fail = [&](const std::string &msg) {
      return Fail("op " + std::to_string(i) + " " + OpKindName(op.Kind) + " table=" + std::to_string(op.Table) +
                  " pk=" + Hex(op.PK) + ": " + msg);
    };
    // write applies a new image of an existing row, unless it would
    // duplicate a unique key.
    auto write = [&](MRow r, ColMask written) -> Status {
      Result want = Duplicates(op.Table, op.PK, r->row(), written) ? Result::kDuplicate : Result::kOK;
      if (want != op.Res)
        return fail("result " + std::to_string(int(op.Res)) + ", model says " + std::to_string(int(want)));
      if (want == Result::kOK) Put(k, std::move(r), &log);
      return {};
    };
    switch (op.Kind) {
      case OpKind::kGet:
        if ((row != nullptr) != op.Found)
          return fail(std::string("found=") + (op.Found ? "true" : "false") + ", model has row=" +
                      (row != nullptr ? "true" : "false"));
        if (row != nullptr && !ColsEqual(op.Cols, op.Vals.row(), row->row())) {
          char cols[32];
          snprintf(cols, sizeof cols, "%#" PRIx64, uint64_t(op.Cols));
          return fail("read " + Format(op.Vals.row()) + ", model has " + Format(row) + " (cols " + cols + ")");
        }
        break;
      case OpKind::kSet:
      case OpKind::kAdd: {
        if (row == nullptr) {
          if (op.Res != Result::kNotFound)
            return fail("result " + std::to_string(int(op.Res)) + ", model says not found");
          continue;
        }
        std::vector<Value> r(row->row().begin(), row->row().end());
        ColMask written = op.Cols;
        if (op.Kind == OpKind::kSet) {
          EachCol(op.Cols, [&](int c) {
            At(r, c);
            r[c] = At(op.Vals.row(), c);
          });
        } else {
          written = Col(op.Col);
          if (!AddChecked(At(r, op.Col), op.Delta, &r[op.Col])) overflow = true;
        }
        if (Status st = write(std::make_shared<const OwnedRow>(Row(r)), written); !st.ok()) return st;
        break;
      }
      case OpKind::kInsert:
        if (row != nullptr) {
          if (op.Res != Result::kDuplicate) return fail("result " + std::to_string(int(op.Res)) + ", model has the row");
          continue;
        }
        if (Status st = write(std::make_shared<const OwnedRow>(op.Vals), ~kExists); !st.ok()) return st;
        break;
      case OpKind::kDelete: {
        Result got = row == nullptr ? Result::kNotFound : Result::kOK;
        if (got != op.Res)
          return fail("result " + std::to_string(int(op.Res)) + ", model says " + std::to_string(int(got)));
        if (row != nullptr) Put(k, nullptr, &log);
        break;
      }
      case OpKind::kScan:
      case OpKind::kIndexScan: {
        std::vector<std::string> pks = op.Kind == OpKind::kScan ? RangeKeys(op.Table, op.Lo, op.Hi)
                                                                : IndexRange(op.Table, op.Index, op.Lo, op.Hi);
        if (pks.size() != op.Rows.size())
          return fail(std::string(OpKindName(op.Kind)) + " saw " + std::to_string(op.Rows.size()) +
                      " rows, model has " + std::to_string(pks.size()));
        for (size_t j = 0; j < op.Rows.size(); j++) {
          const ScanRow &sr = op.Rows[j];
          if (sr.PK != pks[j])
            return fail("row " + std::to_string(j) + " is pk " + Hex(sr.PK) + ", model has " + Hex(pks[j]));
          ColMask cols = op.Cols;
          if (sr.Match) cols |= op.Proj;
          MRow r = Get(Key{op.Table, pks[j]});
          if (!ColsEqual(cols, sr.Vals.row(), RowOf(r)))
            return fail("pk " + Hex(sr.PK) + " read " + Format(sr.Vals.row()) + ", model has " + Format(r));
        }
        break;
      }
      case OpKind::kSavepoint:
        sps[op.SP] = log.size();
        break;
      case OpKind::kRollback: {
        auto it = sps.find(op.SP);
        if (it == sps.end()) return fail("rollback to unknown savepoint " + std::to_string(op.SP));
        Rollback(&log, it->second);
        break;
      }
    }
  }

  // Rows this transaction changed, after rollbacks.
  std::vector<Key> touched;
  std::set<Key> seen;
  for (const Undo &u : log)
    if (seen.insert(u.k).second) touched.push_back(u.k);
  std::string violation;
  for (const Key &k : touched) {
    MRow row = Get(k);
    if (row == nullptr) continue;
    for (const Check &c : schemas_[k.tbl].checks) {
      if (!c.ok(row->row()) && violation.empty())
        violation = "table " + std::to_string(k.tbl) + " pk " + Hex(k.pk) + " violates " + c.name + ": " + Format(row);
    }
  }

  switch (l.Kind) {
    case Kind::kWriter:
      if (overflow) return Fail("committed an overflowing delta");
      if (!violation.empty()) return Fail("committed a row that " + violation);
      break;
    case Kind::kReader:
      for (const Undo &u : log) {
        if (!RowsEqual(RowOf(Get(u.k)), RowOf(Before(u.k, log))))
          return Fail("read-only transaction changed table " + std::to_string(u.k.tbl) + " pk " + Hex(u.k.pk));
      }
      Rollback(&log, 0);
      break;
    case Kind::kRejected:
      if (violation.empty() && !overflow) return Fail("rejected, but every changed row passes its checks");
      Rollback(&log, 0);
      break;
  }
  return {};
}

Status Model::Compare(int tbl, const std::vector<OwnedRec> &dump) {
  Table &t = table(tbl);
  if (t.size() != dump.size())
    return Fail("table " + std::to_string(tbl) + " has " + std::to_string(dump.size()) + " rows, model has " +
                std::to_string(t.size()));
  auto it = t.begin();
  for (const OwnedRec &r : dump) {
    if (r.pk != it->first || !RowsEqual(r.row.row(), it->second->row()))
      return Fail("table " + std::to_string(tbl) + " row " + Hex(r.pk) + " = " + Format(r.row.row()) +
                  ", model has pk " + Hex(it->first) + " = " + Format(it->second));
    ++it;
  }
  return {};
}

}  // namespace

Status Replay(const std::vector<Schema> &schemas, const std::vector<Log> &logs, uint64_t last,
              const std::function<std::vector<OwnedRec>(int tbl)> &dump) {
  std::vector<const Log *> order;
  order.reserve(logs.size());
  for (const Log &l : logs) order.push_back(&l);
  std::stable_sort(order.begin(), order.end(), [](const Log *a, const Log *b) {
    if (a->Ts != b->Ts) return a->Ts < b->Ts;
    return a->Kind == Kind::kWriter && b->Kind != Kind::kWriter;
  });
  uint64_t writers = 0;
  for (const Log *l : order) {
    if (l->Kind != Kind::kWriter) continue;
    writers++;
    if (l->Ts != writers)
      return Fail("check: writer commit timestamps skip from " + std::to_string(writers - 1) + " to " +
                  std::to_string(l->Ts));
  }
  if (writers != last)
    return Fail("check: " + std::to_string(writers) + " writers logged, last commit is " + std::to_string(last));
  Model m(schemas);
  for (size_t i = 0; i < order.size(); i++) {
    const Log &l = *order[i];
    if (Status st = m.Run(l); !st.ok())
      return Fail("check: log " + std::to_string(i) + " (" + KindName(l.Kind) + " ts=" + std::to_string(l.Ts) +
                  " readTs=" + std::to_string(l.ReadTs) + " client=" + std::to_string(l.Client) +
                  "): " + st.error().detail);
  }
  for (size_t tbl = 0; tbl < schemas.size(); tbl++) {
    if (Status st = m.Compare(int(tbl), dump(int(tbl))); !st.ok())
      return Fail("check: final state: " + st.error().detail);
  }
  return {};
}

}  // namespace celltnx::check
