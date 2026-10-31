// Go's math.Pow as it runs on amd64, bit for bit, so the Zipf generator
// draws the same ranks in both ports. Go's Pow is pure Go over math.Exp
// and math.Log, which on amd64 are assembly (exp_amd64.s, log_amd64.s);
// Exp takes a fused multiply-add path when the CPU has AVX and FMA, so
// GoExp dispatches the same way. Callers must compile without
// floating-point contraction (-ffp-contract=off) so every other operation
// rounds as Go's does.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace celltnx {

namespace gomath {

inline constexpr uint64_t kSign = uint64_t(1) << 63;
inline constexpr uint64_t kPosInf = 0x7FF0000000000000ull;
inline constexpr uint64_t kNegInf = 0xFFF0000000000000ull;

inline double Bits(uint64_t b) { return std::bit_cast<double>(b); }
inline uint64_t Of(double f) { return std::bit_cast<uint64_t>(f); }
inline double Inf(int sign) { return sign >= 0 ? Bits(kPosInf) : Bits(kNegInf); }
// NaN is math.NaN(), whose payload differs from the C++ default.
inline double NaN() { return Bits(0x7FF8000000000001ull); }

// UseFMA mirrors math.useFMA: cpu.X86.HasAVX && cpu.X86.HasFMA. Both
// checks include the OS's support for AVX state, as Go's do.
inline bool UseFMA() {
#if defined(__x86_64__)
  static const bool v = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma");
  }();
  return v;
#else
  return false;
#endif
}

// CvtSD2SL is the CVTSD2SL instruction: round to nearest even, or
// INT32_MIN when the result is NaN or out of range.
inline int32_t CvtSD2SL(double t) {
  double r = std::nearbyint(t);
  if (!(r >= -2147483648.0 && r <= 2147483647.0)) return std::numeric_limits<int32_t>::min();
  return int32_t(r);
}

// Exp is math.archExp on amd64; fma selects its AVX+FMA path.
inline double Exp(double x, bool fma) {
  constexpr double kLog2e = 1.4426950408889634073599246810018920;
  constexpr double kLn2U = 0.69314718055966295651160180568695068359375;
  constexpr double kLn2L = 0.28235290563031577122588448175013436025525412068e-12;
  constexpr double kOverflow = 7.09782712893384e+02;
  constexpr double c0 = 0.5, c8 = 1.0, c16 = 2.0, c24 = 1.6666666666666666667e-1, c32 = 4.1666666666666666667e-2,
                   c40 = 8.3333333333333333333e-3, c48 = 1.3888888888888888889e-3, c56 = 1.9841269841269841270e-4,
                   c64 = 2.4801587301587301587e-5;
  uint64_t bx = Of(x);
  if (int64_t(kPosInf) <= int64_t(bx & ~kSign)) {
    if (bx == kNegInf) return 0;
    return x;  // NaN or +Inf
  }
  if (x > kOverflow) return Inf(1);
  int32_t e = CvtSD2SL(kLog2e * x);
  double k = double(e);
  double r, p;
  if (fma) {
    r = std::fma(-kLn2U, k, x);
    r = std::fma(-kLn2L, k, r);
    r *= 0.0625;
    p = c64;
    p = std::fma(r, p, c56);
    p = std::fma(r, p, c48);
    p = std::fma(r, p, c40);
    p = std::fma(r, p, c32);
    p = std::fma(r, p, c24);
    p = std::fma(r, p, c0);
    p = std::fma(r, p, c8);
    r = r * p;
    p = r + c16;
    r = r * p;
    p = r + c16;
    r = r * p;
    p = r + c16;
    r = r * p;
    p = r + c16;
    r = std::fma(p, r, c8);
  } else {
    r = x - kLn2U * k;
    r = r - kLn2L * k;
    r *= 0.0625;
    p = c64 * r;
    p = (p + c56) * r;
    p = (p + c48) * r;
    p = (p + c40) * r;
    p = (p + c32) * r;
    p = (p + c24) * r;
    p = (p + c0) * r;
    p = p + c8;
    r = r * p;
    for (int i = 0; i < 4; i++) {
      p = c16 + r;
      r = r * p;
    }
    r = r + c8;
  }
  // Multiply by 2**e, through a denormal scale when needed.
  int32_t b = e + 0x3FF;
  if (b <= 0) {
    if (b < -52) return 0;
    b += 0x3FE;
    r *= Bits(uint64_t(uint32_t(b)) << 52);
    b = 1;
  } else if (b >= 0x7FF) {
    return Inf(1);
  }
  return r * Bits(uint64_t(uint32_t(b)) << 52);
}

inline double Exp(double x) { return Exp(x, UseFMA()); }

// Log is math.archLog on amd64.
inline double Log(double x) {
  constexpr double kHSqrt2 = 7.07106781186547524401e-01;
  constexpr double kLn2Hi = 6.93147180369123816490e-01;
  constexpr double kLn2Lo = 1.90821492927058770002e-10;
  constexpr double L1 = 6.666666666666735130e-01, L2 = 3.999999999940941908e-01, L3 = 2.857142874366239149e-01,
                   L4 = 2.222219843214978396e-01, L5 = 1.818357216161805012e-01, L6 = 1.531383769920937332e-01,
                   L7 = 1.479819860511658591e-01;
  uint64_t bx = Of(x);
  if ((bx & ~kSign) == 0) return Inf(-1);
  if (int64_t(bx) < 0) return NaN();
  if (int64_t(kPosInf) <= int64_t(bx)) return x;  // +Inf or NaN
  double f1 = Bits((bx & 0x000FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
  double k = double(int32_t((bx >> 52) & 0x7FF) - 0x3FE);
  // CMPSD's not-less-than: f1 <= Sqrt2/2 takes k-1 and 2*f1.
  double one = kHSqrt2 < f1 ? 0.0 : 1.0;
  k = k - one;
  f1 = f1 * (one + 1.0);
  double f = f1 - 1.0;
  double s = f / (2.0 + f);
  double s2 = s * s;
  double s4 = s2 * s2;
  double t1 = s2 * (((L7 * s4 + L5) * s4 + L3) * s4 + L1);
  double t2 = s4 * ((L6 * s4 + L4) * s4 + L2);
  double R = t1 + t2;
  double hfsq = 0.5 * f * f;
  double a = s * (R + hfsq) + kLn2Lo * k;
  return k * kLn2Hi - ((hfsq - a) - f);
}

// Modf is math.Modf.
inline void Modf(double f, double *integer, double *frac) {
  *integer = std::trunc(f);
  *frac = std::copysign(f - *integer, f);
}

// Normalize is math.normalize.
inline double Normalize(double x, int *exp) {
  constexpr double kSmallestNormal = 2.2250738585072014e-308;
  if (std::fabs(x) < kSmallestNormal) {
    *exp = -52;
    return x * double(uint64_t(1) << 52);
  }
  *exp = 0;
  return x;
}

// Frexp is math.Frexp.
inline double Frexp(double f, int *exp) {
  *exp = 0;
  if (f == 0 || std::isinf(f) || std::isnan(f)) return f;
  int e;
  f = Normalize(f, &e);
  uint64_t x = Of(f);
  *exp = e + int((x >> 52) & 0x7FF) - 1023 + 1;
  x &= ~(uint64_t(0x7FF) << 52);
  x |= uint64_t(-1 + 1023) << 52;
  return Bits(x);
}

// Ldexp is math.Ldexp.
inline double Ldexp(double frac, int exp) {
  if (frac == 0 || std::isinf(frac) || std::isnan(frac)) return frac;
  int e;
  frac = Normalize(frac, &e);
  exp += e;
  uint64_t x = Of(frac);
  exp += int((x >> 52) & 0x7FF) - 1023;
  if (exp < -1075) return std::copysign(0.0, frac);
  if (exp > 1023) return frac < 0 ? Inf(-1) : Inf(1);
  double m = 1;
  if (exp < -1022) {
    exp += 53;
    m = 1.0 / double(uint64_t(1) << 53);
  }
  x &= ~(uint64_t(0x7FF) << 52);
  x |= uint64_t(exp + 1023) << 52;
  return m * Bits(x);
}

inline bool IsOddInt(double x) {
  if (std::fabs(x) >= double(uint64_t(1) << 53)) return false;
  double xi, xf;
  Modf(x, &xi, &xf);
  return xf == 0 && (int64_t(xi) & 1) == 1;
}

}  // namespace gomath

// GoPow is Go's math.Pow on amd64.
inline double GoPow(double x, double y) {
  using namespace gomath;
  if (y == 0 || x == 1) return 1;
  if (y == 1) return x;
  if (std::isnan(x) || std::isnan(y)) return NaN();
  if (x == 0) {
    if (y < 0) return std::signbit(x) && IsOddInt(y) ? Inf(-1) : Inf(1);
    if (y > 0) return std::signbit(x) && IsOddInt(y) ? x : 0;
  } else if (std::isinf(y)) {
    if (x == -1) return 1;
    if ((std::fabs(x) < 1) == (y > 0)) return 0;
    return Inf(1);
  } else if (std::isinf(x)) {
    if (x < 0) return GoPow(1 / x, -y);  // Pow(-0, -y)
    if (y < 0) return 0;
    if (y > 0) return Inf(1);
  } else if (y == 0.5) {
    return std::sqrt(x);
  } else if (y == -0.5) {
    return 1 / std::sqrt(x);
  }

  double yi, yf;
  Modf(std::fabs(y), &yi, &yf);
  if (yf != 0 && x < 0) return NaN();
  if (yi >= 9223372036854775808.0) {
    // yi is a large even int that overflows or underflows for every x
    // but -1.
    if (x == -1) return 1;
    if ((std::fabs(x) < 1) == (y > 0)) return 0;
    return Inf(1);
  }

  // ans = a1 * 2**ae (= 1 for now).
  double a1 = 1.0;
  int ae = 0;

  // ans *= x**yf
  if (yf != 0) {
    if (yf > 0.5) {
      yf--;
      yi++;
    }
    a1 = Exp(yf * Log(x));
  }

  // ans *= x**yi, by multiplying in successive squarings of x according
  // to the bits of yi, accumulating powers of two into ae.
  int xe;
  double x1 = Frexp(x, &xe);
  for (int64_t i = int64_t(yi); i != 0; i >>= 1) {
    if (xe < -(1 << 12) || (1 << 12) < xe) {
      // The final Ldexp under- or overflows; stop before xe does.
      ae += xe;
      break;
    }
    if ((i & 1) == 1) {
      a1 *= x1;
      ae += xe;
    }
    x1 *= x1;
    xe <<= 1;
    if (x1 < .5) {
      x1 += x1;
      xe--;
    }
  }

  // ans = a1*2**ae; if y < 0, ans = 1/ans, in the opposite order.
  if (y < 0) {
    a1 = 1 / a1;
    ae = -ae;
  }
  return Ldexp(a1, ae);
}

// GoFloatToUint64 is Go's uint64(f) conversion on amd64: CVTTSD2SQ below
// 2^63, and above it CVTTSD2SQ(f - 2^63) with the top bit set. NaN and
// out-of-range values give what that instruction sequence gives.
inline uint64_t GoFloatToUint64(double f) {
  constexpr double kCutoff = 9223372036854775808.0;
  auto cvt = [](double x) -> uint64_t {
    if (!(x >= -kCutoff && x < kCutoff)) return uint64_t(1) << 63;
    return uint64_t(int64_t(x));
  };
  if (f < kCutoff) return cvt(f);
  return cvt(f - kCutoff) | (uint64_t(1) << 63);
}

}  // namespace celltnx
