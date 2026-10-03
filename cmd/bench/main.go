// Command bench measures throughput, aborts and latency for a workload on
// real goroutines, in each conflict-tracking mode.
//
//	bench -workload tpcc -warehouses 1 -clients 16 -duration 10s
//	bench -workload users -theta 0.9 -mix 4,0,4,2,0,0 -csv out.csv
package main

import (
	"errors"
	"flag"
	"fmt"
	"math/bits"
	"math/rand/v2"
	"os"
	"runtime/debug"
	"runtime/pprof"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"cell-tnx/check"
	"cell-tnx/txn"
	"cell-tnx/workload"
)

func main() {
	if os.Getenv("GOGC") == "" {
		// The heap is many small long-lived objects; collect less often.
		debug.SetGCPercent(400)
	}
	wl := flag.String("workload", "tpcc", "users or tpcc")
	modes := flag.String("modes", "row,cell,cell+delta", "modes to run")
	clients := flag.Int("clients", 16, "concurrent clients")
	duration := flag.Duration("duration", 5*time.Second, "run time per mode")
	bucketBits := flag.Uint("bucketbits", 4, "bucketOf(pk) = pk >> bucketbits")
	think := flag.Float64("think", 0, "per-operation chance of a 1-50µs pause")
	csvPath := flag.String("csv", "", "append results to this CSV file")
	verify := flag.Bool("check", false, "record the history and verify it after each run")
	cpuprofile := flag.String("cpuprofile", "", "write a CPU profile of the measured runs")
	// users
	rows := flag.Uint64("rows", 1000, "users: rows")
	theta := flag.Float64("theta", 0.9, "users: Zipf skew in [0,1)")
	mix := flag.String("mix", "4,4,4,2,1,2", "users: weights for pay,deposit,profile,audit,same,read")
	// tpcc
	warehouses := flag.Uint64("warehouses", 1, "tpcc: warehouses")
	districts := flag.Uint64("districts", 10, "tpcc: districts per warehouse")
	customers := flag.Uint64("customers", 3000, "tpcc: customers per district")
	items := flag.Uint64("items", 10000, "tpcc: items")
	lines := flag.Int("lines", 15, "tpcc: max order lines")
	flag.Parse()

	var w workload.Workload
	param := ""
	switch *wl {
	case "users":
		var m [6]int
		for i, f := range strings.Split(*mix, ",") {
			m[i], _ = strconv.Atoi(f)
		}
		w = workload.NewUsers(*rows, *theta, m, *think)
		param = fmt.Sprintf("theta=%.2f mix=%s", *theta, strings.ReplaceAll(*mix, ",", "/"))
	case "tpcc":
		t := workload.NewTPCC(*warehouses, *districts, *customers, *items, *lines)
		t.Mix[2] = 0
		t.Think = *think
		w = t
		param = fmt.Sprintf("warehouses=%d", *warehouses)
	default:
		fmt.Fprintln(os.Stderr, "unknown workload", *wl)
		os.Exit(2)
	}

	if *cpuprofile != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	var out *os.File
	if *csvPath != "" {
		f, err := os.OpenFile(*csvPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer f.Close()
		if st, _ := f.Stat(); st.Size() == 0 {
			fmt.Fprintln(f, "workload,param,mode,clients,seconds,commits_per_s,rejected,conflicts,row_conflicts,bucket_conflicts,retries_per_txn,p50_us,p99_us,lock_wait_p50_ns,lock_wait_p99_ns,lock_hold_p50_ns,lock_hold_p99_ns")
		}
		out = f
	}

	fmt.Printf("%-11s %10s %9s %9s %8s %8s %8s %10s %10s\n", "mode", "commits/s", "conflicts", "retry/txn", "p50µs", "p99µs", "rejected", "lockwait99", "lockhold99")
	for _, name := range strings.Split(*modes, ",") {
		mode := map[string]txn.Mode{"row": txn.Row, "cell": txn.Cell, "cell+delta": txn.CellDelta}[name]
		r := run(w, mode, *clients, *duration, *bucketBits, *verify)
		secs := r.elapsed.Seconds()
		fmt.Printf("%-11s %10.0f %9d %9.2f %8d %8d %8d %9dns %9dns\n", name, float64(r.stats.Commits)/secs,
			r.stats.Conflicts, r.retriesPerTxn(), r.lat.quantile(0.5), r.lat.quantile(0.99), r.stats.Rejected,
			r.wait.quantile(0.99), r.hold.quantile(0.99))
		if out != nil {
			fmt.Fprintf(out, "%s,%s,%s,%d,%.2f,%.0f,%d,%d,%d,%d,%.3f,%d,%d,%d,%d,%d,%d\n", *wl, param, name, *clients, secs,
				float64(r.stats.Commits)/secs, r.stats.Rejected, r.stats.Conflicts, r.stats.RowConf, r.stats.BucketConf,
				r.retriesPerTxn(), r.lat.quantile(0.5), r.lat.quantile(0.99),
				r.wait.quantile(0.5), r.wait.quantile(0.99), r.hold.quantile(0.5), r.hold.quantile(0.99))
		}
	}
}

type result struct {
	stats           workload.Stats
	txns, retries   int64
	elapsed         time.Duration
	lat, wait, hold hist
}

func (r *result) retriesPerTxn() float64 { return float64(r.retries) / max(1, float64(r.txns)) }

type env struct {
	start    time.Time
	deadline time.Time
}

func (e env) Yield()            {}
func (e env) Sleep(ticks int64) { time.Sleep(time.Duration(ticks) * time.Microsecond) }
func (e env) Now() int64        { return time.Since(e.start).Microseconds() }
func (e env) Stopped() bool     { return time.Now().After(e.deadline) }

func run(w workload.Workload, mode txn.Mode, clients int, d time.Duration, bucketBits uint, verify bool) *result {
	res := &result{}
	start := time.Now()
	var measuring atomic.Bool
	db := txn.Open(txn.Options{
		Mode:       mode,
		BucketBits: bucketBits,
		Now:        func() int64 { return int64(time.Since(start)) },
		OnCommit: func(wait, hold int64) {
			if measuring.Load() {
				res.wait.add(wait)
				res.hold.add(hold)
			}
		},
	}, w.Schemas()...)
	rec := check.NewRecorder()
	rec.Off = !verify
	e := env{start: start, deadline: time.Now().Add(time.Hour)}
	if err := w.Load(&workload.Client{DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(1, 1)), Env: e}); err != nil {
		fmt.Fprintln(os.Stderr, "load:", err)
		os.Exit(1)
	}
	measuring.Store(true)
	e.deadline = time.Now().Add(d)
	t0 := time.Now()
	var wg sync.WaitGroup
	var mu sync.Mutex
	for i := range clients {
		c := &workload.Client{ID: i + 1, DB: db, Rec: rec, Rand: rand.New(rand.NewPCG(uint64(i), 7)), Env: e}
		c.OnDone = func(lat int64, retries int, err error) {
			res.lat.add(lat)
			atomic.AddInt64(&res.txns, 1)
			atomic.AddInt64(&res.retries, int64(retries))
		}
		wg.Go(func() {
			if err := workload.Run(w, c); err != nil && !errors.Is(err, txn.ErrDone) {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
			mu.Lock()
			res.stats.Add(c.Stats)
			mu.Unlock()
		})
	}
	wg.Wait()
	res.elapsed = time.Since(t0)
	if verify {
		if err := check.Replay(w.Schemas(), rec.Logs(), db.LastCommitTs(), db.Dump); err != nil {
			fmt.Fprintln(os.Stderr, "check:", err)
			os.Exit(1)
		}
	}
	return res
}

// hist is a concurrent log-linear histogram: 8 sub-buckets per power of 2.
type hist struct {
	counts [64 * 8]atomic.Int64
}

func bucketOf(v int64) int {
	if v < 8 {
		return int(max(v, 0))
	}
	e := 63 - bits.LeadingZeros64(uint64(v))
	return e*8 + int(v>>(e-3)&7)
}

func lowerBound(b int) int64 {
	if b < 8 {
		return int64(b)
	}
	e := b / 8
	return int64(8+b%8) << (e - 3)
}

func (h *hist) add(v int64) { h.counts[bucketOf(v)].Add(1) }

func (h *hist) quantile(q float64) int64 {
	var total int64
	for i := range h.counts {
		total += h.counts[i].Load()
	}
	if total == 0 {
		return 0
	}
	target := int64(q * float64(total))
	var seen int64
	for i := range h.counts {
		if seen += h.counts[i].Load(); seen > target {
			return lowerBound(i)
		}
	}
	return lowerBound(len(h.counts) - 1)
}
