// An ordered map from byte-string keys to values. One writer (the
// committer, under the commit lock) inserts; readers traverse without
// locks. Keys are never removed, so a node pointer stays valid for the
// skiplist's lifetime.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string_view>

#include "bugs.h"

namespace celltnx {

inline constexpr int kMaxLevel = 20;

// Hook is an optional interleaving point: the simulator switches coroutines
// there, and production passes nullptr.
using Hook = const std::function<void()> *;

// LevelOf derives a node's height from its key, so the structure is
// deterministic without a random source.
inline int LevelOf(std::string_view key) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
  h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
  h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
  h ^= h >> 31;
  int lvl = 1;
  while ((h & 3) == 0 && lvl < kMaxLevel) {
    lvl++;
    h >>= 2;
  }
  return lvl;
}

// A node is one allocation: this header, its next pointers, then its key.
template <class T>
struct alignas(std::atomic<void *>) Node {
  T val{};
  uint32_t klen = 0;
  uint8_t height = 0;

  std::atomic<Node *> *nexts() { return reinterpret_cast<std::atomic<Node *> *>(this + 1); }
  std::atomic<Node *> &next(int i) { return nexts()[i]; }
  std::string_view key() const {
    auto *p = reinterpret_cast<const char *>(const_cast<Node *>(this)->nexts() + height);
    return {p, klen};
  }
  Node *Next() { return next(0).load(std::memory_order_acquire); }

  static Node *Make(std::string_view key, int height) {
    static_assert(alignof(Node) >= alignof(std::atomic<Node *>));
    size_t size = sizeof(Node) + height * sizeof(std::atomic<Node *>) + key.size();
    void *mem = malloc(size);
    if (mem == nullptr) throw std::bad_alloc();
    Node *n = new (mem) Node();
    n->klen = uint32_t(key.size());
    n->height = uint8_t(height);
    for (int i = 0; i < height; i++) new (&n->next(i)) std::atomic<Node *>(nullptr);
    if (!key.empty()) memcpy(const_cast<char *>(n->key().data()), key.data(), key.size());
    return n;
  }
  static void Free(Node *n) {
    n->~Node();
    free(n);
  }
};

template <class T>
class Skiplist {
 public:
  using N = Node<T>;

  Skiplist() : head_(N::Make({}, kMaxLevel)) {}
  Skiplist(const Skiplist &) = delete;
  Skiplist &operator=(const Skiplist &) = delete;
  ~Skiplist() {
    for (N *n = head_; n != nullptr;) {
      N *next = n->next(0).load(std::memory_order_relaxed);
      N::Free(n);
      n = next;
    }
  }

  // Seek returns the first node with key >= k, filling preds when non-null.
  // It returns the node it compared: reloading the predecessor's next
  // pointer could see a smaller key that the writer inserted after the
  // comparison. Readers pass the interleave hook, which runs before Seek
  // returns so a commit can interleave there; the writer passes nullptr.
  N *Seek(std::string_view k, N **preds, Hook interleave) const {
    N *x = head_;
    N *n = nullptr;
    for (int i = kMaxLevel - 1; i >= 0; i--) {
      for (;;) {
        n = x->next(i).load(std::memory_order_acquire);
        if (n == nullptr || n->key() >= k) break;
        x = n;
      }
      if (preds != nullptr) preds[i] = x;
    }
    if (interleave != nullptr) {
      (*interleave)();
      if (bugSeekReload) return x->next(0).load(std::memory_order_acquire);
    }
    return n;
  }

  N *Get(std::string_view k, Hook interleave) const {
    N *n = Seek(k, nullptr, interleave);
    return n != nullptr && n->key() == k ? n : nullptr;
  }

  N *First() const { return head_->next(0).load(std::memory_order_acquire); }

  // GetOrInsert must only be called by the single writer. A new node is
  // fully built before it is linked, bottom level first, so readers always
  // see a sorted list at level 0. interleave, when set, runs before each
  // link, so readers in the simulator see a node linked at some levels only.
  N *GetOrInsert(std::string_view k, Hook interleave) {
    N *preds[kMaxLevel];
    if (N *n = Seek(k, preds, nullptr); n != nullptr && n->key() == k) return n;
    int lvl = LevelOf(k);
    N *n = N::Make(k, lvl);
    if (bugLinkBeforeInit) {
      for (int i = 0; i < lvl; i++) {
        N *succ = preds[i]->next(i).load(std::memory_order_relaxed);
        preds[i]->next(i).store(n, std::memory_order_release);
        if (interleave != nullptr) (*interleave)();
        n->next(i).store(succ, std::memory_order_release);
      }
      return n;
    }
    for (int i = 0; i < lvl; i++) n->next(i).store(preds[i]->next(i).load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int i = 0; i < lvl; i++) {
      if (interleave != nullptr) (*interleave)();
      preds[i]->next(i).store(n, std::memory_order_release);
    }
    return n;
  }

  // Check verifies that every level is sorted.
  bool Check() const {
    for (int lvl = 0; lvl < kMaxLevel; lvl++) {
      N *prev = nullptr;
      for (N *n = head_->next(lvl).load(std::memory_order_acquire); n != nullptr;
           n = n->next(lvl).load(std::memory_order_acquire)) {
        if (prev != nullptr && prev->key() >= n->key()) return false;
        prev = n;
      }
    }
    return true;
  }

 private:
  N *head_;
};

}  // namespace celltnx
