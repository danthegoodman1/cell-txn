// An open-addressing hash map with linear probing, for a transaction's
// read and write positions. It grows until Reset.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace celltnx {

template <class K, class V, class Hash>
class FlatMap {
 public:
  V *Find(const K &k) {
    if (size_ == 0) return nullptr;
    size_t mask = slots_.size() - 1;
    for (size_t i = Hash{}(k) & mask;; i = (i + 1) & mask) {
      Slot &s = slots_[i];
      if (!s.used) return nullptr;
      if (s.kv.first == k) return &s.kv.second;
    }
  }
  const V *Find(const K &k) const { return const_cast<FlatMap *>(this)->Find(k); }

  // Insert adds k with v; k must be absent.
  void Insert(const K &k, V v) {
    if ((size_ + 1) * 4 > slots_.size() * 3) Rehash(slots_.empty() ? 16 : slots_.size() * 2);
    Place(k, std::move(v));
    size_++;
  }

  // Reset empties the map for reuse. It keeps a small table and releases
  // a large one, so one large transaction does not slow every later Reset.
  void Reset() {
    if (slots_.size() > kKeepSlots) {
      slots_ = {};
    } else if (size_ > 0) {
      for (Slot &s : slots_) s.used = false;
    }
    size_ = 0;
  }

  size_t size() const { return size_; }

 private:
  static constexpr size_t kKeepSlots = 1024;

  struct Slot {
    std::pair<K, V> kv{};
    bool used = false;
  };

  void Place(const K &k, V v) {
    size_t mask = slots_.size() - 1;
    for (size_t i = Hash{}(k) & mask;; i = (i + 1) & mask) {
      if (!slots_[i].used) {
        slots_[i].kv = {k, std::move(v)};
        slots_[i].used = true;
        return;
      }
    }
  }

  void Rehash(size_t n) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(n, Slot{});
    for (Slot &s : old)
      if (s.used) Place(s.kv.first, std::move(s.kv.second));
  }

  std::vector<Slot> slots_;
  size_t size_ = 0;
};

}  // namespace celltnx
