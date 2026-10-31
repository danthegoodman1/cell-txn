// The deterministic simulator: runs the core and a workload on one thread
// from a seed, with clients as coroutines, a simulated disk, crashes and
// recovery, then replays the history through the checker. A port of the Go
// package sim (without the SQL simulation); a seed must produce the same
// trace and step count in both.
#pragma once

#include <cstdint>
#include <string>

#include "db.h"
#include "workload.h"

namespace celltnx::sim {

// Config fixes parts of the drawn parameters.
struct Config {
  std::string workload;       // "random", "users" or "tpcc"; empty draws one
  int mode = -1;              // a Mode; negative draws one
  int durable = -1;           // 1 forces a store, 0 forbids one, negative draws
  int64_t commit_delay = -1;  // the writer's delay in steps; negative draws one
  double interleave = -1;     // switch probability at interleave points; negative draws one
  // Chance of a snapshot refresh before each transaction operation, which
  // the Go simulator lacks; negative draws one. Zero keeps every seed
  // identical to Go's.
  double refresh = 0;
  bool no_invariants = false; // skip internal invariant checks
  int64_t max_steps = 0;      // liveness bound; zero uses a default
};

// Params are a seed's drawn parameters.
struct Params {
  Mode mode = Mode::kRow;
  std::string workload;
  int clients = 0;
  int quota = 0;
  unsigned bucket_bits = 0;
  bool skew = false;          // clients get unequal scheduling weights
  bool durable = false;
  bool no_sync = false;       // acknowledge commits before they sync
  double crash_p = 0;         // per-step crash chance
  double sync_fail_p = 0;     // per-sync failure chance; a failure crashes
  int64_t latency = 0;        // max sync time in steps
  int64_t commit_delay = 0;   // the writer's wait before each flush, in steps
  double interleave = 0;      // chance of switching at each interleave point
  double refresh = 0;         // chance of a snapshot refresh before each operation
  std::string detail;         // the workload's configuration

  // String formats the parameters as the Go simulator does.
  std::string String() const;
};

// Result is the outcome of one seed. An empty err means it passed.
struct Result {
  uint64_t seed = 0;
  Params params;
  int64_t steps = 0;
  int crashes = 0;
  workload::Stats stats;
  int logs = 0;
  uint64_t trace = 0;  // hash of every outcome, for determinism checks
  std::string err;
};

Result Run(uint64_t seed, const Config &cfg);

}  // namespace celltnx::sim
