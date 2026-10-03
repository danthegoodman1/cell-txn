// Package sim runs the transaction layer and a workload deterministically
// on one goroutine from a seed. Each client is an iter.Pull coroutine that
// yields before every operation and at interleaving points inside the
// store; a seeded PRNG picks which coroutine runs next. Durable seeds add a
// writer coroutine, a simulated disk, crashes and recovery. The run then
// replays every committed transaction through the checker.
package sim

import (
	"errors"
	"fmt"
	"iter"
	"math/rand/v2"
	"runtime/debug"

	"cell-tnx/check"
	"cell-tnx/txn"
	"cell-tnx/workload"
)

// Config fixes parts of the drawn parameters.
type Config struct {
	Workload     string // "random", "users", "tpcc" or "sql"; empty draws one of the first three
	Mode         int    // a txn.Mode; negative draws one
	Durable      int    // 1 forces a store, 0 forbids one, negative draws
	CommitDelay  int64  // the writer's delay in steps; negative draws one
	NoInvariants bool   // skip internal invariant checks
	MaxSteps     int    // liveness bound; zero uses a default
}

// Params are a seed's drawn parameters.
type Params struct {
	Mode        txn.Mode
	Workload    string
	Clients     int
	Quota       int
	BucketBits  uint
	Skew        bool // clients get unequal scheduling weights
	Durable     bool
	NoSync      bool    // acknowledge commits before they sync
	CrashP      float64 // per-step crash chance
	SyncFailP   float64 // per-sync failure chance; a failure crashes
	Latency     int64   // max sync time in steps
	CommitDelay int64   // the writer's wait before each flush, in steps
	Detail      string  // workload configuration
}

func (p Params) String() string {
	s := fmt.Sprintf("mode=%s workload=%s clients=%d quota=%d bucketBits=%d skew=%v",
		p.Mode, p.Workload, p.Clients, p.Quota, p.BucketBits, p.Skew)
	if p.Durable {
		s += fmt.Sprintf(" durable nosync=%v crashP=%g syncFailP=%g latency=%d commitDelay=%d", p.NoSync, p.CrashP, p.SyncFailP, p.Latency, p.CommitDelay)
	}
	return s + " " + p.Detail
}

// Result is the outcome of one seed.
type Result struct {
	Seed    uint64
	Params  Params
	Steps   int
	Crashes int
	Stats   workload.Stats
	Logs    int
	Trace   uint64 // hash of every outcome, for determinism checks
	Err     error
}

var errStopped = errors.New("sim: coroutine stopped")

type sched struct {
	now int64
	cur *co
}

type co struct {
	next    func() (struct{}, bool)
	stop    func()
	yield   func(struct{}) bool
	wake    int64
	weight  int         // scheduling weight; skewed seeds starve some clients
	blocked func() bool // set while waiting; runnable once it returns true
	daemon  bool        // runs only while other coroutines do
	done    bool
	err     error
}

// Yield suspends the running coroutine. Outside a coroutine it does nothing.
func (s *sched) Yield() {
	if c := s.cur; c != nil && !c.yield(struct{}{}) {
		panic(errStopped)
	}
}

// Wait suspends the running coroutine until cond holds.
func (s *sched) Wait(cond func() bool) {
	c := s.cur
	for !cond() {
		if c == nil {
			panic("sim: wait outside a coroutine")
		}
		c.blocked = cond
		s.Yield()
	}
	if c != nil {
		c.blocked = nil
	}
}

func (s *sched) spawn(f func() error) *co {
	c := &co{weight: 1}
	c.next, c.stop = iter.Pull(func(yield func(struct{}) bool) {
		c.yield = yield
		c.err = f()
	})
	return c
}

func stopAll(cos []*co) {
	for _, c := range cos {
		func() {
			defer func() { _ = recover() }()
			c.stop()
		}()
	}
}

// env is a client's view of the scheduler.
type env struct{ s *sched }

func (e env) Yield()        { e.s.Yield() }
func (e env) Now() int64    { return e.s.now }
func (e env) Stopped() bool { return false }
func (e env) Sleep(ticks int64) {
	if c := e.s.cur; c != nil {
		c.wake = e.s.now + ticks
		e.s.Yield()
	}
}

func draw(seed uint64, r *rand.Rand, cfg Config) (Params, workload.Workload) {
	p := Params{
		Mode:       txn.Mode(r.IntN(3)),
		Clients:    1 + r.IntN(16),
		Quota:      3 + r.IntN(40),
		BucketBits: uint(r.IntN(5)),
		Skew:       r.IntN(3) == 0,
		Durable:    r.IntN(2) == 0,
	}
	if cfg.Mode >= 0 {
		p.Mode = txn.Mode(cfg.Mode)
	}
	if cfg.Durable >= 0 {
		p.Durable = cfg.Durable == 1
	}
	if p.Durable {
		p.NoSync = r.IntN(4) == 0
		p.CrashP = [...]float64{0, 0.0005, 0.002, 0.01}[r.IntN(4)]
		p.SyncFailP = [...]float64{0, 0, 0.001, 0.01}[r.IntN(4)]
		p.Latency = [...]int64{0, 5, 50}[r.IntN(3)]
		// A separate stream leaves every other parameter as before.
		g := rand.New(rand.NewPCG(seed, 0xbb67ae8584caa73b))
		p.CommitDelay = [...]int64{0, 0, 0, 2, 10, 50, 200}[g.IntN(7)]
		if cfg.CommitDelay >= 0 {
			p.CommitDelay = cfg.CommitDelay
		}
	}
	p.Workload = cfg.Workload
	if p.Workload == "" {
		p.Workload = [...]string{"random", "random", "users", "tpcc"}[r.IntN(4)]
	}
	var w workload.Workload
	switch p.Workload {
	case "random":
		w = workload.NewRandom(r)
	case "users":
		w = workload.NewUsersFrom(r)
	case "tpcc":
		w = workload.NewTPCCFrom(r)
	default:
		panic("sim: unknown workload " + p.Workload)
	}
	p.Detail = fmt.Sprintf("%+v", w)
	return p, w
}

// Run simulates one seed. Workload "sql" runs RunSQL.
func Run(seed uint64, cfg Config) (res Result) {
	if cfg.Workload == "sql" {
		return RunSQL(seed, cfg)
	}
	res.Seed = seed
	r := rand.New(rand.NewPCG(seed, 0x9e3779b97f4a7c15))
	p, w := draw(seed, r, cfg)
	res.Params = p
	maxSteps := cfg.MaxSteps
	if maxSteps == 0 {
		maxSteps = 5_000_000
	}
	child := func() *rand.Rand { return rand.New(rand.NewPCG(r.Uint64(), r.Uint64())) }

	s := &sched{}
	d := &disk{s: s, r: child(), latency: p.Latency}
	rec := check.NewRecorder()
	schemas := w.Schemas()
	open := func() *txn.DB {
		o := txn.Options{Mode: p.Mode, BucketBits: p.BucketBits, Yield: s.Yield, Wait: s.Wait, NoInvariants: cfg.NoInvariants}
		if p.Durable {
			o.Store, o.NoSync = d, p.NoSync
			if p.CommitDelay > 0 {
				o.CommitDelay = func() { env{s}.Sleep(p.CommitDelay) }
			}
		}
		return txn.Open(o, schemas...)
	}
	db := open()

	var cos []*co
	defer func() { stopAll(cos) }()
	defer func() {
		if x := recover(); x != nil {
			res.Err = fmt.Errorf("panic: %v\n%s", x, debug.Stack())
		}
	}()
	startWriter := func() {
		if p.Durable {
			c := s.spawn(db.RunWriter)
			c.daemon = true
			cos = append(cos, c)
		}
	}

	clients := make([]*workload.Client, p.Clients)
	weights := make([]int, p.Clients)
	for i := range clients {
		clients[i] = &workload.Client{ID: i + 1, Rec: rec, Rand: child(), Env: env{s}, Quota: p.Quota}
		weights[i] = 1
		if p.Skew {
			weights[i] = 1 << r.IntN(6)
		}
	}
	startClients := func() {
		for i, c := range clients {
			if c.Done < c.Quota {
				c.DB = db
				co := s.spawn(func() error { return workload.Run(w, c) })
				co.weight = weights[i]
				cos = append(cos, co)
			}
		}
	}

	// crash stops every coroutine, drops the disk's unsynced suffix,
	// recovers a new DB from what survived, checks it against the history,
	// and restarts the writer and the unfinished clients.
	crash := func() error {
		res.Crashes++
		stopAll(cos)
		cos = nil
		db.Close()
		last := d.crash()
		if err := rec.Truncate(last, !p.NoSync); err != nil {
			return err
		}
		db = open()
		if err := db.Recover(last, d.load); err != nil {
			return err
		}
		if err := check.Replay(schemas, rec.Logs(), last, db.Dump); err != nil {
			return fmt.Errorf("recovered state after crash %d: %w", res.Crashes, err)
		}
		startWriter()
		startClients()
		return nil
	}

	runnable := make([]*co, 0, p.Clients+2)
	loop := func(crashes bool) error {
		for {
			runnable = runnable[:0]
			wake, live, total := int64(-1), false, 0
			for _, c := range cos {
				if c.done {
					continue
				}
				live = live || !c.daemon
				switch {
				case c.wake > s.now:
					if wake < 0 || c.wake < wake {
						wake = c.wake
					}
				case c.blocked == nil || c.blocked():
					runnable = append(runnable, c)
					total += c.weight
				}
			}
			if !live {
				return nil
			}
			if len(runnable) == 0 {
				if wake < 0 {
					return errors.New("deadlock: every coroutine is blocked")
				}
				s.now = wake
				continue
			}
			var c *co
			n := r.IntN(total)
			for _, c = range runnable {
				if n < c.weight {
					break
				}
				n -= c.weight
			}
			s.cur = c
			_, ok := c.next()
			s.cur = nil
			if !ok {
				c.done = true
				switch {
				case errors.Is(c.err, errSyncFailed) && crashes:
					if err := crash(); err != nil {
						return err
					}
				case c.err != nil:
					return c.err
				}
			}
			s.now++
			res.Steps++
			if res.Steps > maxSteps {
				return fmt.Errorf("liveness: clients unfinished after %d steps", maxSteps)
			}
			if !cfg.NoInvariants && res.Steps%1000 == 0 {
				if err := db.CheckInvariants(); err != nil {
					return err
				}
			}
			if crashes && r.Float64() < p.CrashP {
				if err := crash(); err != nil {
					return err
				}
			}
		}
	}

	startWriter()
	loader := &workload.Client{DB: db, Rec: rec, Rand: child(), Env: env{s}}
	cos = append(cos, s.spawn(func() error { return w.Load(loader) }))
	if res.Err = loop(false); res.Err != nil {
		return res
	}
	if p.Durable {
		// Even without sync acknowledgments, the initial data must survive.
		cos = append(cos, s.spawn(func() error {
			s.Wait(func() bool { return db.DurableTs() >= db.LastCommitTs() })
			return nil
		}))
		if res.Err = loop(false); res.Err != nil {
			return res
		}
	}
	d.failP = p.SyncFailP
	startClients()
	if res.Err = loop(true); res.Err != nil {
		return res
	}

	for _, c := range clients {
		res.Stats.Add(c.Stats)
	}
	if !cfg.NoInvariants {
		if res.Err = db.CheckInvariants(); res.Err != nil {
			return res
		}
	}
	logs := rec.Logs()
	res.Logs = len(logs)
	res.Trace = rec.Trace() ^ uint64(res.Steps)
	res.Err = check.Replay(schemas, logs, db.LastCommitTs(), db.Dump)
	return res
}
