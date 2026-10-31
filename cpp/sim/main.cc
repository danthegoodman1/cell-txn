// Command sim runs deterministic simulations of the C++ core: one seed
// verbosely, or a sweep of seeds in parallel with determinism checks. Its
// flags and output follow the Go cmd/sim.
//
//   sim -seed 42                 replay one seed
//   sim -seeds 30000             sweep seeds [start, start+30000)
//   sim -bug gc-eager -seeds N   self-test (needs the simbugs preset)
//   sim -seeds 10000 -golden F   write each seed's outcome to F, as Go's -golden does
//   sim -seeds N -refresh -1     also refresh snapshots before operations, at a drawn rate
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bugs.h"
#include "sim.h"

using namespace celltnx;

namespace {

struct Flags {
  int64_t seed = -1;
  int64_t seeds = 1000;
  uint64_t start = 0;
  int workers = int(std::thread::hardware_concurrency());
  int det = 10;
  std::string bug, golden;
  sim::Config cfg;
};

[[noreturn]] void Usage(const char *bad) {
  fprintf(stderr, "sim: bad flag %s\n", bad);
  exit(2);
}

Flags Parse(int argc, char **argv) {
  Flags f;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    while (!a.empty() && a[0] == '-') a.erase(0, 1);
    std::string v;
    if (size_t eq = a.find('='); eq != std::string::npos) {
      v = a.substr(eq + 1);
      a = a.substr(0, eq);
    } else if (i + 1 < argc) {
      v = argv[++i];
    } else {
      Usage(argv[i]);
    }
    if (a == "seed") f.seed = std::stoll(v);
    else if (a == "seeds") f.seeds = std::stoll(v);
    else if (a == "start") f.start = std::stoull(v);
    else if (a == "workers") f.workers = std::stoi(v);
    else if (a == "workload") f.cfg.workload = v;
    else if (a == "mode") f.cfg.mode = std::stoi(v);
    else if (a == "durable") f.cfg.durable = std::stoi(v);
    else if (a == "commitdelay") f.cfg.commit_delay = std::stoll(v);
    else if (a == "interleave") f.cfg.interleave = std::stod(v);
    else if (a == "refresh") f.cfg.refresh = std::stod(v);
    else if (a == "bug") f.bug = v;
    else if (a == "det") f.det = std::stoi(v);
    else if (a == "golden") f.golden = v;
    else Usage(argv[i]);
  }
  return f;
}

std::string Hex(uint64_t x) {
  char buf[24];
  snprintf(buf, sizeof buf, "%#" PRIx64, x);
  return x == 0 ? "0x0" : buf;
}

std::string FirstLines(const std::string &s, int n) {
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] == '\n' && --n == 0) return s.substr(0, i);
  return s;
}

}  // namespace

int main(int argc, char **argv) {
  Flags f = Parse(argc, argv);
  if (!f.bug.empty()) {
    if (!SetBug(f.bug)) {
      fprintf(stderr, "sim: unknown bug %s, or a build without CELLTNX_SIMBUGS\n", f.bug.c_str());
      return 2;
    }
    f.cfg.no_invariants = true;
  }

  if (f.seed >= 0) {
    sim::Result r = sim::Run(uint64_t(f.seed), f.cfg);
    const workload::Stats &st = r.stats;
    printf("seed %" PRIu64 ": %s\nsteps=%" PRId64 " crashes=%d logs=%d {Commits:%" PRId64 " Rejected:%" PRId64
           " UserAborts:%" PRId64 " Conflicts:%" PRId64 " RowConf:%" PRId64 " BucketConf:%" PRId64 "} trace=%s\n",
           r.seed, r.params.String().c_str(), r.steps, r.crashes, r.logs, st.Commits, st.Rejected, st.UserAborts,
           st.Conflicts, st.RowConf, st.BucketConf, Hex(r.trace).c_str());
    if (!r.err.empty()) {
      printf("FAIL: %s\n", r.err.c_str());
      return 1;
    }
    printf("ok\n");
    return 0;
  }

  std::mutex mu;
  std::atomic<int64_t> next{0};
  int64_t ran = 0, steps = 0, crashes = 0;
  workload::Stats total;
  std::vector<sim::Result> failures, results;
  std::vector<uint64_t> nondet;
  std::map<std::string, int> cover;
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (int k = 0; k < f.workers; k++)
    threads.emplace_back([&] {
      for (;;) {
        int64_t i = next++;
        if (i >= f.seeds) return;
        uint64_t s = f.start + uint64_t(i);
        sim::Result r = sim::Run(s, f.cfg);
        bool deterministic = true;
        if (f.det > 0 && s % uint64_t(f.det) == 0) {
          sim::Result r2 = sim::Run(s, f.cfg);
          deterministic = r2.trace == r.trace && r2.steps == r.steps && r2.err == r.err;
        }
        std::lock_guard g(mu);
        ran++;
        steps += r.steps;
        crashes += r.crashes;
        total.Add(r.stats);
        char key[96];
        snprintf(key, sizeof key, "%-10s %-6s %s", ModeName(r.params.mode), r.params.workload.c_str(),
                 r.params.durable ? "durable" : "memory");
        cover[key]++;
        if (!r.err.empty()) failures.push_back(r);
        if (!f.golden.empty()) results.push_back(r);
        if (!deterministic) nondet.push_back(s);
      }
    });
  for (auto &t : threads) t.join();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  printf("seeds: %" PRId64 " in %.3fs (%d workers)\n", ran, secs, f.workers);
  printf("steps: %" PRId64 "  commits: %" PRId64 "  rejected: %" PRId64 "  user aborts: %" PRId64 "  conflicts: %" PRId64
         " (row %" PRId64 ", bucket %" PRId64 ")  crashes: %" PRId64 "\n",
         steps, total.Commits, total.Rejected, total.UserAborts, total.Conflicts, total.RowConf, total.BucketConf,
         crashes);
  for (auto &[k, n] : cover) printf("  %-29s %6d seeds\n", k.c_str(), n);
  if (!f.golden.empty()) {
    std::sort(results.begin(), results.end(), [](auto &a, auto &b) { return a.seed < b.seed; });
    FILE *out = fopen(f.golden.c_str(), "w");
    if (out == nullptr) {
      perror(f.golden.c_str());
      return 1;
    }
    for (auto &r : results)
      fprintf(out, "%" PRIu64 " %s %" PRId64 " %d %s %s\n", r.seed, Hex(r.trace).c_str(), r.steps, r.crashes,
              r.err.empty() ? "false" : "true", r.params.String().c_str());
    fclose(out);
  }
  if (f.det > 0) {
    std::sort(nondet.begin(), nondet.end());
    printf("determinism: %" PRId64 " seeds rerun, %zu mismatches", (ran + f.det - 1) / f.det, nondet.size());
    for (uint64_t s : nondet) printf(" %" PRIu64, s);
    printf("\n");
  }
  std::sort(failures.begin(), failures.end(), [](auto &a, auto &b) { return a.seed < b.seed; });
  printf("failures: %zu\n", failures.size());
  for (size_t i = 0; i < failures.size(); i++) {
    if (i == 5) {
      printf("  ... %zu more\n", failures.size() - 5);
      break;
    }
    printf("  seed %" PRIu64 ": %s\n    %s\n", failures[i].seed, failures[i].params.String().c_str(),
           FirstLines(failures[i].err, 6).c_str());
  }
  if (!f.bug.empty()) {
    if (failures.empty()) {
      printf("SELF-TEST FAILED: bug %s not found\n", f.bug.c_str());
      return 1;
    }
    printf("bug %s found: first at seed %" PRIu64 "\n", f.bug.c_str(), failures[0].seed);
    return 0;
  }
  return failures.empty() && nondet.empty() ? 0 : 1;
}
