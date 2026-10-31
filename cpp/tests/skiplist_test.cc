// Ported from txn/skiplist_test.go.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "keys.h"
#include "skiplist.h"

namespace celltnx {
namespace {

// Readers look up keys just above the writer's insertion point while it
// inserts; a lookup must never miss a key that existed before it began.
TEST(Skiplist, ConcurrentGet) {
  for (int round = 0; round < 5; round++) {
    Skiplist<int> s;
    const uint64_t n = 1 << 18;
    for (uint64_t i = 0; i < n; i += 2) s.GetOrInsert(keys::U64(i), nullptr);
    std::atomic<uint64_t> pos{1};  // the odd key the writer inserts next
    std::atomic<bool> done{false};
    std::atomic<int64_t> misses{0};
    std::vector<std::thread> readers;
    unsigned procs = std::max(2u, std::thread::hardware_concurrency()) - 1;
    for (unsigned r = 0; r < procs; r++)
      readers.emplace_back([&] {
        while (!done.load()) {
          uint64_t k = pos.load() + 1;  // even, so it exists
          if (k < n && s.Get(keys::U64(k), nullptr) == nullptr) misses++;
        }
      });
    for (uint64_t i = 1; i < n; i += 2) {
      pos.store(i);
      s.GetOrInsert(keys::U64(i), nullptr);
    }
    done.store(true);
    for (auto &t : readers) t.join();
    ASSERT_EQ(misses.load(), 0) << "round " << round << ": lookups missed an existing key";
    ASSERT_TRUE(s.Check()) << "round " << round << ": skiplist out of order";
  }
}

}  // namespace
}  // namespace celltnx
