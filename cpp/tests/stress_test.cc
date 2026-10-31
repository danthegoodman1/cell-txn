// Every workload on real threads in every mode, with the whole history
// checked: a port of workload/stress_test.go. Run it under the tsan and asan
// presets.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "random.h"
#include "replay.h"
#include "tpcc.h"
#include "users.h"

namespace celltnx::workload {
namespace {

// RealEnv runs clients on real threads; ticks are microseconds.
class RealEnv : public Env {
 public:
  void Yield() override { std::this_thread::yield(); }
  void Sleep(int64_t ticks) override { std::this_thread::sleep_for(std::chrono::microseconds(ticks)); }
  int64_t Now() override {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count();
  }
  bool Stopped() override { return false; }

 private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

void Stress(Workload &w, Mode mode, uint64_t seed) {
  DB db(Options{.mode = mode, .bucket_bits = unsigned(seed % 4)}, w.Schemas());
  check::Recorder rec;
  RealEnv env;
  Client loader{.DB = &db, .Rec = &rec, .Rand = Rand(seed, 2), .Env = &env};
  Status st = w.Load(loader);
  ASSERT_TRUE(st.ok()) << st.Message();
  std::vector<Client> clients;
  for (int i = 0; i < 8; i++)
    clients.push_back(
        Client{.ID = i + 1, .DB = &db, .Rec = &rec, .Rand = Rand(seed, uint64_t(i + 3)), .Env = &env, .Quota = 150});
  std::vector<Status> errs(clients.size());
  std::vector<std::thread> threads;
  for (size_t i = 0; i < clients.size(); i++) threads.emplace_back([&, i] { errs[i] = Run(w, clients[i]); });
  for (std::thread &t : threads) t.join();
  for (const Status &err : errs) EXPECT_TRUE(err.ok()) << err.Message();
  st = db.CheckInvariants();
  ASSERT_TRUE(st.ok()) << st.Message();
  st = check::Replay(w.Schemas(), rec.Logs(), db.LastCommitTs(), [&](int t) { return db.Dump(t); });
  ASSERT_TRUE(st.ok()) << st.Message();
}

using Param = std::tuple<uint64_t, Mode, int>;

class StressTest : public testing::TestWithParam<Param> {};

// Each seed draws a Random, a Users and a TPCC workload from one generator,
// in that order, for every mode.
TEST_P(StressTest, Run) {
  auto [seed, mode, which] = GetParam();
  Rand r(seed, 1);
  std::unique_ptr<Workload> ws[] = {NewRandom(r), NewUsersFrom(r), NewTPCCFrom(r)};
  SCOPED_TRACE(ws[which]->Detail());
  Stress(*ws[which], mode, seed);
}

std::string Name(const testing::TestParamInfo<Param> &info) {
  static const char *const names[] = {"random", "users", "tpcc"};
  std::string mode = ModeName(std::get<1>(info.param));
  for (char &c : mode)
    if (c == '+') c = '_';
  return mode + "_" + names[std::get<2>(info.param)] + "_" + std::to_string(std::get<0>(info.param));
}

INSTANTIATE_TEST_SUITE_P(All, StressTest,
                         testing::Combine(testing::Values(uint64_t{0}, uint64_t{1}, uint64_t{2}),
                                          testing::Values(Mode::kRow, Mode::kCell, Mode::kCellDelta),
                                          testing::Values(0, 1, 2)),
                         Name);

}  // namespace
}  // namespace celltnx::workload
