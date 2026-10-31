// TPC-C NewOrder and Payment, scaled down, plus a consistency audit.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "workload.h"

namespace celltnx::workload {

// TPCC runs TPC-C NewOrder and Payment, scaled down, plus a read-only audit
// that verifies consistency conditions 1–4. Payment's YTD, balance and
// count updates and NewOrder's stock counters are Adds, so the mode alone
// decides whether they commute. Like TPC-C, 60% of Payments find the
// customer by last name through a secondary index.
class TPCC : public Workload {
 public:
  TPCC(uint64_t warehouses, uint64_t districts, uint64_t customers, uint64_t items, int max_lines);

  uint64_t Warehouses;
  uint64_t Districts;  // per warehouse, at most 255
  uint64_t Customers;  // per district
  uint64_t Items;
  int MaxLines;  // order lines per NewOrder, at most 15
  // Mix weights for NewOrder, Payment and the audit.
  std::array<int, 3> Mix{45, 43, 4};
  double Think = 0;

  std::string Name() override { return "tpcc"; }
  std::vector<Schema> Schemas() override;
  // Load inserts the initial data in transactions of at most 1000 rows.
  // Consistency condition 1 holds after every one of them.
  Status Load(Client &c) override;
  Status Step(Client &c) override;
  std::string Detail() override;

 private:
  uint64_t LastNames() const;
  void ThinkFor(Client &c) const;
  uint64_t OtherWarehouse(Rand &r, uint64_t wh) const;
  Status NewOrder(Client &c);
  Status Payment(Client &c);
  Status Audit(Client &c);
};

// NewTPCC returns the standard 45:43 NewOrder:Payment mix with a small
// share of audits.
std::unique_ptr<TPCC> NewTPCC(uint64_t warehouses, uint64_t districts, uint64_t customers, uint64_t items,
                              int max_lines);
// NewTPCCFrom draws a small, contended configuration from r.
std::unique_ptr<Workload> NewTPCCFrom(Rand &r);

}  // namespace celltnx::workload
