// RowLocks queues transactions that read a row with SELECT ... FOR UPDATE,
// so a hot read-modify-write waits for the previous one to commit instead
// of failing validation and rerunning. The locks are advisory: commit-time
// validation alone decides serializability, so a wait that times out costs
// at most a conflict. Only a transaction holding no lock waits, so no wait
// can close a cycle.
//
// Waiters queue in arrival order, each on its own condition variable, and
// a release wakes the first kWake of them. Whichever runs first takes the
// key, unless a running newcomer took it already. Waking one waiter leaves
// the key idle until that waiter runs; waking all of them sends dozens of
// threads after one mutex at a hot row. Before it sleeps, a waiter checks
// the key kSpin more times, yielding between checks: most holds of a hot
// row last one statement, and a check costs less than a sleep and a wake-up.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "value.h"

namespace celltnx::engine {

class RowLocks {
 public:
  enum class Got { kHeld, kFree, kWaited, kBusy, kUnused };

  static constexpr int kWake = 8;
  static constexpr int kSpin = 100;

  // Acquire takes key for owner, which uses cols of the row, or reports
  // kHeld if owner has it. When another owner holds the key it waits, if
  // wait is set, until it takes the key, the timeout passes, or killed
  // returns true; it checks killed at least every 100 ms. With if_used, it
  // takes nothing and reports kUnused unless the key's holder or waiters
  // use one of cols.
  Got Acquire(const std::string &key, const void *owner, ColMask cols, bool wait,
              std::chrono::milliseconds timeout, const std::function<bool()> &killed, bool if_used = false) {
    Shard &sh = ShardOf(key);
    std::unique_lock g(sh.mu);
    if (if_used) {
      auto it = sh.map.find(key);
      if (it == sh.map.end() || (it->second.cols & cols) == 0) return Got::kUnused;
    }
    Entry &e = sh.map[key];
    e.cols |= cols;
    if (e.owner == owner) return Got::kHeld;
    if (e.owner == nullptr) {
      e.owner = owner;
      return Got::kFree;
    }
    if (!wait || timeout.count() == 0) return Got::kBusy;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    Waiter w;
    auto pos = e.queue.insert(e.queue.end(), &w);
    for (int i = 0; i < kSpin && e.owner != nullptr; i++) {
      g.unlock();
      std::this_thread::yield();
      g.lock();
    }
    Got got = Got::kBusy;
    for (;;) {
      if (e.owner == nullptr) {
        e.owner = owner;
        got = Got::kWaited;
        break;
      }
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline || killed()) break;
      w.cv.wait_until(g, std::min(deadline, now + std::chrono::milliseconds(100)));
    }
    e.queue.erase(pos);
    return got;
  }

  // Release frees key, which the caller holds, and wakes its first waiters.
  void Release(const std::string &key) {
    Shard &sh = ShardOf(key);
    std::lock_guard g(sh.mu);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return;
    Entry &e = it->second;
    if (e.queue.empty()) {
      sh.map.erase(it);
      return;
    }
    e.owner = nullptr;
    // A Waiter lives on its owner's stack until the owner leaves the queue
    // under the shard's mutex, so notify while holding it.
    int n = 0;
    for (Waiter *w : e.queue) {
      if (n++ == kWake) break;
      w->cv.notify_one();
    }
  }

  // Waiters returns how many owners await key.
  size_t Waiters(const std::string &key) {
    Shard &sh = ShardOf(key);
    std::lock_guard g(sh.mu);
    auto it = sh.map.find(key);
    return it == sh.map.end() ? 0 : it->second.queue.size();
  }

  // Size returns how many keys are held or awaited.
  size_t Size() {
    size_t n = 0;
    for (Shard &sh : shards_) {
      std::lock_guard g(sh.mu);
      n += sh.map.size();
    }
    return n;
  }

 private:
  static constexpr size_t kShards = 64;
  struct Waiter {
    std::condition_variable cv;
  };
  struct Entry {
    const void *owner = nullptr;
    // The columns the holders and waiters since the entry's creation read
    // or assign; it resets when the key falls idle.
    ColMask cols = 0;
    std::list<Waiter *> queue;  // in arrival order
  };
  struct Shard {
    std::mutex mu;
    std::unordered_map<std::string, Entry> map;  // node-based: entries stay put while waiters hold them
  };

  Shard &ShardOf(const std::string &key) { return shards_[std::hash<std::string>{}(key) % kShards]; }

  Shard shards_[kShards];
};

}  // namespace celltnx::engine
