// SPEC.md scenarios 1–9 and the core's other tests, ported from
// txn/txn_test.go.
#include <atomic>
#include <mutex>
#include <thread>

#include "testutil.h"

namespace celltnx::test {
namespace {

// Scenario 1 and 9: a read of A.email and a write of A.wallet are disjoint.
TEST(Txn, DisjointColumnsMerge) {
  for (Mode mode : kModes) {
    SCOPED_TRACE(ModeName(mode));
    auto db = Open(mode, 50);
    auto t1 = db->Begin(), t2 = db->Begin();
    std::vector<Value> r;
    bool found;
    MUST(t1->Get(kUsers, A, Col(kEmail), &r, &found));
    MUST(t1->Set(kUsers, A, Col(kName), Vals(kName, 99)));
    MUST(t2->Set(kUsers, A, Col(kWallet), Vals(kWallet, 77)));
    MUST(t2->Commit());
    Status st = t1->Commit();
    if (mode == Mode::kRow) {
      WANT_CONFLICT(std::move(st));
      continue;
    }
    MUST(std::move(st));
    OwnedRow got = RowOf(*db, A);
    EXPECT_EQ(got[kName].i, 99);
    EXPECT_EQ(got[kEmail].i, 11);
    EXPECT_EQ(got[kWallet].i, 77);
  }
}

// Scenario 2.
TEST(Txn, ReadWriteConflict) {
  auto db = Open(Mode::kCell, 50);
  auto t1 = db->Begin(), t2 = db->Begin();
  std::vector<Value> r;
  bool found;
  MUST(t2->Get(kUsers, A, Col(kEmail), &r, &found));
  MUST(t2->Set(kUsers, B, Col(kName), Vals(kName, 1)));
  MUST(t1->Set(kUsers, A, Col(kEmail), Vals(kEmail, 12)));
  MUST(t1->Commit());
  WANT_CONFLICT(t2->Commit());
}

// Scenario 3.
TEST(Txn, WriteSkew) {
  auto db = Open(Mode::kCellDelta, 50);
  auto t1 = db->Begin(), t2 = db->Begin();
  std::vector<Value> r;
  bool found;
  for (Txn *tx : {t1.get(), t2.get()})
    for (const std::string &pk : {A, B}) MUST(tx->Get(kUsers, pk, Col(kWallet), &r, &found));
  MUST(t1->Set(kUsers, A, Col(kWallet), Vals(kWallet, 0)));
  MUST(t2->Set(kUsers, B, Col(kWallet), Vals(kWallet, 0)));
  MUST(t1->Commit());
  WANT_CONFLICT(t2->Commit());
}

// Scenario 4.
TEST(Txn, Phantom) {
  for (auto [insert, abort] : {std::pair<uint64_t, bool>{5, true}, {40, false}}) {
    auto db = Open(Mode::kCell, 50);
    auto t1 = db->Begin(), t2 = db->Begin();
    std::vector<Rec> recs;
    MUST(t1->Scan(kUsers, K(0), K(16), Col(kWallet), Col(kEmail), nullptr, &recs));
    ASSERT_EQ(recs.size(), 2u);
    MUST(t1->Set(kUsers, B, Col(kName), Vals(kName, 1)));
    MUST(t2->Insert(kUsers, K(insert), Ints({0, 0, 0})));
    MUST(t2->Commit());
    if (abort)
      WANT_CONFLICT(t1->Commit());
    else
      MUST(t1->Commit());
  }
}

// Scenario 5.
TEST(Txn, AbsentKeys) {
  for (auto [insert, abort] : {std::pair<uint64_t, bool>{7, true}, {8, false}}) {
    auto db = Open(Mode::kCell, 50);
    auto t1 = db->Begin(), t2 = db->Begin();
    std::vector<Value> r;
    bool found = true;
    MUST(t1->Get(kUsers, K(7), Col(kName), &r, &found));
    ASSERT_FALSE(found);
    MUST(t1->Set(kUsers, A, Col(kName), Vals(kName, 1)));
    MUST(t2->Insert(kUsers, K(insert), Ints({0, 0, 0})));
    MUST(t2->Commit());
    if (abort)
      WANT_CONFLICT(t1->Commit());
    else
      MUST(t1->Commit());
  }

  DB db(Options{.mode = Mode::kCell, .bucket_bits = 8}, {UsersSchema(), OtherSchema()});
  std::vector<std::unique_ptr<Txn>> txs;
  for (int i = 0; i < 100; i++) {
    auto tx = db.Begin();
    MUST(tx->Insert(kOther, K(256 + i), Ints({i})));  // all in bucket 1
    txs.push_back(std::move(tx));
  }
  for (auto &tx : txs) MUST(tx->Commit());

  auto t1 = db.Begin(), t2 = db.Begin();
  MUST(t1->Insert(kOther, K(5000), Ints({1})));
  MUST(t2->Insert(kOther, K(5000), Ints({2})));
  MUST(t1->Commit());
  WANT_CONFLICT(t2->Commit());
}

// Scenario 6.
TEST(Txn, SetConflictsWithDelete) {
  auto db = Open(Mode::kCellDelta, 50);
  auto t1 = db->Begin(), t2 = db->Begin();
  MUST(t1->Set(kUsers, A, Col(kEmail), Vals(kEmail, 5)));
  MUST(t2->Delete(kUsers, A));
  MUST(t2->Commit());
  WANT_CONFLICT(t1->Commit());
}

// Scenario 7, on real threads.
TEST(Txn, ConcurrentDeltas) {
  auto db = Open(Mode::kCellDelta, 1000);
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 100; i++)
    threads.emplace_back([&] {
      auto tx = db->Begin();
      if (!tx->Add(kUsers, A, kWallet, -1).ok()) failures++;
      if (!tx->Commit().ok()) failures++;
    });
  for (auto &t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(RowOf(*db, A)[kWallet].i, 900);
}

// Scenario 8, on real threads.
TEST(Txn, DeltaCheck) {
  auto db = Open(Mode::kCellDelta, 5);
  std::mutex mu;
  int ok = 0, constraint = 0, other = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < 10; i++)
    threads.emplace_back([&] {
      auto tx = db->Begin();
      Status add = tx->Add(kUsers, A, kWallet, -1);
      Status st = tx->Commit();
      std::lock_guard g(mu);
      if (!add.ok())
        other++;
      else if (st.ok())
        ok++;
      else if (st.code() == Code::kConstraint)
        constraint++;
      else
        other++;
    });
  for (auto &t : threads) t.join();
  EXPECT_EQ(ok, 5);
  EXPECT_EQ(constraint, 5);
  EXPECT_EQ(other, 0);
}

// Deltas conflict unless the mode lets them commute.
TEST(Txn, DeltaModes) {
  for (Mode mode : kModes) {
    SCOPED_TRACE(ModeName(mode));
    auto db = Open(mode, 50);
    auto t1 = db->Begin(), t2 = db->Begin();
    MUST(t1->Add(kUsers, A, kWallet, 1));
    MUST(t2->Add(kUsers, A, kWallet, 1));
    MUST(t2->Commit());
    if (mode == Mode::kCellDelta)
      MUST(t1->Commit());
    else
      WANT_CONFLICT(t1->Commit());
  }
}

// Reading a column with a pending delta makes the delta read its column.
TEST(Txn, ReadingDeltaRecordsRead) {
  auto db = Open(Mode::kCellDelta, 50);
  auto t1 = db->Begin(), t2 = db->Begin();
  MUST(t1->Add(kUsers, A, kWallet, 1));
  std::vector<Value> r;
  bool found;
  MUST(t1->Get(kUsers, A, Col(kWallet), &r, &found));
  ASSERT_EQ(r[kWallet].i, 51);
  MUST(t2->Add(kUsers, A, kWallet, 1));
  MUST(t2->Commit());
  WANT_CONFLICT(t1->Commit());
}

TEST(Txn, OverflowIsConstraint) {
  auto db = Open(Mode::kCellDelta, int64_t(1) << 62);
  auto tx = db->Begin();
  MUST(tx->Add(kUsers, A, kWallet, int64_t(1) << 62));
  WANT_CODE(tx->Commit(), Code::kConstraint);
}

TEST(Txn, Savepoint) {
  auto db = Open(Mode::kCell, 50);
  auto tx = db->Begin();
  MUST(tx->Set(kUsers, A, Col(kName), Vals(kName, 1)));
  int sp = tx->Savepoint();
  MUST(tx->Set(kUsers, A, Col(kName), Vals(kName, 2)));
  MUST(tx->Delete(kUsers, B));
  MUST(tx->Insert(kUsers, K(3), Ints({3, 3, 3})));
  tx->RollbackTo(sp);
  MUST(tx->Commit());
  EXPECT_EQ(RowOf(*db, A)[kName].i, 1);
  RowOf(*db, B);
  auto check = db->Begin();
  std::vector<Value> r;
  bool found = true;
  MUST(check->Get(kUsers, K(3), Col(kName), &r, &found));
  EXPECT_FALSE(found) << "row 3 survived rollback";
}

// A long reader keeps its snapshot while GC trims behind newer writers.
TEST(Txn, GCKeepsSnapshots) {
  auto db = Open(Mode::kCell, 50);
  auto old = db->Begin();
  for (int i = 0; i < 10; i++) {
    auto tx = db->Begin();
    MUST(tx->Set(kUsers, A, Col(kWallet), Vals(kWallet, i)));
    MUST(tx->Commit());
  }
  std::vector<Value> r;
  bool found;
  MUST(old->Get(kUsers, A, Col(kWallet), &r, &found));
  ASSERT_TRUE(found);
  EXPECT_EQ(r[kWallet].i, 50);
  EXPECT_GE(db->ChainLength(kUsers, A), 11);
  old->Abort();
  // The chain trims once its installs since the last trim reach the
  // versions that trim kept, here at most the 11 the old reader held.
  for (int i = 0; i < 12 && db->ChainLength(kUsers, A) > 2; i++) {
    auto tx = db->Begin();
    MUST(tx->Set(kUsers, A, Col(kWallet), Vals(kWallet, 99)));
    MUST(tx->Commit());
  }
  EXPECT_EQ(db->ChainLength(kUsers, A), 2);
  // From there it stays within twice the two live versions.
  for (int i = 0; i < 20; i++) {
    auto tx = db->Begin();
    MUST(tx->Set(kUsers, A, Col(kWallet), Vals(kWallet, i)));
    MUST(tx->Commit());
    EXPECT_LE(db->ChainLength(kUsers, A), 3);
  }
  MUST(db->CheckInvariants());
}

// Validation checks a run of deltas once, yet still finds a conflicting
// write behind the run, and stops at the snapshot inside a run.
TEST(Txn, ValidateAcrossDeltaRuns) {
  auto db = Open(Mode::kCellDelta, 1000);
  auto behind = db->Begin();
  std::vector<Value> r;
  bool found;
  MUST(behind->Get(kUsers, A, Col(kEmail), &r, &found));
  MUST(behind->Set(kUsers, B, Col(kName), Vals(kName, 1)));
  auto w = db->Begin();
  MUST(w->Set(kUsers, A, Col(kEmail), Vals(kEmail, 12)));
  MUST(w->Commit());
  for (int i = 0; i < 2; i++) {
    auto d = db->Begin();
    MUST(d->Add(kUsers, A, kWallet, -1));
    MUST(d->Commit());
  }
  auto inside = db->Begin();
  MUST(inside->Get(kUsers, A, Col(kEmail), &r, &found));
  MUST(inside->Set(kUsers, B, Col(kName), Vals(kName, 2)));
  for (int i = 0; i < 3; i++) {
    auto d = db->Begin();
    MUST(d->Add(kUsers, A, kWallet, -1));
    MUST(d->Commit());
  }
  WANT_CONFLICT(behind->Commit());
  MUST(inside->Commit());
  EXPECT_EQ(RowOf(*db, A)[kWallet].i, 995);
}

// Scans see the transaction's own inserts, updates and deletes.
TEST(Txn, ScanOwnWrites) {
  auto db = Open(Mode::kCell, 50);
  auto tx = db->Begin();
  MUST(tx->Insert(kUsers, K(0), Ints({0, 0, 1})));
  MUST(tx->Set(kUsers, A, Col(kWallet), Vals(kWallet, 7)));
  MUST(tx->Delete(kUsers, B));
  MUST(tx->Insert(kUsers, K(9), Ints({9, 9, 9})));
  std::vector<Rec> recs;
  MUST(tx->Scan(kUsers, K(0), K(100), Col(kWallet), 0, nullptr, &recs));
  std::vector<std::pair<std::string, int64_t>> got, want = {{K(0), 1}, {A, 7}, {K(9), 9}};
  for (auto &r : recs) got.emplace_back(std::string(r.pk), r.row[kWallet].i);
  EXPECT_EQ(got, want);
}

TEST(Txn, ReadOnlyCommitsAtReadTs) {
  auto db = Open(Mode::kCell, 50);
  auto tx = db->Begin();
  std::vector<Value> r;
  bool found;
  MUST(tx->Get(kUsers, A, Col(kWallet), &r, &found));
  auto w = db->Begin();
  MUST(w->Set(kUsers, A, Col(kWallet), Vals(kWallet, 1)));
  MUST(w->Commit());
  MUST(tx->Commit());
  EXPECT_FALSE(tx->Wrote());
  EXPECT_EQ(tx->SerialTs(), tx->ReadTs());
}

// A reused transaction reads a fresh snapshot and carries nothing from
// its previous life: neither reads to validate nor writes to install.
TEST(Txn, BeginReuse) {
  auto db = Open(Mode::kCell, 50);
  auto tx = db->Begin();
  std::vector<Value> r;
  bool found;
  MUST(tx->Get(kUsers, A, Col(kEmail), &r, &found));
  MUST(tx->Set(kUsers, B, Col(kName), Vals(kName, 7)));
  tx->Abort();
  auto w = db->Begin();
  MUST(w->Set(kUsers, A, Col(kEmail), Vals(kEmail, 3)));
  MUST(w->Commit());

  Txn *same = tx.get();
  tx = db->Begin(std::move(tx));
  ASSERT_EQ(tx.get(), same);
  EXPECT_EQ(tx->ReadTs(), w->SerialTs());
  MUST(tx->Get(kUsers, A, Col(kEmail), &r, &found));
  EXPECT_EQ(r[kEmail].i, 3);
  MUST(tx->Get(kUsers, B, Col(kName), &r, &found));
  EXPECT_EQ(r[kName].i, 20);
  MUST(tx->Commit());
  EXPECT_FALSE(tx->Wrote());

  // A committed transaction is reusable too, and its new writes install.
  tx = db->Begin(std::move(tx));
  MUST(tx->Set(kUsers, B, Col(kName), Vals(kName, 9)));
  MUST(tx->Commit());
  auto rd = db->Begin();
  MUST(rd->Get(kUsers, B, Col(kName), &r, &found));
  EXPECT_EQ(r[kName].i, 9);
}

// Refresh moves the snapshot past commits that touched only cells the
// transaction did not read; later reads see those commits.
TEST(Txn, RefreshPastDisjointWrites) {
  auto db = Open(Mode::kCell, 50);
  auto tx = db->Begin();
  std::vector<Value> r;
  bool found;
  MUST(tx->Get(kUsers, A, Col(kWallet), &r, &found));
  auto w = db->Begin();
  MUST(w->Set(kUsers, A, Col(kEmail), Vals(kEmail, 5)));
  MUST(w->Set(kUsers, B, Col(kWallet), Vals(kWallet, 7)));
  MUST(w->Commit());
  MUST(tx->Refresh());
  EXPECT_EQ(tx->ReadTs(), w->SerialTs());
  MUST(tx->Get(kUsers, B, Col(kWallet), &r, &found));
  EXPECT_EQ(r[kWallet].i, 7);
  MUST(tx->Set(kUsers, A, Col(kWallet), Vals(kWallet, r[kWallet].i + 1)));
  MUST(tx->Commit());
}

// Refresh fails when a commit since the snapshot wrote a cell the
// transaction read, or changed a bucket it scanned.
TEST(Txn, RefreshConflicts) {
  {
    auto db = Open(Mode::kCell, 50);
    auto tx = db->Begin();
    std::vector<Value> r;
    bool found;
    MUST(tx->Get(kUsers, A, Col(kWallet), &r, &found));
    auto w = db->Begin();
    MUST(w->Add(kUsers, A, kWallet, 1));
    MUST(w->Commit());
    WANT_CONFLICT(tx->Refresh());
    // The snapshot stays where it was: a write still conflicts at commit.
    EXPECT_EQ(tx->ReadTs(), w->SerialTs() - 1);
    MUST(tx->Set(kUsers, B, Col(kName), Vals(kName, 1)));
    WANT_CONFLICT(tx->Commit());
  }
  {
    auto db = Open(Mode::kCell, 50);
    auto tx = db->Begin();
    std::vector<Rec> recs;
    MUST(tx->Scan(kUsers, K(0), K(16), Col(kWallet), Col(kEmail), nullptr, &recs));
    auto w = db->Begin();
    MUST(w->Insert(kUsers, K(5), Ints({0, 0, 0})));
    MUST(w->Commit());
    WANT_CONFLICT(tx->Refresh());
  }
}

// Rows returned before a refresh stay valid while later commits trim the
// versions they came from.
TEST(Txn, RefreshPinsEarlierRows) {
  Schema s{.name = "docs", .cols = {"a", "b"}};
  DB db(Options{.mode = Mode::kCell}, {s});
  std::string first(64, 'f');
  auto load = db.Begin();
  std::vector<Value> row = {Value::Str(first), Value::Str("")};
  MUST(load->Insert(0, K(1), row));
  MUST(load->Commit());

  auto tx = db.Begin();
  std::vector<Rec> recs;
  MUST(tx->Scan(0, K(0), K(9), Col(0), Col(0), nullptr, &recs));
  ASSERT_EQ(recs.size(), 1u);
  for (int i = 0; i < 2; i++) {
    std::string b = "b" + std::to_string(i);
    std::vector<Value> vals = {Value(), Value::Str(b)};
    auto w = db.Begin();
    MUST(w->Set(0, K(1), Col(1), vals));
    MUST(w->Commit());
    MUST(tx->Refresh());
  }
  for (int i = 0; i < 3; i++) {
    std::string a(64, char('0' + i));
    std::vector<Value> vals = {Value::Str(a), Value()};
    auto w = db.Begin();
    MUST(w->Set(0, K(1), Col(0), vals));
    MUST(w->Commit());
  }
  EXPECT_EQ(recs[0].row[0].str(), first);
  EXPECT_GE(db.ChainLength(0, K(1)), 6);
  tx->Abort();
  for (int i = 0; i < 8 && db.ChainLength(0, K(1)) > 2; i++) {
    auto w = db.Begin();
    std::vector<Value> vals = {Value(), Value::Str("z")};
    MUST(w->Set(0, K(1), Col(1), vals));
    MUST(w->Commit());
  }
  EXPECT_EQ(db.ChainLength(0, K(1)), 2);
  MUST(db.CheckInvariants());
}

// Size limits: a write past the row limit fails at once, and a merge that
// grows past it fails at commit before anything installs.
TEST(Txn, RowSizeLimit) {
  Schema s{.name = "docs", .cols = {"a", "b"}};
  DB db(Options{.mode = Mode::kCellDelta, .max_key_bytes = 16, .max_row_bytes = 64}, {s});
  std::string small(20, 'x'), big(70, 'y');
  auto load = db.Begin();
  std::vector<Value> row = {Value::Str(""), Value::Str("")};
  MUST(load->Insert(0, K(1), row));
  MUST(load->Commit());

  auto tx = db.Begin();
  WANT_CODE(tx->Insert(0, std::string(17, 'k'), row), Code::kTooLarge);
  std::vector<Value> vals = {Value::Str(big), Value()};
  WANT_CODE(tx->Set(0, K(1), Col(0), vals), Code::kTooLarge);
  tx->Abort();

  // Each transaction grows a different column within the limit; the second
  // merge would pass it.
  auto t1 = db.Begin(), t2 = db.Begin();
  std::string twice = small + small;
  std::vector<Value> a = {Value::Str(small), Value()}, b = {Value(), Value::Str(twice)};
  MUST(t1->Set(0, K(1), Col(0), a));
  MUST(t2->Set(0, K(1), Col(1), b));
  MUST(t1->Commit());
  uint64_t before = db.LastCommitTs();
  WANT_CODE(t2->Commit(), Code::kConstraint);
  EXPECT_EQ(db.LastCommitTs(), before);
  MUST(db.CheckInvariants());
}

}  // namespace
}  // namespace celltnx::test
