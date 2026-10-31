// A PCG generator with 128-bit state and DXSM output, and the bounded
// draws the workloads and the simulator use. Every draw is defined bit for
// bit, so a seed replays the same values on every platform.
#pragma once

#include <cstdint>
#include <stdexcept>

namespace celltnx {

class Rand {
 public:
  // Rand(seed1, seed2) starts from the 128-bit state seed1:seed2.
  Rand(uint64_t seed1, uint64_t seed2) : hi_(seed1), lo_(seed2) {}

  uint64_t Uint64() {
    constexpr uint64_t kMulHi = 2549297995355413924ull, kMulLo = 4865540595714422341ull;
    constexpr uint64_t kIncHi = 6364136223846793005ull, kIncLo = 1442695040888963407ull;
    // state = state * mul + inc, in 128 bits.
    unsigned __int128 m = (unsigned __int128)lo_ * kMulLo;
    uint64_t hi = uint64_t(m >> 64) + hi_ * kMulLo + lo_ * kMulHi;
    uint64_t lo = uint64_t(m);
    unsigned __int128 s = (unsigned __int128)lo + kIncLo;
    lo = uint64_t(s);
    hi += kIncHi + uint64_t(s >> 64);
    lo_ = lo;
    hi_ = hi;
    // DXSM output.
    constexpr uint64_t kCheapMul = 0xda942042e4dd58b5ull;
    hi ^= hi >> 32;
    hi *= kCheapMul;
    hi ^= hi >> 48;
    hi *= (lo | 1);
    return hi;
  }

  uint64_t Uint64N(uint64_t n) {
    if (n == 0) throw std::invalid_argument("invalid argument to Uint64N");
    return Uint64n(n);
  }
  int64_t Int64N(int64_t n) {
    if (n <= 0) throw std::invalid_argument("invalid argument to Int64N");
    return int64_t(Uint64n(uint64_t(n)));
  }
  // IntN draws from [0, n), as Int64N does.
  int64_t IntN(int64_t n) {
    if (n <= 0) throw std::invalid_argument("invalid argument to IntN");
    return int64_t(Uint64n(uint64_t(n)));
  }
  double Float64() { return double(Uint64() << 11 >> 11) / double(uint64_t(1) << 53); }

 private:
  uint64_t Uint64n(uint64_t n) {
    if ((n & (n - 1)) == 0) return Uint64() & (n - 1);
    unsigned __int128 p = (unsigned __int128)Uint64() * n;
    uint64_t hi = uint64_t(p >> 64), lo = uint64_t(p);
    if (lo < n) {
      uint64_t thresh = (0 - n) % n;
      while (lo < thresh) {
        p = (unsigned __int128)Uint64() * n;
        hi = uint64_t(p >> 64);
        lo = uint64_t(p);
      }
    }
    return hi;
  }

  uint64_t hi_, lo_;
};

}  // namespace celltnx
