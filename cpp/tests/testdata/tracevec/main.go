// Prints the recorder trace and outcome counts of deterministic
// multi-client runs of every workload in every mode, for the C++ port's
// end-to-end check. Clients run on goroutines but pass a baton: exactly
// one runs at a time, and a seeded stream picks who runs next at every
// yield and sleep, so transactions interleave, conflict and retry the same
// way on every run:
//
//	go run ./cpp/tests/testdata/tracevec > cpp/tests/testdata/trace_vectors.txt
package main

import (
	"fmt"
	"math/rand/v2"
	"os"
	"slices"

	"cell-tnx/check"
	"cell-tnx/txn"
	"cell-tnx/workload"
)

// baton hands control from client to client.
type baton struct {
	r    *rand.Rand
	turn []chan struct{}
	live []int
	now  int64
	done chan struct{}
}

func (b *baton) pick() int { return b.live[b.r.IntN(len(b.live))] }

func (b *baton) yield(i int) {
	b.now++
	j := b.pick()
	if j == i {
		return
	}
	b.turn[j] <- struct{}{}
	<-b.turn[i]
}

func (b *baton) exit(i int) {
	b.live = slices.DeleteFunc(b.live, func(x int) bool { return x == i })
	if len(b.live) == 0 {
		close(b.done)
		return
	}
	b.turn[b.pick()] <- struct{}{}
}

type env struct {
	b *baton
	i int
}

func (e env) Yield() { e.b.yield(e.i) }
func (e env) Sleep(ticks int64) {
	e.b.now += ticks
	e.b.yield(e.i)
}
func (e env) Now() int64    { return e.b.now }
func (e env) Stopped() bool { return false }

// idle runs the loader alone.
type idle struct{}

func (idle) Yield()        {}
func (idle) Sleep(int64)   {}
func (idle) Now() int64    { return 0 }
func (idle) Stopped() bool { return false }

func run(seed uint64, mode txn.Mode, w workload.Workload) {
	db := txn.Open(txn.Options{Mode: mode, BucketBits: uint(seed % 4)}, w.Schemas()...)
	rec := check.NewRecorder()
	loader := &workload.Client{DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(seed, 2)), Env: idle{}}
	if err := w.Load(loader); err != nil {
		fmt.Fprintln(os.Stderr, "load:", err)
		os.Exit(1)
	}
	n := 1 + int(seed%6)
	b := &baton{r: rand.New(rand.NewPCG(seed, 3)), done: make(chan struct{})}
	clients := make([]*workload.Client, n)
	errs := make([]error, n)
	for i := range n {
		b.turn = append(b.turn, make(chan struct{}))
		b.live = append(b.live, i)
		clients[i] = &workload.Client{ID: i + 1, DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(seed, uint64(i+4))),
			Env: env{b, i}, Quota: 25}
	}
	for i, c := range clients {
		go func() {
			<-b.turn[i]
			errs[i] = workload.Run(w, c)
			b.exit(i)
		}()
	}
	b.turn[b.pick()] <- struct{}{}
	<-b.done
	var st workload.Stats
	failed := 0
	for i, c := range clients {
		st.Add(c.Stats)
		if errs[i] != nil {
			fmt.Fprintln(os.Stderr, "run:", errs[i])
			failed++
		}
	}
	replay := "ok"
	if err := check.Replay(w.Schemas(), rec.Logs(), db.LastCommitTs(), db.Dump); err != nil {
		fmt.Fprintln(os.Stderr, "replay:", err)
		replay = "fail"
	}
	fmt.Printf("%s %d %s clients=%d trace=%#x logs=%d last=%d now=%d commits=%d rejected=%d aborts=%d "+
		"conflicts=%d rowconf=%d bucketconf=%d failed=%d replay=%s\n",
		w.Name(), seed, mode, n, rec.Trace(), len(rec.Logs()), db.LastCommitTs(), b.now, st.Commits, st.Rejected,
		st.UserAborts, st.Conflicts, st.RowConf, st.BucketConf, failed, replay)
}

func main() {
	for seed := range uint64(60) {
		for _, mode := range []txn.Mode{txn.Row, txn.Cell, txn.CellDelta} {
			r := rand.New(rand.NewPCG(seed, 11))
			for _, w := range []workload.Workload{workload.NewRandom(r), workload.NewUsersFrom(r), workload.NewTPCCFrom(r)} {
				run(seed, mode, w)
			}
		}
	}
}
