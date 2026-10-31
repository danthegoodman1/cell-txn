// The synthetic Users workload and its Zipf generator.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "workload.h"

namespace celltnx::workload {

// Zipf draws ranks in [0, n) with skew theta in [0, 1), following Gray et
// al., "Quickly Generating Billion-Record Synthetic Databases". It uses
// detmath::Pow, so a seed draws the same ranks on every platform.
class Zipf {
 public:
  Zipf(uint64_t n, double theta);
  uint64_t Next(Rand &r) const;

 private:
  uint64_t n_;
  double theta_, alpha_ = 0, zetan_ = 0, eta_ = 0;
};

inline Zipf NewZipf(uint64_t n, double theta) { return Zipf(n, theta); }

// Users is the synthetic workload: hot user rows picked by Zipf(Theta),
// with transaction types that touch disjoint or shared columns.
class Users : public Workload {
 public:
  // Mix weights: pay (wallet -= x as a delta), deposit (wallet += x as a
  // delta), profile (set email), audit (read email, insert an audit row),
  // same (read wallet, set wallet), read (read-only lookup of two users).
  static constexpr int kMix = 6;

  Users(uint64_t rows, double theta, std::array<int, kMix> mix, double think);

  uint64_t Rows;
  double Theta;
  std::array<int, kMix> Mix;
  double Think;

  std::string Name() override { return "users"; }
  std::vector<Schema> Schemas() override;
  Status Load(Client &c) override;
  Status Step(Client &c) override;
  // Detail is String().
  std::string Detail() override { return String(); }
  std::string String() const;

 private:
  int Pick(Rand &r) const;
  void ThinkFor(Client &c) const;

  Zipf zipf_;
};

// NewUsers returns the workload with the given rows, skew and mix.
std::unique_ptr<Users> NewUsers(uint64_t rows, double theta, std::array<int, Users::kMix> mix, double think);
// NewUsersFrom draws a configuration from r.
std::unique_ptr<Workload> NewUsersFrom(Rand &r);

}  // namespace celltnx::workload
