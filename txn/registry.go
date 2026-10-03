package txn

import (
	"sort"
	"sync"
)

// registry tracks the readTs of every active transaction so GC knows the
// oldest snapshot still in use. Begin loads lastCommitTs and registers under
// one mutex, so GC never computes a horizon that skips a starting
// transaction.
type registry struct {
	mu    sync.Mutex
	slots []slot // ascending ts; slots[head:] are live
	head  int
}

type slot struct {
	ts uint64
	n  int
}

func (r *registry) begin(load func() uint64) uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	ts := load()
	if k := len(r.slots); k > r.head && r.slots[k-1].ts == ts {
		r.slots[k-1].n++
	} else {
		r.slots = append(r.slots, slot{ts, 1})
	}
	return ts
}

func (r *registry) end(ts uint64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	live := r.slots[r.head:]
	i := sort.Search(len(live), func(i int) bool { return live[i].ts >= ts })
	if i == len(live) || live[i].ts != ts || live[i].n == 0 {
		panic("txn: registry end without begin")
	}
	live[i].n--
	for r.head < len(r.slots) && r.slots[r.head].n == 0 {
		r.head++
	}
	if r.head > 64 && r.head*2 > len(r.slots) {
		r.slots = append(r.slots[:0], r.slots[r.head:]...)
		r.head = 0
	}
}

// horizon returns the oldest active readTs, or last when none is active.
func (r *registry) horizon(last uint64) uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.head == len(r.slots) {
		return last
	}
	return r.slots[r.head].ts
}

func (r *registry) check() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	for i := r.head + 1; i < len(r.slots); i++ {
		if r.slots[i-1].ts >= r.slots[i].ts {
			return errorf("registry slots out of order at %d", i)
		}
	}
	return nil
}
