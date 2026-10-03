// Command sim runs deterministic simulations: one seed verbosely, or a
// sweep of seeds in parallel with determinism checks.
//
//	sim -seed 42                 replay one seed
//	sim -seeds 30000             sweep seeds [start, start+30000)
//	sim -soak                    sweep random seeds until interrupted
//	sim -bug gc-eager -seeds N   self-test (needs -tags simbugs)
package main

import (
	"flag"
	"fmt"
	"math/rand/v2"
	"os"
	"os/signal"
	"runtime"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"cell-tnx/sim"
	"cell-tnx/txn"
	"cell-tnx/workload"
)

func main() {
	seed := flag.Int64("seed", -1, "run this single seed and print its result")
	seeds := flag.Int("seeds", 1000, "number of seeds to sweep")
	start := flag.Uint64("start", 0, "first seed of the sweep")
	soak := flag.Bool("soak", false, "sweep random seeds until interrupted")
	workers := flag.Int("workers", runtime.GOMAXPROCS(0), "parallel simulations")
	wl := flag.String("workload", "", "random, users or tpcc (default: drawn per seed)")
	mode := flag.Int("mode", -1, "0=row 1=cell 2=cell+delta (default: drawn per seed)")
	durable := flag.Int("durable", -1, "1 forces a simulated disk with crashes, 0 forbids one (default: drawn per seed)")
	commitDelay := flag.Int64("commitdelay", -1, "the writer's delay before each flush, in steps (default: drawn per seed)")
	bug := flag.String("bug", "", "enable a deliberate bug; the sweep must find it")
	det := flag.Int("det", 10, "rerun every Nth seed to check determinism (0 disables)")
	flag.Parse()

	cfg := sim.Config{Workload: *wl, Mode: *mode, Durable: *durable, CommitDelay: *commitDelay}
	if *bug != "" {
		if err := txn.SetBug(*bug); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		cfg.NoInvariants = true
	}

	if *seed >= 0 {
		r := sim.Run(uint64(*seed), cfg)
		fmt.Printf("seed %d: %s\nsteps=%d crashes=%d logs=%d %+v trace=%#x\n", r.Seed, r.Params, r.Steps, r.Crashes, r.Logs, r.Stats, r.Trace)
		if r.Err != nil {
			fmt.Printf("FAIL: %v\n", r.Err)
			os.Exit(1)
		}
		fmt.Println("ok")
		return
	}

	next := func() func() (uint64, bool) {
		if *soak {
			r := rand.New(rand.NewPCG(uint64(time.Now().UnixNano()), 0))
			stop := make(chan os.Signal, 1)
			signal.Notify(stop, os.Interrupt)
			var stopped atomic.Bool
			go func() { <-stop; stopped.Store(true) }()
			return func() (uint64, bool) { return r.Uint64(), !stopped.Load() }
		}
		var i uint64
		return func() (uint64, bool) {
			if i == uint64(*seeds) {
				return 0, false
			}
			i++
			return *start + i - 1, true
		}
	}()

	var (
		mu       sync.Mutex
		ran      int
		crashes  int
		failures []sim.Result
		nondet   []uint64
		total    workload.Stats
		steps    int
		cover    = map[string]int{}
	)
	t0 := time.Now()
	var wg sync.WaitGroup
	var nextMu sync.Mutex
	for range *workers {
		wg.Go(func() {
			for {
				nextMu.Lock()
				s, ok := next()
				nextMu.Unlock()
				if !ok {
					return
				}
				r := sim.Run(s, cfg)
				deterministic := true
				if *det > 0 && s%uint64(*det) == 0 {
					r2 := sim.Run(s, cfg)
					deterministic = r2.Trace == r.Trace && r2.Steps == r.Steps && fmt.Sprint(r2.Err) == fmt.Sprint(r.Err)
				}
				mu.Lock()
				ran++
				steps += r.Steps
				crashes += r.Crashes
				total.Add(r.Stats)
				store := "memory"
				if r.Params.Durable {
					store = "durable"
				}
				cover[fmt.Sprintf("%-10s %-6s %s", r.Params.Mode, r.Params.Workload, store)]++
				if r.Err != nil {
					failures = append(failures, r)
				}
				if !deterministic {
					nondet = append(nondet, s)
				}
				if *soak && ran%10000 == 0 {
					fmt.Printf("%d seeds, %d failures, %s\n", ran, len(failures), time.Since(t0).Round(time.Second))
				}
				mu.Unlock()
			}
		})
	}
	wg.Wait()

	fmt.Printf("seeds: %d in %s (%d workers)\n", ran, time.Since(t0).Round(time.Millisecond), *workers)
	fmt.Printf("steps: %d  commits: %d  rejected: %d  user aborts: %d  conflicts: %d (row %d, bucket %d)  crashes: %d\n",
		steps, total.Commits, total.Rejected, total.UserAborts, total.Conflicts, total.RowConf, total.BucketConf, crashes)
	var keys []string
	for k := range cover {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Printf("  %-29s %6d seeds\n", k, cover[k])
	}
	if *det > 0 {
		fmt.Printf("determinism: %d seeds rerun, %d mismatches %v\n", (ran+*det-1) / *det, len(nondet), nondet)
	}
	sort.Slice(failures, func(i, j int) bool { return failures[i].Seed < failures[j].Seed })
	fmt.Printf("failures: %d\n", len(failures))
	for i, f := range failures {
		if i == 5 {
			fmt.Printf("  ... %d more\n", len(failures)-5)
			break
		}
		fmt.Printf("  seed %d: %s\n    %v\n", f.Seed, f.Params, firstLines(f.Err.Error(), 6))
	}
	if *bug != "" {
		if len(failures) == 0 {
			fmt.Printf("SELF-TEST FAILED: bug %s not found\n", *bug)
			os.Exit(1)
		}
		fmt.Printf("bug %s found: first at seed %d\n", *bug, failures[0].Seed)
		return
	}
	if len(failures) > 0 || len(nondet) > 0 {
		os.Exit(1)
	}
}

func firstLines(s string, n int) string {
	for i, c := range s {
		if c == '\n' {
			if n--; n == 0 {
				return s[:i]
			}
		}
	}
	return s
}
