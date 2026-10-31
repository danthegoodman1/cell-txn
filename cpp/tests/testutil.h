// Helpers shared by the core's tests.
#pragma once

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "keys.h"

namespace celltnx::test {

#define MUST(expr)                                  \
  do {                                              \
    ::celltnx::Status st_ = (expr);                 \
    ASSERT_TRUE(st_.ok()) << st_.Message();         \
  } while (0)

#define WANT_CODE(expr, want)                                  \
  do {                                                         \
    ::celltnx::Status st_ = (expr);                            \
    ASSERT_EQ(st_.code(), (want)) << st_.Message();            \
  } while (0)

#define WANT_CONFLICT(expr) WANT_CODE(expr, ::celltnx::Code::kConflict)

enum { kName, kEmail, kWallet };
enum { kUsers, kOther };

inline std::string K(uint64_t i) { return keys::U64(i); }
inline const std::string A = K(1), B = K(2);

inline std::vector<Value> Ints(std::initializer_list<int64_t> vs) {
  std::vector<Value> r;
  for (int64_t v : vs) r.push_back(Value::Int(v));
  return r;
}

// Vals is a 3-wide row with only column c set.
inline std::vector<Value> Vals(int c, int64_t v) {
  std::vector<Value> r(3);
  r[c] = Value::Int(v);
  return r;
}

inline Schema UsersSchema() {
  return Schema{.name = "users",
                .cols = {"name", "email", "wallet"},
                .checks = {Check{"wallet >= 0", Col(kWallet), [](Row r) { return r[kWallet].i >= 0; }}}};
}

inline Schema OtherSchema() { return Schema{.name = "other", .cols = {"v"}}; }

inline constexpr ColMask kAll = Col(kName) | Col(kEmail) | Col(kWallet);

// Open returns a DB with users A and B, wallets set to wallet_a and 100.
inline std::unique_ptr<DB> Open(Mode mode, int64_t wallet_a) {
  auto db = std::make_unique<DB>(Options{.mode = mode, .bucket_bits = 4},
                                 std::vector<Schema>{UsersSchema(), OtherSchema()});
  auto tx = db->Begin();
  EXPECT_TRUE(tx->Insert(kUsers, A, Ints({10, 11, wallet_a})).ok());
  EXPECT_TRUE(tx->Insert(kUsers, B, Ints({20, 21, 100})).ok());
  EXPECT_TRUE(tx->Commit().ok());
  return db;
}

// RowOf reads every column of a user in a new transaction.
inline OwnedRow RowOf(DB &db, const std::string &pk) {
  auto tx = db.Begin();
  std::vector<Value> r;
  bool found = false;
  EXPECT_TRUE(tx->Get(kUsers, pk, kAll, &r, &found).ok());
  EXPECT_TRUE(found) << "row missing";
  OwnedRow out(r);
  tx->Abort();
  return out;
}

inline const Mode kModes[] = {Mode::kRow, Mode::kCell, Mode::kCellDelta};

}  // namespace celltnx::test
