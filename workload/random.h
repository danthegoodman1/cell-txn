// Random issues random transactions over a small key space using every
// API call.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "workload.h"

namespace celltnx::workload {

// Random issues random transactions over a small key space using every API
// call. Column 0 of table 0 holds small values under CHECK >= 0 when Check
// is set. Table 0 may index column 0 (non-unique) and column 1 (unique,
// drawn from a small domain so claims collide). Every other written value
// is unique.
class Random : public Workload {
 public:
  // Weights index: get, untracked get, scan, index scan, set, add, insert,
  // delete.
  static constexpr int kOps = 8;

  std::vector<int> Widths;  // columns per table
  uint64_t Keys = 0;        // keys per table
  bool Check = false;
  bool Index0 = false;   // non-unique index on table 0 column 0
  bool Unique1 = false;  // unique index on table 0 column 1
  int MaxOps = 0;
  std::array<int, kOps> Weights{};
  double ReadOnly = 0;   // share of long read-only transactions
  double Abandon = 0;    // per-operation chance of abandoning the transaction
  double Save = 0;       // per-operation chance of a savepoint around it
  double Think = 0;      // per-operation chance of a pause
  double Unbounded = 0;  // share of scans with no upper bound

  std::string Name() override { return "random"; }
  std::vector<Schema> Schemas() override;
  Status Load(Client &c) override;
  Status Step(Client &c) override;
  std::string Detail() override;
};

// NewRandom draws a configuration from r.
std::unique_ptr<Workload> NewRandom(Rand &r);

}  // namespace celltnx::workload
