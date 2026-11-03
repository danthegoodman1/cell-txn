// RowLocks: ownership, timeouts, kills, if_used, every waiter of a long
// queue getting the key without waiting out a poll, and a threaded stress
// test checking that a key has one holder. Run it under the tsan and asan
// presets.
#include "row_locks.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace celltnx::engine {
namespace {

using namespace std::chrono_literals;
using Got = RowLocks::Got;

const std::function<bool()> kAlive = [] { return false; };

// Owners are distinct addresses.
char owners[64];
const void *O(int i) { return &owners[i]; }

// WaitFor polls until key has n waiters, failing after 5 s.
void WaitFor(RowLocks &l, const std::string &key, size_t n) {
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (l.Waiters(key) != n) {
    if (std::chrono::steady_clock::now() > deadline) {
      ADD_FAILURE() << key << " has " << l.Waiters(key) << " waiters, not " << n;
      return;
    }
    std::this_thread::sleep_for(100us);
  }
}

TEST(RowLocks, FreeHeldBusy) {
  RowLocks l;
  EXPECT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive), Got::kFree);
  EXPECT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive), Got::kHeld);
  EXPECT_EQ(l.Acquire("k", O(1), Col(1), false, 1s, kAlive), Got::kBusy);
  EXPECT_EQ(l.Acquire("k", O(1), Col(1), true, 0ms, kAlive), Got::kBusy);
  EXPECT_EQ(l.Acquire("j", O(1), Col(1), true, 1s, kAlive), Got::kFree);
  l.Release("k");
  l.Release("j");
  EXPECT_EQ(l.Size(), 0u);
  EXPECT_EQ(l.Acquire("k", O(1), Col(1), false, 1s, kAlive), Got::kFree);
  l.Release("k");
  EXPECT_EQ(l.Size(), 0u);
}

TEST(RowLocks, TimesOut) {
  RowLocks l;
  ASSERT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive), Got::kFree);
  auto t = std::chrono::steady_clock::now();
  EXPECT_EQ(l.Acquire("k", O(1), Col(1), true, 30ms, kAlive), Got::kBusy);
  EXPECT_GE(std::chrono::steady_clock::now() - t, 30ms);
  EXPECT_EQ(l.Waiters("k"), 0u);
  // The holder still holds it, and its release frees the key.
  EXPECT_EQ(l.Acquire("k", O(2), Col(1), false, 1s, kAlive), Got::kBusy);
  l.Release("k");
  EXPECT_EQ(l.Size(), 0u);
}

TEST(RowLocks, KillEndsWait) {
  RowLocks l;
  ASSERT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive), Got::kFree);
  std::atomic<bool> killed{false};
  std::thread th([&] { EXPECT_EQ(l.Acquire("k", O(1), Col(1), true, 5s, [&] { return killed.load(); }), Got::kBusy); });
  WaitFor(l, "k", 1);
  killed = true;
  th.join();  // within the 100 ms poll, not the 5 s timeout
  EXPECT_EQ(l.Waiters("k"), 0u);
  l.Release("k");
  EXPECT_EQ(l.Size(), 0u);
}

// More waiters than a release wakes all get the key in turn, each woken by
// the release before it rather than by its 100 ms poll.
TEST(RowLocks, EveryWaiterGetsTheKey) {
  RowLocks l;
  constexpr int kWaiters = 3 * RowLocks::kWake;
  ASSERT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive), Got::kFree);
  std::atomic<int> holders{0};
  std::mutex mu;
  std::vector<std::chrono::steady_clock::time_point> times;
  std::vector<std::thread> ths;
  for (int i = 1; i <= kWaiters; i++) {
    ths.emplace_back([&, i] {
      EXPECT_EQ(l.Acquire("k", O(i), Col(1), true, 5s, kAlive), Got::kWaited);
      EXPECT_EQ(holders.fetch_add(1), 0);
      {
        std::lock_guard g(mu);
        times.push_back(std::chrono::steady_clock::now());
      }
      std::this_thread::sleep_for(100us);
      holders--;
      l.Release("k");
    });
  }
  WaitFor(l, "k", kWaiters);
  times.push_back(std::chrono::steady_clock::now());
  l.Release("k");
  for (auto &th : ths) th.join();
  ASSERT_EQ(times.size(), size_t(kWaiters + 1));
  std::chrono::steady_clock::duration gap{};
  for (size_t i = 1; i < times.size(); i++) gap = std::max(gap, times[i] - times[i - 1]);
  EXPECT_LT(gap, 20ms);
  EXPECT_EQ(l.Size(), 0u);
}

TEST(RowLocks, IfUsed) {
  RowLocks l;
  // Nobody holds the key: nothing to take, and no entry is left behind.
  EXPECT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive, true), Got::kUnused);
  EXPECT_EQ(l.Size(), 0u);
  ASSERT_EQ(l.Acquire("k", O(0), Col(1), true, 1s, kAlive), Got::kFree);
  EXPECT_EQ(l.Acquire("k", O(1), Col(2), true, 1s, kAlive, true), Got::kUnused);
  EXPECT_EQ(l.Acquire("k", O(1), Col(1), false, 1s, kAlive, true), Got::kBusy);
  // A waiter's columns count too: once one awaits Col(2), Col(2) is used.
  std::thread th([&] {
    EXPECT_EQ(l.Acquire("k", O(1), Col(2), true, 5s, kAlive), Got::kWaited);
    l.Release("k");
  });
  WaitFor(l, "k", 1);
  EXPECT_EQ(l.Acquire("k", O(2), Col(2), false, 1s, kAlive, true), Got::kBusy);
  l.Release("k");
  th.join();
  EXPECT_EQ(l.Size(), 0u);
  // The columns reset once the key falls idle.
  ASSERT_EQ(l.Acquire("k", O(0), Col(3), true, 1s, kAlive), Got::kFree);
  EXPECT_EQ(l.Acquire("k", O(1), Col(1), true, 1s, kAlive, true), Got::kUnused);
  l.Release("k");
  EXPECT_EQ(l.Size(), 0u);
}

// Stress: threads take a few hot keys with short timeouts, so wake-ups race
// with timeouts and newcomers. Each holder checks it holds its key alone.
TEST(RowLocks, Stress) {
  RowLocks l;
  constexpr int kThreads = 32, kKeys = 3, kOps = 2000;
  std::atomic<const void *> holder[kKeys] = {};
  std::atomic<int> taken{0}, busy{0};
  std::vector<std::thread> ths;
  for (int t = 0; t < kThreads; t++) {
    ths.emplace_back([&, t] {
      std::mt19937 r(t);
      for (int i = 0; i < kOps; i++) {
        int k = int(r() % kKeys);
        std::string key = "k" + std::to_string(k);
        Got got = l.Acquire(key, O(t), Col(int(r() % 2)), true, std::chrono::milliseconds(1 + r() % 3), kAlive,
                            r() % 4 == 0);
        if (got == Got::kBusy) {
          busy++;
          continue;
        }
        if (got == Got::kUnused) continue;
        ASSERT_TRUE(got == Got::kFree || got == Got::kWaited);
        const void *none = nullptr;
        ASSERT_TRUE(holder[k].compare_exchange_strong(none, O(t))) << "two holders of " << key;
        taken++;
        if (r() % 2) std::this_thread::sleep_for(std::chrono::microseconds(r() % 200));
        holder[k] = nullptr;
        l.Release(key);
      }
    });
  }
  for (auto &th : ths) th.join();
  EXPECT_GT(taken.load(), 0);
  EXPECT_GT(busy.load(), 0);  // some waits timed out, racing their wake-ups
  EXPECT_EQ(l.Size(), 0u);
}

}  // namespace
}  // namespace celltnx::engine
