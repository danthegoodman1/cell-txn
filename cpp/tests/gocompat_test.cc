// The C++ port's random numbers, float formatting and key encodings match
// Go's. The vectors come from testdata/randvec, fmtvec and keyvec:
//   go run ./cpp/tests/testdata/randvec > cpp/tests/testdata/rand_vectors.txt
//   go run ./cpp/tests/testdata/fmtvec > cpp/tests/testdata/float_vectors.txt
//   go run ./cpp/tests/testdata/keyvec > cpp/tests/testdata/key_vectors.txt
#include <gtest/gtest.h>

#include <bit>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "gofmt.h"
#include "keys.h"
#include "rand.h"

namespace celltnx {
namespace {

TEST(GoCompat, Rand) {
  std::ifstream in(CELLTNX_TESTDATA "/rand_vectors.txt");
  ASSERT_TRUE(in.good());
  std::unique_ptr<Rand> r;
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    std::istringstream is(line);
    std::string op;
    is >> op;
    char buf[64] = "";
    if (op == "seed") {
      unsigned long long a, b;
      is >> a >> b;
      r = std::make_unique<Rand>(a, b);
      continue;
    }
    if (op == "u") snprintf(buf, sizeof buf, "u %llu", (unsigned long long)r->Uint64());
    if (op == "i") {
      long long a;
      is >> a;
      snprintf(buf, sizeof buf, "i %lld %lld", a, (long long)r->IntN(a));
    }
    if (op == "n") {
      unsigned long long a;
      is >> a;
      snprintf(buf, sizeof buf, "n %llu %llu", a, (unsigned long long)r->Uint64N(a));
    }
    if (op == "l") {
      long long a;
      is >> a;
      snprintf(buf, sizeof buf, "l %lld %lld", a, (long long)r->Int64N(a));
    }
    if (op == "f") snprintf(buf, sizeof buf, "f %.17g", r->Float64());
    EXPECT_EQ(std::string(buf), line);
    n++;
  }
  EXPECT_EQ(n, 80);
}

TEST(GoCompat, Float) {
  std::ifstream in(CELLTNX_TESTDATA "/float_vectors.txt");
  ASSERT_TRUE(in.good());
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    std::istringstream is(line);
    std::string hex, v, g;
    is >> hex >> v >> g;
    double f = std::bit_cast<double>(std::stoull(hex, nullptr, 16));
    EXPECT_EQ(GoFloat(f), v) << hex;
    EXPECT_EQ(GoFloat(f), g) << hex;
    n++;
  }
  EXPECT_EQ(n, 6022);
}

std::string Hex(std::string_view s) {
  std::string h;
  char buf[3];
  for (unsigned char c : s) {
    snprintf(buf, sizeof buf, "%02x", c);
    h += buf;
  }
  return h;
}

std::string Unhex(std::string_view h) {
  std::string s;
  for (size_t i = 0; i + 1 < h.size(); i += 2) s.push_back(char(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
  return s;
}

TEST(GoCompat, Keys) {
  std::ifstream in(CELLTNX_TESTDATA "/key_vectors.txt");
  ASSERT_TRUE(in.good());
  std::string line, prev;
  int n = 0;
  while (std::getline(in, line)) {
    std::istringstream is(line);
    std::string kind, arg, want;
    is >> kind >> arg >> want;
    std::string got;
    if (kind == "u64")
      got = keys::U64(std::stoull(arg));
    else if (kind == "uint")
      keys::AppendUint(got, std::stoull(arg));
    else if (kind == "int")
      keys::AppendInt(got, std::stoll(arg));
    else if (kind == "float")
      keys::AppendFloat(got, std::bit_cast<double>(std::stoull(arg, nullptr, 16)));
    else if (kind == "string")
      keys::AppendString(got, Unhex(std::string_view(arg).substr(1)));
    else if (kind == "succ")
      got = keys::Succ(prev.empty() ? std::string("\xff\xff") : prev);
    else if (kind == "next")
      got = keys::Next(prev);
    else if (kind == "null")
      keys::AppendNull(got);
    else
      FAIL() << "unknown kind " << kind;
    EXPECT_EQ(Hex(got), want) << line;
    if (kind == "string") prev = got;
    if (kind == "next") prev.clear();
    n++;
  }
  EXPECT_EQ(n, 62);
}

}  // namespace
}  // namespace celltnx
