package txn

import (
	"encoding/binary"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
)

// Readers look up keys just above the writer's insertion point while it
// inserts; a lookup must never miss a key that existed before it began.
func TestSkiplistConcurrentGet(t *testing.T) {
	key := func(i uint64) string { return string(binary.BigEndian.AppendUint64(nil, i)) }
	for round := range 5 {
		s := newSkiplist[int]()
		const n = 1 << 18
		for i := uint64(0); i < n; i += 2 {
			s.getOrInsert(key(i), nil)
		}
		var pos atomic.Uint64 // the odd key the writer inserts next
		pos.Store(1)
		var done atomic.Bool
		var misses atomic.Int64
		var wg sync.WaitGroup
		for range max(1, runtime.GOMAXPROCS(0)-1) {
			wg.Go(func() {
				for !done.Load() {
					k := pos.Load() + 1 // even, so it exists
					if k < n && s.get(key(k), nil) == nil {
						misses.Add(1)
					}
				}
			})
		}
		for i := uint64(1); i < n; i += 2 {
			pos.Store(i)
			s.getOrInsert(key(i), nil)
		}
		done.Store(true)
		wg.Wait()
		if m := misses.Load(); m > 0 {
			t.Fatalf("round %d: %d lookups missed an existing key", round, m)
		}
		if !s.check() {
			t.Fatalf("round %d: skiplist out of order", round)
		}
	}
}
