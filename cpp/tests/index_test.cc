// Secondary-index tests, ported from txn/index_test.go.
#include "testutil.h"

namespace celltnx::test {
namespace {

// people(name, email, age) with a unique index on email and a non-unique
// index on age.
constexpr int kAge = kWallet;

Schema PeopleSchema() {
  return Schema{
      .name = "people",
      .cols = {"name", "email", "age"},
      .indexes = {
          Index{.name = "email",
                .cols = Col(kEmail),
                .unique = true,
                .key =
                    [](Row r, std::string *out) {
                      if (r[kEmail].null()) {
                        keys::AppendNull(*out);
                        return false;
                      }
                      keys::AppendInt(*out, r[kEmail].i);
                      return true;
                    }},
          Index{.name = "age",
                .cols = Col(kAge),
                .key =
                    [](Row r, std::string *out) {
                      keys::AppendInt(*out, r[kAge].i);
                      return false;
                    }},
      }};
}

std::unique_ptr<DB> OpenPeople() {
  auto db = std::make_unique<DB>(Options{.mode = Mode::kCellDelta, .bucket_bits = 4}, std::vector{PeopleSchema()});
  auto tx = db->Begin();
  EXPECT_TRUE(tx->Insert(0, A, Ints({1, 100, 30})).ok());
  EXPECT_TRUE(tx->Insert(0, B, Ints({2, 200, 40})).ok());
  EXPECT_TRUE(tx->Commit().ok());
  return db;
}

std::pair<std::string, std::string> AgeRange(int64_t lo, int64_t hi) {
  std::string l, h;
  keys::AppendInt(l, lo);
  keys::AppendInt(h, hi);
  return {l, h};
}

#define WANT_DUP(expr) WANT_CODE(expr, ::celltnx::Code::kDuplicateKey)

TEST(Index, Unique) {
  auto db = OpenPeople();
  auto tx = db->Begin();
  WANT_DUP(tx->Set(0, B, Col(kEmail), Vals(kEmail, 100)));
  WANT_DUP(tx->Insert(0, K(3), Ints({3, 100, 1})));
  // Moving A off 100 frees it within the transaction.
  MUST(tx->Set(0, A, Col(kEmail), Vals(kEmail, 101)));
  MUST(tx->Set(0, B, Col(kEmail), Vals(kEmail, 100)));
  MUST(tx->Set(0, A, Col(kEmail), Vals(kEmail, 200)));
  MUST(tx->Commit());
  MUST(db->CheckInvariants());

  // Concurrent claims of the same free key: the second commit conflicts.
  auto t1 = db->Begin(), t2 = db->Begin();
  MUST(t1->Set(0, A, Col(kEmail), Vals(kEmail, 500)));
  MUST(t2->Set(0, B, Col(kEmail), Vals(kEmail, 500)));
  MUST(t1->Commit());
  WANT_CONFLICT(t2->Commit());

  // A blind rewrite of the same value conflicts with a concurrent move away
  // and a claim of the value by another row.
  t1 = db->Begin();
  MUST(t1->Set(0, A, Col(kEmail), Vals(kEmail, 500)));
  t2 = db->Begin();
  MUST(t2->Set(0, A, Col(kEmail), Vals(kEmail, 600)));
  MUST(t2->Commit());
  auto t3 = db->Begin();
  MUST(t3->Insert(0, K(9), Ints({9, 500, 1})));
  MUST(t3->Commit());
  WANT_CONFLICT(t1->Commit());
  MUST(db->CheckInvariants());
}

TEST(Index, Scan) {
  auto db = OpenPeople();
  auto tx = db->Begin();
  MUST(tx->Insert(0, K(3), Ints({3, 300, 35})));
  MUST(tx->Set(0, B, Col(kAge), Vals(kAge, 20)));
  auto [lo, hi] = AgeRange(25, 50);
  std::vector<Rec> recs;
  MUST(tx->IndexScan(0, 1, lo, hi, 0, Col(kName), nullptr, &recs));
  std::vector<int64_t> names;
  for (auto &r : recs) names.push_back(r.row[kName].i);
  EXPECT_EQ(names, (std::vector<int64_t>{1, 3}));
  MUST(tx->Commit());
  MUST(db->CheckInvariants());

  // A concurrent insert into the scanned range is a phantom.
  auto t1 = db->Begin(), t2 = db->Begin();
  MUST(t1->IndexScan(0, 1, lo, hi, 0, 0, nullptr, &recs));
  MUST(t1->Set(0, A, Col(kName), Vals(kName, 7)));
  MUST(t2->Insert(0, K(4), Ints({4, 400, 45})));
  MUST(t2->Commit());
  WANT_CONFLICT(t1->Commit());

  // So is moving a row into the range.
  t1 = db->Begin();
  t2 = db->Begin();
  MUST(t1->IndexScan(0, 1, lo, hi, 0, 0, nullptr, &recs));
  MUST(t1->Set(0, A, Col(kName), Vals(kName, 8)));
  MUST(t2->Set(0, B, Col(kAge), Vals(kAge, 26)));
  MUST(t2->Commit());
  WANT_CONFLICT(t1->Commit());
  MUST(db->CheckInvariants());
}

// Deltas on indexed columns read the column, so they never commute.
TEST(Index, IndexedDeltasConflict) {
  auto db = OpenPeople();
  auto t1 = db->Begin(), t2 = db->Begin();
  MUST(t1->Add(0, A, kAge, 1));
  MUST(t2->Add(0, A, kAge, 1));
  MUST(t2->Commit());
  WANT_CONFLICT(t1->Commit());
  MUST(db->CheckInvariants());
}

}  // namespace
}  // namespace celltnx::test
