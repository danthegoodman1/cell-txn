#include "random.h"

#include "format.h"
#include "keys.h"

namespace celltnx::workload {

namespace {

enum { kGet, kUntracked, kScan, kIndexScan, kSet, kAdd, kInsert, kDelete };

const char *Bool(bool b) { return b ? "true" : "false"; }

std::function<bool(Row, std::string *)> IntKey(int c) {
  return [c](Row r, std::string *out) {
    keys::AppendInt(*out, AsInt(r[c]));
    return true;
  };
}

std::string IntKeyString(int64_t x) {
  std::string k;
  keys::AppendInt(k, x);
  return k;
}

int64_t Draw(const Random &w, Client &c, int tbl, int col) {
  if (tbl == 0 && col == 0 && (w.Check || w.Index0)) return c.Rand.Int64N(20);
  if (tbl == 0 && col == 1 && w.Unique1) return c.Rand.Int64N(int64_t(2 * w.Keys));
  return c.Uniq();
}

std::vector<Value> NewRow(const Random &w, Client &c, int tbl) {
  std::vector<Value> r(w.Widths[tbl]);
  for (size_t col = 0; col < r.size(); col++) r[col] = Value::Int(Draw(w, c, tbl, int(col)));
  return r;
}

ColMask Mask(Rand &r, int width) {
  ColMask m = r.Uint64N(uint64_t(1) << width);
  return m | Col(int(r.IntN(width)));
}

int Pick(const Random &w, Rand &r, bool read_only) {
  int limit = read_only ? kIndexScan + 1 : Random::kOps;
  int64_t total = 0;
  for (int i = 0; i < limit; i++) total += w.Weights[i];
  if (total == 0) return kGet;
  int64_t n = r.IntN(total);
  for (int op = 0; op < limit; op++) {
    if (n < w.Weights[op]) return op;
    n -= w.Weights[op];
  }
  throw InvariantError("unreachable");
}

Status DoOp(const Random &w, Client &c, check::Tx &tx, bool read_only) {
  Rand &r = c.Rand;
  int tbl = int(r.IntN(int64_t(w.Widths.size())));
  int width = w.Widths[tbl];
  uint64_t k = r.Uint64N(w.Keys);
  std::string pk = keys::U64(k);
  std::vector<Value> row;
  bool found;
  switch (int op = Pick(w, r, read_only)) {
    case kGet:
      return tx.Get(tbl, pk, Mask(r, width), &row, &found);
    case kUntracked:
      return tx.GetUntracked(tbl, pk, Mask(r, width), &row, &found);
    case kScan:
    case kIndexScan: {
      ColMask pred = Mask(r, width);
      ColMask proj = r.Uint64N(uint64_t(1) << width);
      int col = 0;
      while ((pred & Col(col)) == 0) col++;
      int64_t mod = 2 + r.IntN(2);
      Match match = [col, mod](std::string_view, Row row) { return AsInt(row[col]) % mod == 0; };
      int nidx = tbl == 0 ? int(w.Index0) + int(w.Unique1) : 0;
      std::vector<Rec> recs;
      if (nidx == 0 || op == kScan) {
        std::string hi = keys::U64(k + 1 + r.Uint64N(w.Keys / 2 + 1));
        if (r.Float64() < w.Unbounded) hi.clear();
        return tx.Scan(tbl, pk, hi, pred, proj, match, &recs);
      }
      int64_t lo = r.Int64N(int64_t(2 * w.Keys));
      std::string hi = IntKeyString(lo + 1 + r.Int64N(10));
      if (r.Float64() < w.Unbounded) hi.clear();
      int idx = int(r.IntN(nidx));
      return tx.IndexScan(tbl, idx, IntKeyString(lo), hi, pred, proj, match, &recs);
    }
    case kSet: {
      ColMask cols = Mask(r, width);
      std::vector<Value> vals(width);
      EachCol(cols, [&](int col) { vals[col] = Value::Int(Draw(w, c, tbl, col)); });
      return Absent(tx.Set(tbl, pk, cols, vals));
    }
    case kAdd: {
      int col = int(r.IntN(width));
      int64_t delta = r.Int64N(13) - 6;
      return Absent(tx.Add(tbl, pk, col, delta));
    }
    case kInsert: {
      std::vector<Value> fresh = NewRow(w, c, tbl);
      return Absent(tx.Insert(tbl, pk, fresh));
    }
    default:
      return Absent(tx.Delete(tbl, pk));
  }
}

}  // namespace

std::unique_ptr<Workload> NewRandom(Rand &r) {
  auto w = std::make_unique<Random>();
  w->Keys = uint64_t(2 + r.IntN(40));
  w->Check = r.IntN(4) != 0;
  w->Index0 = r.IntN(2) == 0;
  w->Unique1 = r.IntN(2) == 0;
  w->MaxOps = int(1 + r.IntN(8));
  w->ReadOnly = r.Float64() * 0.2;
  w->Abandon = r.Float64() * 0.02;
  w->Save = r.Float64() * 0.2;
  w->Think = r.Float64() * 0.3;
  w->Unbounded = r.Float64() * 0.3;
  for (int64_t i = 0, n = 1 + r.IntN(2); i < n; i++) w->Widths.push_back(int(1 + r.IntN(5)));
  if (w->Widths[0] < 2) w->Unique1 = false;
  for (int &x : w->Weights)
    if (r.IntN(5) != 0) x = int(1 + r.IntN(10));
  if (w->Weights[kGet] == 0 && w->Weights[kSet] == 0) w->Weights[kSet] = 1;
  return w;
}

std::vector<Schema> Random::Schemas() {
  std::vector<Schema> ss;
  for (size_t i = 0; i < Widths.size(); i++) {
    Schema s{.name = "t", .cols = std::vector<std::string>(Widths[i])};
    if (i == 0) {
      if (Check) s.checks = {celltnx::Check{"c0 >= 0", Col(0), [](Row r) { return AsInt(r[0]) >= 0; }}};
      if (Index0) s.indexes.push_back(Index{.name = "c0", .cols = Col(0), .key = IntKey(0), .bucket_prefix = 9});
      if (Unique1)
        s.indexes.push_back(
            Index{.name = "c1", .cols = Col(1), .unique = true, .key = IntKey(1), .bucket_prefix = 9});
    }
    ss.push_back(std::move(s));
  }
  return ss;
}

Status Random::Load(Client &c) {
  return c.Do([&](check::Tx &tx) -> Status {
    for (size_t tbl = 0; tbl < Widths.size(); tbl++) {
      for (uint64_t pk = 0; pk < Keys; pk++) {
        if (c.Rand.IntN(2) != 0) continue;
        std::vector<Value> row = NewRow(*this, c, int(tbl));
        if (Status st = Absent(tx.Insert(int(tbl), keys::U64(pk), row)); !st.ok()) return st;
      }
    }
    return {};
  });
}

Status Random::Step(Client &c) {
  Rand &r = c.Rand;
  bool read_only = r.Float64() < ReadOnly;
  return Expect(c.Do([&](check::Tx &tx) -> Status {
    int64_t n = 1 + r.IntN(MaxOps);
    if (read_only) n = 5 + r.IntN(20);
    for (int64_t i = 0; i < n; i++) {
      if (r.Float64() < Think) c.Env->Sleep(1 + r.Int64N(50));
      if (r.Float64() < Abandon) return ErrAbort();
      if (read_only || r.Float64() >= Save) {
        if (Status st = DoOp(*this, c, tx, read_only); !st.ok()) return st;
        continue;
      }
      int sp = tx.Savepoint();
      for (int64_t j = 0, m = 1 + r.IntN(3); j < m; j++)
        if (Status st = DoOp(*this, c, tx, false); !st.ok()) return st;
      if (r.IntN(2) == 0) tx.RollbackTo(sp);
    }
    return {};
  }));
}

std::string Random::Detail() {
  return "&{Widths:" + FormatInts(Widths) + " Keys:" + std::to_string(Keys) + " Check:" + Bool(Check) +
         " Index0:" + Bool(Index0) + " Unique1:" + Bool(Unique1) + " MaxOps:" + std::to_string(MaxOps) +
         " Weights:" + FormatInts(Weights) + " ReadOnly:" + FormatFloat(ReadOnly) + " Abandon:" + FormatFloat(Abandon) +
         " Save:" + FormatFloat(Save) + " Think:" + FormatFloat(Think) + " Unbounded:" + FormatFloat(Unbounded) + "}";
}

}  // namespace celltnx::workload
