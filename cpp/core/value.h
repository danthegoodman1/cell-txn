// Column values, column masks and immutable row images.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace celltnx {

// ColMask is a set of columns. Bit 63 is kExists, the row's existence.
using ColMask = uint64_t;
inline constexpr ColMask kExists = ColMask(1) << 63;
inline constexpr int kMaxCols = 63;

constexpr ColMask Col(int i) { return ColMask(1) << i; }

// EachCol calls f for every column in m, in order, excluding kExists.
template <class F>
inline void EachCol(ColMask m, F &&f) {
  for (m &= ~kExists; m != 0; m &= m - 1) f(std::countr_zero(m));
}

enum class Type : uint8_t { kNull, kInt, kUint, kFloat, kString };

// Value is a column value: NULL, int64, uint64, double or a byte string. A
// string value points at bytes owned elsewhere: a row image, a
// transaction's arena, or the caller.
struct Value {
  Type type = Type::kNull;
  uint32_t len = 0;
  union {
    int64_t i;
    uint64_t u;
    double f;
    const char *s;
  };

  Value() : i(0) {}
  static Value Int(int64_t x) {
    Value v;
    v.type = Type::kInt;
    v.i = x;
    return v;
  }
  static Value Uint(uint64_t x) {
    Value v;
    v.type = Type::kUint;
    v.u = x;
    return v;
  }
  static Value Float(double x) {
    Value v;
    v.type = Type::kFloat;
    v.f = x;
    return v;
  }
  static Value Str(std::string_view x) {
    Value v;
    v.type = Type::kString;
    v.len = uint32_t(x.size());
    v.s = x.data();
    return v;
  }

  bool null() const { return type == Type::kNull; }
  std::string_view str() const { return {s, len}; }

  // Equality matches Go's == on interface values: the same type and an
  // equal value, so NaN differs from itself and -0 equals +0.
  friend bool operator==(const Value &a, const Value &b) {
    if (a.type != b.type) return false;
    switch (a.type) {
      case Type::kNull:
        return true;
      case Type::kInt:
        return a.i == b.i;
      case Type::kUint:
        return a.u == b.u;
      case Type::kFloat:
        return a.f == b.f;
      case Type::kString:
        return a.str() == b.str();
    }
    return false;
  }
};

using Row = std::span<const Value>;

// EncodedSize is a row's size in the canonical encoding that row-size
// limits measure: a type byte per value, 8 bytes per number, and a 4-byte
// length plus the bytes per string.
inline size_t EncodedSize(Row row) {
  size_t n = 0;
  for (const Value &v : row) {
    n += 1;
    if (v.type == Type::kString)
      n += 4 + v.len;
    else if (v.type != Type::kNull)
      n += 8;
  }
  return n;
}

// StringBytes is the total length of a row's strings.
inline size_t StringBytes(Row row) {
  size_t n = 0;
  for (const Value &v : row)
    if (v.type == Type::kString) n += v.len;
  return n;
}

// CopyRow copies row into vals and its strings into bytes, which must hold
// StringBytes(row), pointing each string value at its copy.
inline void CopyRow(Row row, Value *vals, char *bytes) {
  for (size_t i = 0; i < row.size(); i++) {
    vals[i] = row[i];
    if (row[i].type == Type::kString) {
      if (row[i].len) memcpy(bytes, row[i].s, row[i].len);
      vals[i].s = bytes;
      bytes += row[i].len;
    }
  }
}

// OwnedRow is a row that owns its strings, for callers that keep rows past
// the transaction that read them.
class OwnedRow {
 public:
  OwnedRow() = default;
  explicit OwnedRow(Row row) { Assign(row); }
  OwnedRow(const OwnedRow &o) { Assign(o.row()); }
  OwnedRow &operator=(const OwnedRow &o) {
    if (this != &o) Assign(o.row());
    return *this;
  }
  OwnedRow(OwnedRow &&) = default;
  OwnedRow &operator=(OwnedRow &&) = default;

  void Assign(Row row) {
    vals_.assign(row.size(), Value());
    bytes_.resize(StringBytes(row));
    CopyRow(row, vals_.data(), bytes_.data());
  }
  Row row() const { return vals_; }
  const Value &operator[](size_t i) const { return vals_[i]; }
  size_t size() const { return vals_.size(); }
  bool empty() const { return vals_.empty(); }
  friend bool operator==(const OwnedRow &a, const OwnedRow &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
      if (!(a.vals_[i] == b.vals_[i])) return false;
    return true;
  }

 private:
  std::vector<Value> vals_;
  std::vector<char> bytes_;  // a vector keeps its buffer when moved
};

}  // namespace celltnx
