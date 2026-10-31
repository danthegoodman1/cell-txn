// The checker accepts serializable histories and rejects altered ones.
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "db.h"
#include "keys.h"
#include "record.h"
#include "replay.h"

namespace celltnx::check {
namespace {

const std::string A = keys::U64(1), B = keys::U64(2), C = keys::U64(3);

int64_t AsInt(const Value &v) { return v.type == Type::kInt ? v.i : 0; }

std::string IntKey(int64_t x) {
  std::string k;
  keys::AppendInt(k, x);
  return k;
}

// Table t has columns a and b, CHECK a >= 0, and a unique index on b.
std::vector<Schema> Schemas() {
  return {Schema{.name = "t",
                 .cols = {"a", "b"},
                 .checks = {Check{"a >= 0", Col(0), [](Row r) { return AsInt(r[0]) >= 0; }}},
                 .indexes = {Index{.name = "b",
                                   .cols = Col(1),
                                   .unique = true,
                                   .key =
                                       [](Row r, std::string *out) {
                                         keys::AppendInt(*out, AsInt(r[1]));
                                         return true;
                                       }}}}};
}

// R builds a row of ints, with -1 standing for NULL.
OwnedRow R(std::initializer_list<int64_t> xs) {
  std::vector<Value> v;
  for (int64_t x : xs) v.push_back(x == -1 ? Value() : Value::Int(x));
  return OwnedRow(v);
}

Op Insert(const std::string &pk, OwnedRow row, Result res = Result::kOK) {
  return Op{.Kind = OpKind::kInsert, .PK = pk, .Vals = std::move(row), .Res = res};
}

// History is a small serializable history, logged out of order, and the
// final state it leaves.
struct History {
  std::vector<Log> logs;
  std::vector<OwnedRec> dump;
  uint64_t last = 3;
};

History Good() {
  History h;
  h.logs = {
      // Ts 2: a reader scans A and B. A matches and shows its projection.
      Log{.Kind = Kind::kReader,
          .Ts = 2,
          .ReadTs = 2,
          .Client = 3,
          .Ops = {Op{.Kind = OpKind::kScan,
                     .Cols = Col(0),
                     .Proj = Col(1),
                     .Rows = {ScanRow{A, true, R({1, 30})}, ScanRow{B, false, R({7, -1})}}}}},
      // Ts 1: load A and B.
      Log{.Kind = Kind::kWriter, .Ts = 1, .Client = 1, .Ops = {Insert(A, R({1, 10})), Insert(B, R({2, 20}))}},
      // Ts 2: a rejected delta drives A.a below zero.
      Log{.Kind = Kind::kRejected,
          .Ts = 2,
          .ReadTs = 1,
          .Client = 4,
          .Ops = {Op{.Kind = OpKind::kAdd, .PK = A, .Col = 0, .Delta = -5}},
          .Acked = true},
      // Ts 2: read A.a, move A.b, add to B.a, collide on A, miss C.
      Log{.Kind = Kind::kWriter,
          .Ts = 2,
          .ReadTs = 1,
          .Client = 2,
          .Ops = {Op{.Kind = OpKind::kGet, .PK = A, .Cols = Col(0), .Vals = R({1, -1}), .Found = true},
                  Op{.Kind = OpKind::kSet, .PK = A, .Cols = Col(1), .Vals = R({-1, 30})},
                  Op{.Kind = OpKind::kAdd, .PK = B, .Col = 0, .Delta = 5},
                  Insert(A, R({9, 9}), Result::kDuplicate),
                  Op{.Kind = OpKind::kDelete, .PK = C, .Res = Result::kNotFound}}},
      // Ts 3: B.b may not take A's 30; C comes and goes; B goes.
      Log{.Kind = Kind::kWriter,
          .Ts = 3,
          .ReadTs = 2,
          .Client = 1,
          .Ops = {Op{.Kind = OpKind::kSet, .PK = B, .Cols = Col(1), .Vals = R({-1, 30}), .Res = Result::kDuplicate},
                  Op{.Kind = OpKind::kSavepoint, .SP = 0},
                  Insert(C, R({3, 40})),
                  Op{.Kind = OpKind::kRollback, .SP = 0},
                  Op{.Kind = OpKind::kGet, .PK = C, .Cols = Col(0) | Col(1)},
                  Op{.Kind = OpKind::kDelete, .PK = B}}},
      // Ts 3: an index scan from b = 0 sees only A.
      Log{.Kind = Kind::kReader,
          .Ts = 3,
          .ReadTs = 3,
          .Client = 2,
          .Ops = {Op{.Kind = OpKind::kIndexScan, .Cols = Col(0), .Lo = IntKey(0), .Rows = {ScanRow{A, true, R({1, 30})}}}}},
  };
  h.dump.push_back(OwnedRec{A, R({1, 30})});
  return h;
}

Status ReplayOf(const History &h) {
  return Replay(Schemas(), h.logs, h.last, [&](int) { return h.dump; });
}

Op &FindOp(History &h, uint64_t ts, Kind kind, size_t i) {
  for (Log &l : h.logs)
    if (l.Ts == ts && l.Kind == kind) return l.Ops.at(i);
  throw std::out_of_range("no such log");
}

TEST(Check, ReplayAcceptsSerializableHistory) {
  History h = Good();
  Status st = ReplayOf(h);
  EXPECT_TRUE(st.ok()) << st.Message();
}

TEST(Check, ReplayRejectsAlteredHistories) {
  struct Case {
    const char *name;
    std::function<void(History &)> alter;
  };
  const Case cases[] = {
      {"read value", [](History &h) { FindOp(h, 2, Kind::kWriter, 0).Vals = R({2, -1}); }},
      {"read of a missing row", [](History &h) { FindOp(h, 3, Kind::kWriter, 4).Found = true; }},
      {"scan projection", [](History &h) { FindOp(h, 2, Kind::kReader, 0).Rows[0].Vals = R({1, 10}); }},
      {"scan predicate on a miss", [](History &h) { FindOp(h, 2, Kind::kReader, 0).Rows[1].Vals = R({2, -1}); }},
      {"scan row count", [](History &h) { FindOp(h, 2, Kind::kReader, 0).Rows.pop_back(); }},
      {"index scan order",
       [](History &h) { FindOp(h, 3, Kind::kReader, 0).Rows.push_back(ScanRow{C, false, R({3, -1})}); }},
      {"unique claim", [](History &h) { FindOp(h, 3, Kind::kWriter, 0).Res = Result::kOK; }},
      {"insert over a row", [](History &h) { FindOp(h, 2, Kind::kWriter, 3).Res = Result::kOK; }},
      {"delete result", [](History &h) { FindOp(h, 2, Kind::kWriter, 4).Res = Result::kOK; }},
      {"unjustified rejection", [](History &h) { FindOp(h, 2, Kind::kRejected, 0).Delta = -1; }},
      {"committed violation", [](History &h) { FindOp(h, 2, Kind::kWriter, 2).Delta = -10; }},
      {"reader that wrote",
       [](History &h) {
         for (Log &l : h.logs)
           if (l.Ts == 2 && l.Kind == Kind::kReader)
             l.Ops.push_back(Op{.Kind = OpKind::kAdd, .PK = A, .Col = 0, .Delta = 1});
       }},
      {"skipped commit timestamp",
       [](History &h) {
         for (Log &l : h.logs)
           if (l.Ts == 3) l.Ts = 4;
         h.last = 4;
       }},
      {"last commit", [](History &h) { h.last = 2; }},
      {"final state", [](History &h) { h.dump[0].row = R({1, 31}); }},
      {"final row count", [](History &h) { h.dump.push_back(OwnedRec{B, R({7, 20})}); }},
  };
  for (const Case &c : cases) {
    History h = Good();
    c.alter(h);
    EXPECT_FALSE(ReplayOf(h).ok()) << c.name;
  }
}

// Recorded runs through the core replay cleanly, and altering a recorded
// read makes them fail.
TEST(Check, RecordedHistory) {
  DB db(Options{.mode = Mode::kCellDelta}, Schemas());
  Recorder rec;
  int yields = 0;
  auto yield = [&] { yields++; };
  std::vector<Value> row;
  bool found;
  {
    auto tx = rec.Begin(db, 1, yield);
    ASSERT_TRUE(tx->Insert(0, A, R({1, 10}).row()).ok());
    ASSERT_TRUE(tx->Insert(0, B, R({2, 20}).row()).ok());
    ASSERT_TRUE(tx->Commit().ok());
  }
  {
    auto t1 = rec.Begin(db, 1, yield), t2 = rec.Begin(db, 2, yield);
    ASSERT_TRUE(t1->Get(0, A, Col(0), &row, &found).ok());
    ASSERT_TRUE(found);
    ASSERT_TRUE(t1->Add(0, B, 0, 3).ok());
    ASSERT_TRUE(t2->Add(0, B, 0, -1).ok());
    EXPECT_EQ(t2->Insert(0, C, R({3, 10}).row()).code(), Code::kDuplicateKey);
    int sp = t2->Savepoint();
    ASSERT_TRUE(t2->Insert(0, C, R({3, 30}).row()).ok());
    t2->RollbackTo(sp);
    ASSERT_TRUE(t2->Commit().ok());
    ASSERT_TRUE(t1->Commit().ok());  // the deltas commute
  }
  {
    auto tx = rec.Begin(db, 3, yield);
    ASSERT_TRUE(tx->Add(0, A, 0, -2).ok());
    EXPECT_EQ(tx->Commit().code(), Code::kConstraint);
  }
  {
    auto tx = rec.Begin(db, 3, yield);
    std::vector<Rec> recs;
    ASSERT_TRUE(tx->Scan(0, "", "", Col(0), Col(1), [](std::string_view, Row r) { return r[0].i > 0; }, &recs).ok());
    EXPECT_EQ(recs.size(), 2u);
    ASSERT_TRUE(tx->IndexScan(0, 0, IntKey(15), "", Col(0), 0, nullptr, &recs).ok());
    EXPECT_EQ(recs.size(), 1u);
    ASSERT_TRUE(tx->Commit().ok());
  }
  {
    auto tx = rec.Begin(db, 4, yield);
    ASSERT_TRUE(tx->Delete(0, A).ok());
    tx->Abort();
  }
  EXPECT_EQ(yields, 22);  // every Begin, read, write and Commit
  std::vector<Log> logs = rec.Logs();
  ASSERT_EQ(logs.size(), 5u);
  EXPECT_EQ(logs[1].Client, 2);  // t2 committed first
  EXPECT_EQ(logs[3].Kind, Kind::kRejected);
  EXPECT_EQ(logs[4].Kind, Kind::kReader);
  for (const Log &l : logs) EXPECT_TRUE(l.Acked);
  auto dump = [&](int t) { return db.Dump(t); };
  Status st = Replay(Schemas(), logs, db.LastCommitTs(), dump);
  ASSERT_TRUE(st.ok()) << st.Message();

  std::vector<Log> bad = logs;
  bad[2].Ops[0].Vals = R({5, -1});  // t1's read of A
  EXPECT_FALSE(Replay(Schemas(), bad, db.LastCommitTs(), dump).ok());

  // A recovered history keeps what the recovered commit covers.
  EXPECT_EQ(rec.Truncate(1, true).code(), Code::kInternal);
  EXPECT_EQ(rec.Logs().size(), 5u);
  ASSERT_TRUE(rec.Truncate(1, false).ok());
  EXPECT_EQ(rec.Logs().size(), 1u);
}

// The trace hashes every outcome, so identical runs agree and a different
// read value changes it.
TEST(Check, TraceFollowsOutcomes) {
  auto run = [](int64_t b) {
    DB db(Options{}, Schemas());
    Recorder rec;
    auto tx = rec.Begin(db, 1, nullptr);
    EXPECT_TRUE(tx->Insert(0, A, R({1, b}).row()).ok());
    EXPECT_TRUE(tx->Commit().ok());
    tx = rec.Begin(db, 1, nullptr);
    std::vector<Value> row;
    bool found;
    EXPECT_TRUE(tx->Get(0, A, Col(1), &row, &found).ok());
    tx->Abort();
    return rec.Trace();
  };
  EXPECT_EQ(run(5), run(5));
  EXPECT_NE(run(5), run(6));

  Recorder off;
  off.off = true;
  DB db(Options{}, Schemas());
  uint64_t empty = off.Trace();
  auto tx = off.Begin(db, 1, nullptr);
  EXPECT_TRUE(tx->Insert(0, A, R({1, 2}).row()).ok());
  EXPECT_TRUE(tx->Commit().ok());
  EXPECT_EQ(off.Trace(), empty);
  EXPECT_TRUE(off.Logs().empty());
}

}  // namespace
}  // namespace celltnx::check
