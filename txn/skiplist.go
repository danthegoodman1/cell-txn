package txn

import "sync/atomic"

const maxLevel = 20

// skiplist is an ordered map from string keys to nodes. One writer (the
// committer, under the commit lock) inserts; readers traverse without
// locks. Keys are never removed, so a node pointer stays valid forever.
type skiplist[T any] struct {
	head node[T]
}

type node[T any] struct {
	key  string
	val  T
	next []atomic.Pointer[node[T]]
}

func newSkiplist[T any]() *skiplist[T] {
	s := &skiplist[T]{}
	s.head.next = make([]atomic.Pointer[node[T]], maxLevel)
	return s
}

// levelOf derives a node's height from its key, so the structure is
// deterministic without a random source.
func levelOf(key string) int {
	h := uint64(14695981039346656037)
	for i := 0; i < len(key); i++ {
		h = (h ^ uint64(key[i])) * 1099511628211
	}
	h = (h ^ h>>30) * 0xbf58476d1ce4e5b9
	h = (h ^ h>>27) * 0x94d049bb133111eb
	h ^= h >> 31
	lvl := 1
	for h&3 == 0 && lvl < maxLevel {
		lvl++
		h >>= 2
	}
	return lvl
}

// seek returns the first node with key >= k, filling preds when non-nil.
func (s *skiplist[T]) seek(k string, preds *[maxLevel]*node[T]) *node[T] {
	x := &s.head
	for i := maxLevel - 1; i >= 0; i-- {
		for {
			n := x.next[i].Load()
			if n == nil || n.key >= k {
				break
			}
			x = n
		}
		if preds != nil {
			preds[i] = x
		}
	}
	return x.next[0].Load()
}

func (s *skiplist[T]) get(k string) *node[T] {
	if n := s.seek(k, nil); n != nil && n.key == k {
		return n
	}
	return nil
}

func (s *skiplist[T]) first() *node[T] { return s.head.next[0].Load() }

// getOrInsert must only be called by the single writer. A new node is fully
// built before it is linked, bottom level first, so readers always see a
// sorted list at level 0.
func (s *skiplist[T]) getOrInsert(k string) *node[T] {
	var preds [maxLevel]*node[T]
	if n := s.seek(k, &preds); n != nil && n.key == k {
		return n
	}
	lvl := levelOf(k)
	n := &node[T]{key: k, next: make([]atomic.Pointer[node[T]], lvl)}
	for i := range lvl {
		n.next[i].Store(preds[i].next[i].Load())
	}
	for i := range lvl {
		preds[i].next[i].Store(n)
	}
	return n
}

// check verifies that every level is sorted.
func (s *skiplist[T]) check() bool {
	for lvl := range maxLevel {
		var prev *node[T]
		for n := s.head.next[lvl].Load(); n != nil; n = n.next[lvl].Load() {
			if prev != nil && prev.key >= n.key {
				return false
			}
			prev = n
		}
	}
	return true
}
