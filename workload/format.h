// Float formatting for the simulator's parameter lines and the checker's
// messages, identical on every platform.
#pragma once

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace celltnx {

// FormatFloat formats f with the shortest digits that parse back to it, in
// exponent form (1e-05, 1.5e+06) when the exponent is below -4 or at least
// 6.
inline std::string FormatFloat(double f) {
  if (std::isnan(f)) return "NaN";
  if (std::isinf(f)) return f > 0 ? "+Inf" : "-Inf";
  if (f == 0) return std::signbit(f) ? "-0" : "0";
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, f, std::chars_format::scientific);
  std::string s(buf, res.ptr);
  bool neg = s[0] == '-';
  if (neg) s.erase(0, 1);
  size_t e = s.find('e');
  int exp = std::atoi(s.c_str() + e + 1);
  std::string digits;
  for (size_t i = 0; i < e; i++)
    if (s[i] != '.') digits += s[i];
  int nd = int(digits.size());
  std::string out = neg ? "-" : "";
  if (exp < -4 || exp >= 6) {
    out += digits[0];
    if (nd > 1) out += "." + digits.substr(1);
    out += exp < 0 ? "e-" : "e+";
    int a = std::abs(exp);
    if (a < 10) out += '0';
    out += std::to_string(a);
    return out;
  }
  int dp = exp + 1;  // digits before the decimal point
  if (dp <= 0) return out + "0." + std::string(-dp, '0') + digits;
  if (dp >= nd) return out + digits + std::string(dp - nd, '0');
  return out + digits.substr(0, dp) + "." + digits.substr(dp);
}

}  // namespace celltnx
