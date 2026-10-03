package badger

import (
	"encoding/gob"
	"fmt"
	"math/rand/v2"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"testing"
	"time"

	"cell-tnx/check"
	"cell-tnx/txn"
	"cell-tnx/workload"
)

func init() {
	gob.Register(int64(0))
	gob.Register(uint64(0))
	gob.Register(float64(0))
	gob.Register("")
}

// record is one history event streamed from the child: a transaction
// installed (Log set) or acknowledged.
type record struct {
	Index int
	Ack   bool
	Log   check.Log
}

func crashWorkload() *workload.TPCC { return workload.NewTPCC(2, 4, 30, 200, 10) }

type env struct{ start time.Time }

func (e env) Yield()            {}
func (e env) Sleep(ticks int64) { time.Sleep(time.Duration(ticks) * time.Microsecond) }
func (e env) Now() int64        { return time.Since(e.start).Microseconds() }
func (e env) Stopped() bool     { return false }

// TestCrash runs TPC-C in sync mode in a child process, kills it with
// SIGKILL at a random moment, recovers the store, and checks that no
// acknowledged commit was lost and that the recovered state equals the
// serial replay of every surviving commit. It repeats this several times
// on the same store.
func TestCrash(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns processes")
	}
	dir := t.TempDir()
	schemas := crashWorkload().Schemas()
	var history []check.Log
	r := rand.New(rand.NewPCG(uint64(time.Now().UnixNano()), 0))
	for round := range 8 {
		pr, pw, err := os.Pipe()
		if err != nil {
			t.Fatal(err)
		}
		cmd := exec.Command(os.Args[0], "-test.run=^TestCrashChild$")
		// Odd rounds hold each sync open, so batches gather commits.
		cmd.Env = append(os.Environ(), "CRASH_DIR="+dir, "CRASH_COMMIT_DELAY="+[...]string{"0", "500us"}[round%2])
		cmd.ExtraFiles = []*os.File{pw}
		cmd.Stderr = os.Stderr
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		pw.Close()
		base := len(history)
		var mu sync.Mutex
		done := make(chan struct{})
		go func() {
			defer close(done)
			dec := gob.NewDecoder(pr)
			for {
				var rec record
				if dec.Decode(&rec) != nil {
					return
				}
				mu.Lock()
				if rec.Ack {
					history[base+rec.Index].Acked = true
				} else {
					history = append(history, rec.Log)
				}
				mu.Unlock()
			}
		}()
		time.Sleep(time.Duration(300+r.IntN(1200)) * time.Millisecond)
		cmd.Process.Kill()
		cmd.Wait()
		<-done
		pr.Close()
		if ws, ok := cmd.ProcessState.Sys().(syscall.WaitStatus); !ok || !ws.Signaled() {
			t.Fatalf("round %d: child exited on its own: %v", round, cmd.ProcessState)
		}

		st, err := Open(dir)
		if err != nil {
			t.Fatal(err)
		}
		last := st.Last()
		rec := check.NewRecorder()
		rec.SetLogs(history)
		if err := rec.Truncate(last, true); err != nil {
			t.Fatalf("round %d: %v", round, err)
		}
		history = rec.Logs()
		acked := 0
		for _, l := range history {
			if l.Acked {
				acked++
			}
		}
		db := txn.Open(txn.Options{}, schemas...)
		if err := db.Recover(last, st.Load); err != nil {
			t.Fatal(err)
		}
		if err := db.CheckInvariants(); err != nil {
			t.Fatalf("round %d: %v", round, err)
		}
		if err := check.Replay(schemas, history, last, db.Dump); err != nil {
			t.Fatalf("round %d: %v", round, err)
		}
		st.Close()
		t.Logf("round %d: recovered through commit %d; %d commits logged, %d acknowledged", round, last, len(history), acked)
	}
}

// TestCrashChild is the process TestCrash kills. It recovers the store,
// loads TPC-C if the store is empty, and runs eight clients until killed,
// streaming its history to file descriptor 3.
func TestCrashChild(t *testing.T) {
	dir := os.Getenv("CRASH_DIR")
	if dir == "" {
		t.Skip("run by TestCrash")
	}
	st, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	w := crashWorkload()
	delay, err := time.ParseDuration(os.Getenv("CRASH_COMMIT_DELAY"))
	if err != nil {
		t.Fatal(err)
	}
	opts := txn.Options{Mode: txn.CellDelta, BucketBits: 2, Store: st}
	if delay > 0 {
		opts.CommitDelay = func() { time.Sleep(delay) }
	}
	db := txn.Open(opts, w.Schemas()...)
	if err := db.Recover(st.Last(), st.Load); err != nil {
		t.Fatal(err)
	}
	go func() {
		if err := db.RunWriter(); err != nil {
			fmt.Fprintln(os.Stderr, "writer:", err)
			os.Exit(1)
		}
	}()
	out := os.NewFile(3, "history")
	enc := gob.NewEncoder(out)
	rec := check.NewRecorder()
	rec.OnAdd = func(i int, l check.Log) { must(enc.Encode(record{Index: i, Log: l})) }
	rec.OnAck = func(i int) { must(enc.Encode(record{Index: i, Ack: true})) }
	e := env{time.Now()}
	if st.Last() == 0 {
		if err := w.Load(&workload.Client{DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(1, 1)), Env: e}); err != nil {
			t.Fatal(err)
		}
	}
	seed := uint64(time.Now().UnixNano())
	var wg sync.WaitGroup
	for i := range 8 {
		c := &workload.Client{ID: i + 1, DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(seed, uint64(i))), Env: e}
		wg.Go(func() {
			if err := workload.Run(w, c); err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
		})
	}
	wg.Wait()
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}
