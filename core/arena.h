// A bump allocator for one transaction's pending writes, keys and views.
// Everything it hands out lives until Reset, which keeps the first block.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

namespace celltnx {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  ~Arena() {
    for (void *b : blocks_) free(b);
  }

  void *Alloc(size_t n, size_t align = alignof(std::max_align_t)) {
    size_t p = (reinterpret_cast<uintptr_t>(cur_) + align - 1) & ~(align - 1);
    if (cur_ == nullptr || p + n > reinterpret_cast<uintptr_t>(end_)) {
      Grow(n + align);
      p = (reinterpret_cast<uintptr_t>(cur_) + align - 1) & ~(align - 1);
    }
    cur_ = reinterpret_cast<char *>(p + n);
    return reinterpret_cast<void *>(p);
  }

  template <class T>
  T *Array(size_t n) {
    return static_cast<T *>(Alloc(n * sizeof(T), alignof(T)));
  }

  std::string_view Copy(std::string_view s) {
    if (s.empty()) return {};
    char *p = static_cast<char *>(Alloc(s.size(), 1));
    memcpy(p, s.data(), s.size());
    return {p, s.size()};
  }

  // Reset frees every block but the first and rewinds into it.
  void Reset() {
    if (blocks_.empty()) return;
    for (size_t i = 1; i < blocks_.size(); i++) free(blocks_[i]);
    blocks_.resize(1);
    cur_ = static_cast<char *>(blocks_[0]);
    end_ = cur_ + first_size_;
    next_size_ = first_size_;
  }

 private:
  void Grow(size_t need) {
    size_t size = next_size_;
    while (size < need) size *= 2;
    void *b = malloc(size);
    if (b == nullptr) throw std::bad_alloc();
    if (blocks_.empty()) first_size_ = size;
    blocks_.push_back(b);
    cur_ = static_cast<char *>(b);
    end_ = cur_ + size;
    if (next_size_ < (size_t(1) << 20)) next_size_ *= 2;
  }

  char *cur_ = nullptr;
  char *end_ = nullptr;
  size_t next_size_ = 4096;
  size_t first_size_ = 0;
  std::vector<void *> blocks_;
};

}  // namespace celltnx
