// Order-preserving key encodings. Every encoding is prefix-free, so encoded
// values concatenate into composite keys that sort component by component.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace celltnx::keys {

// U64 returns the 8-byte big-endian encoding of x.
inline std::string U64(uint64_t x) {
  std::string k(8, '\0');
  for (int i = 7; i >= 0; i--, x >>= 8) k[i] = char(x & 0xff);
  return k;
}

// ParseU64 decodes a key produced by U64.
inline uint64_t ParseU64(std::string_view k) {
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) x = x << 8 | uint8_t(k[i]);
  return x;
}

// Tags distinguish NULL from values; NULL sorts first, as in MySQL.
inline constexpr char kTagNull = 0x01;
inline constexpr char kTagValue = 0x02;

inline void AppendBE64(std::string &b, uint64_t x) {
  for (int i = 7; i >= 0; i--) b.push_back(char(x >> (8 * i)));
}

// AppendNull appends a NULL.
inline void AppendNull(std::string &b) { b.push_back(kTagNull); }

// AppendInt appends a signed integer.
inline void AppendInt(std::string &b, int64_t x) {
  b.push_back(kTagValue);
  AppendBE64(b, uint64_t(x) ^ (uint64_t(1) << 63));
}

// AppendUint appends an unsigned integer.
inline void AppendUint(std::string &b, uint64_t x) {
  b.push_back(kTagValue);
  AppendBE64(b, x);
}

// AppendFloat appends a float; -0 and +0 encode equally.
inline void AppendFloat(std::string &b, double f) {
  if (f == 0) f = 0;
  uint64_t u = std::bit_cast<uint64_t>(f);
  if (u & (uint64_t(1) << 63))
    u = ~u;
  else
    u |= uint64_t(1) << 63;
  b.push_back(kTagValue);
  AppendBE64(b, u);
}

// AppendString appends a string, escaping 0x00 as 0x00 0xff and ending
// with 0x00 0x01, so shorter strings sort before their extensions.
inline void AppendString(std::string &b, std::string_view s) {
  b.push_back(kTagValue);
  for (char c : s) {
    if (c == 0) {
      b.push_back(0);
      b.push_back(char(0xff));
    } else {
      b.push_back(c);
    }
  }
  b.push_back(0);
  b.push_back(0x01);
}

// Succ returns the smallest key greater than every key with prefix k, or
// "" (unbounded) when no such key exists.
inline std::string Succ(std::string_view k) {
  std::string b(k);
  for (int i = int(b.size()) - 1; i >= 0; i--) {
    if (uint8_t(b[i]) != 0xff) {
      b[i] = char(uint8_t(b[i]) + 1);
      b.resize(i + 1);
      return b;
    }
  }
  return "";
}

// Next returns the smallest key greater than k.
inline std::string Next(std::string_view k) {
  std::string b(k);
  b.push_back(0);
  return b;
}

}  // namespace celltnx::keys
