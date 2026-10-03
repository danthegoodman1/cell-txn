package workload

import (
	"math/rand/v2"
	"runtime"
	"sync"
	"testing"
	"time"

	"cell-tnx/check"
	"cell-tnx/txn"
)

// realEnv runs clients on real goroutines; ticks are microseconds.
type realEnv struct{ start time.Time }

func (e realEnv) Yield()            { runtime.Gosched() }
func (e realEnv) Sleep(ticks int64) { time.Sleep(time.Duration(ticks) * time.Microsecond) }
func (e realEnv) Now() int64        { return time.Since(e.start).Microseconds() }
func (e realEnv) Stopped() bool     { return false }

// TestStress runs every workload on real goroutines in every mode and
// checks the history. Run it under -race.
func TestStress(t *testing.T) {
	for seed := range uint64(3) {
		for _, mode := range []txn.Mode{txn.Row, txn.Cell, txn.CellDelta} {
			r := rand.New(rand.NewPCG(seed, 1))
			for _, w := range []Workload{NewRandom(r), NewUsersFrom(r), NewTPCCFrom(r)} {
				t.Run(mode.String()+"/"+w.Name(), func(t *testing.T) {
					t.Parallel()
					stress(t, w, mode, seed)
				})
			}
		}
	}
}

func stress(t *testing.T, w Workload, mode txn.Mode, seed uint64) {
	db := txn.Open(txn.Options{Mode: mode, BucketBits: uint(seed % 4)}, w.Schemas()...)
	rec := check.NewRecorder()
	env := realEnv{time.Now()}
	if err := w.Load(&Client{DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(seed, 2)), Env: env}); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := range 8 {
		c := &Client{ID: i + 1, DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(seed, uint64(i+3))), Env: env, Quota: 150}
		wg.Go(func() {
			if err := Run(w, c); err != nil {
				t.Error(err)
			}
		})
	}
	wg.Wait()
	if err := db.CheckInvariants(); err != nil {
		t.Fatal(err)
	}
	if err := check.Replay(w.Schemas(), rec.Logs(), db.LastCommitTs(), db.Dump); err != nil {
		t.Fatal(err)
	}
}
