#include "record.h"

#include <bit>
#include <utility>

namespace celltnx::check {

namespace {

constexpr uint64_t kFNVOffset = 14695981039346656037ull;
constexpr uint64_t kFNVPrime = 1099511628211ull;

uint64_t HashString(std::string_view s) {
  uint64_t h = kFNVOffset;
  for (unsigned char c : s) h = (h ^ uint64_t(c)) * kFNVPrime;
  return h;
}

uint64_t HashValue(const Value &v) {
  switch (v.type) {
    case Type::kInt:
      return uint64_t(v.i);
    case Type::kUint:
      return v.u;
    case Type::kFloat:
      return std::bit_cast<uint64_t>(v.f);
    case Type::kString:
      return HashString(v.str());
    case Type::kNull:
      break;
  }
  return 0;
}

// WriteResult maps a write's status to its logged result, reporting false
// for an outcome the log omits.
bool WriteResult(const Status &st, Result *res) {
  switch (st.code()) {
    case Code::kOk:
      *res = Result::kOK;
      return true;
    case Code::kNotFound:
      *res = Result::kNotFound;
      return true;
    case Code::kDuplicateKey:
      *res = Result::kDuplicate;
      return true;
    default:
      return false;
  }
}

}  // namespace

const char *KindName(Kind k) {
  switch (k) {
    case Kind::kWriter:
      return "writer";
    case Kind::kReader:
      return "reader";
    case Kind::kRejected:
      return "rejected";
  }
  return "?";
}

const char *OpKindName(OpKind k) {
  static const char *const names[] = {"get",  "set",       "add",      "insert",   "delete",
                                      "scan", "savepoint", "rollback", "indexscan"};
  size_t i = size_t(k);
  return i < std::size(names) ? names[i] : "?";
}

Recorder::Recorder() : trace_(kFNVOffset) {}

std::vector<Log> Recorder::Logs() {
  std::lock_guard l(mu_);
  return logs_;
}

uint64_t Recorder::Trace() {
  std::lock_guard l(mu_);
  return trace_;
}

void Recorder::Hash(std::initializer_list<uint64_t> words) {
  if (off) return;
  std::lock_guard l(mu_);
  for (uint64_t w : words) {
    for (int i = 0; i < 8; i++) {
      trace_ = (trace_ ^ (w & 0xff)) * kFNVPrime;
      w >>= 8;
    }
  }
}

int Recorder::Add(Log l) {
  if (off) return -1;
  std::lock_guard g(mu_);
  logs_.push_back(std::move(l));
  int i = int(logs_.size()) - 1;
  if (on_add) on_add(i, logs_.back());
  return i;
}

void Recorder::Ack(int i) {
  if (i < 0) return;
  std::lock_guard l(mu_);
  if (size_t(i) >= logs_.size()) throw InvariantError("check: ack of log " + std::to_string(i) + " out of range");
  logs_[i].Acked = true;
  if (on_ack) on_ack(i);
}

void Recorder::SetLogs(std::vector<Log> logs) {
  std::lock_guard l(mu_);
  logs_ = std::move(logs);
}

Status Recorder::Truncate(uint64_t last, bool durable) {
  std::lock_guard g(mu_);
  for (const Log &l : logs_) {
    if (l.Ts > last && durable && l.Acked && l.Kind != Kind::kRejected)
      return Error{.code = Code::kInternal,
                   .detail = std::string("check: acknowledged ") + KindName(l.Kind) + " at ts " + std::to_string(l.Ts) +
                             " lost; recovered through " + std::to_string(last)};
  }
  std::vector<Log> kept;
  for (Log &l : logs_)
    if (l.Ts <= last) kept.push_back(std::move(l));
  logs_ = std::move(kept);
  return {};
}

std::unique_ptr<Tx> Recorder::Begin(DB &db, int client, std::function<void()> yield) {
  if (yield) yield();
  std::unique_ptr<Txn> t = db.Begin();
  Hash({uint64_t(int64_t(client)), t->ReadTs()});
  return std::unique_ptr<Tx>(new Tx(std::move(t), this, std::move(yield), client));
}

void Tx::Record(Op op) {
  if (rec_->off) return;
  rec_->Hash({uint64_t(op.Kind), uint64_t(int64_t(op.Table)), HashString(op.PK), uint64_t(op.Res),
              uint64_t(op.Rows.size())});
  for (const Value &v : op.Vals.row()) rec_->Hash({HashValue(v)});
  for (const ScanRow &r : op.Rows) rec_->Hash({HashString(r.PK)});
  ops_.push_back(std::move(op));
}

Status Tx::Get(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found) {
  Before();
  Status st = t_->Get(tbl, pk, cols, row, found);
  if (st.ok() && !rec_->off)
    Record(Op{.Kind = OpKind::kGet,
              .Table = tbl,
              .PK = std::string(pk),
              .Cols = cols,
              .Vals = OwnedRow(*row),
              .Found = *found});
  return st;
}

Status Tx::GetUntracked(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found) {
  Before();
  return t_->GetUntracked(tbl, pk, cols, row, found);
}

Status Tx::Scan(int tbl, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj, const Match &match,
                std::vector<Rec> *out) {
  return ScanImpl(OpKind::kScan, tbl, 0, lo, hi, pred, proj, match, out);
}

Status Tx::IndexScan(int tbl, int idx, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj,
                     const Match &match, std::vector<Rec> *out) {
  return ScanImpl(OpKind::kIndexScan, tbl, idx, lo, hi, pred, proj, match, out);
}

Status Tx::ScanImpl(OpKind kind, int tbl, int idx, std::string_view lo, std::string_view hi, ColMask pred,
                    ColMask proj, const Match &match, std::vector<Rec> *out) {
  Before();
  bool logging = !rec_->off;
  std::vector<ScanRow> rows;
  Match m = [&](std::string_view pk, Row row) {
    bool ok = !match || match(pk, row);
    if (logging) rows.push_back(ScanRow{std::string(pk), ok, OwnedRow(row)});
    return ok;
  };
  Status st = kind == OpKind::kScan ? t_->Scan(tbl, lo, hi, pred, proj, m, out)
                                    : t_->IndexScan(tbl, idx, lo, hi, pred, proj, m, out);
  if (!st.ok()) {
    out->clear();
    return st;
  }
  if (!logging) return st;
  size_t j = 0;
  for (ScanRow &r : rows) {
    if (r.Match) r.Vals.Assign((*out)[j++].row);
  }
  Record(Op{.Kind = kind,
            .Table = tbl,
            .Index = idx,
            .Cols = pred,
            .Proj = proj,
            .Lo = std::string(lo),
            .Hi = std::string(hi),
            .Rows = std::move(rows)});
  return st;
}

Status Tx::Set(int tbl, std::string_view pk, ColMask cols, Row vals) {
  Before();
  Status st = t_->Set(tbl, pk, cols, vals);
  if (Result res; !rec_->off && WriteResult(st, &res))
    Record(
        Op{.Kind = OpKind::kSet, .Table = tbl, .PK = std::string(pk), .Cols = cols, .Vals = OwnedRow(vals), .Res = res});
  return st;
}

Status Tx::Add(int tbl, std::string_view pk, int col, int64_t delta) {
  Before();
  Status st = t_->Add(tbl, pk, col, delta);
  if (Result res; !rec_->off && WriteResult(st, &res))
    Record(Op{.Kind = OpKind::kAdd, .Table = tbl, .PK = std::string(pk), .Col = col, .Delta = delta, .Res = res});
  return st;
}

Status Tx::Insert(int tbl, std::string_view pk, Row row) {
  Before();
  Status st = t_->Insert(tbl, pk, row);
  if (Result res; !rec_->off && WriteResult(st, &res))
    Record(Op{.Kind = OpKind::kInsert, .Table = tbl, .PK = std::string(pk), .Vals = OwnedRow(row), .Res = res});
  return st;
}

Status Tx::Delete(int tbl, std::string_view pk) {
  Before();
  Status st = t_->Delete(tbl, pk);
  if (Result res; !rec_->off && WriteResult(st, &res))
    Record(Op{.Kind = OpKind::kDelete, .Table = tbl, .PK = std::string(pk), .Res = res});
  return st;
}

int Tx::Savepoint() {
  int sp = t_->Savepoint();
  Record(Op{.Kind = OpKind::kSavepoint, .SP = sp});
  return sp;
}

void Tx::RollbackTo(int sp) {
  t_->RollbackTo(sp);
  Record(Op{.Kind = OpKind::kRollback, .SP = sp});
}

void Tx::Before() {
  Yield();
  if (rec_->refresh && rec_->refresh())
    if (Status st = t_->Refresh(); !st.ok() && doomed_.ok()) doomed_ = std::move(st);
}

Status Tx::Commit() {
  Before();
  if (!doomed_.ok()) {
    t_->Abort();
    rec_->Hash({3});
    return Error(doomed_.error());
  }
  int i = -1;
  t_->on_install = [&](uint64_t ts) {
    i = rec_->Add(Log{.Kind = Kind::kWriter, .Ts = ts, .ReadTs = t_->ReadTs(), .Client = client_, .Ops = std::move(ops_)});
  };
  Status st = t_->Install();
  t_->on_install = nullptr;
  if (st.ok()) {
    if (!t_->Wrote())
      i = rec_->Add(
          Log{.Kind = Kind::kReader, .Ts = t_->SerialTs(), .ReadTs = t_->ReadTs(), .Client = client_, .Ops = std::move(ops_)});
    rec_->Hash({1, t_->SerialTs()});
    st = t_->WaitDurable();
    if (st.ok()) rec_->Ack(i);
  } else if (st.code() == Code::kConstraint) {
    uint64_t after = st.error().after_ts;
    rec_->Add(Log{.Kind = Kind::kRejected,
                  .Ts = after,
                  .ReadTs = t_->ReadTs(),
                  .Client = client_,
                  .Ops = std::move(ops_),
                  .Acked = true});
    rec_->Hash({2, after});
  } else {
    rec_->Hash({3});
  }
  return st;
}

void Tx::Abort() {
  t_->Abort();
  rec_->Hash({4});
}

}  // namespace celltnx::check
