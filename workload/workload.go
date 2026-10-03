// Package workload defines transaction mixes as straight-line Go against a
// Client. The simulator runs each client as a coroutine; the bench and the
// stress tests run each on its own goroutine.
package workload

import (
	"errors"
	"fmt"
	"math/rand/v2"

	"cell-tnx/check"
	"cell-tnx/txn"
)

// Env supplies scheduling and time. Ticks are simulated steps in the
// simulator and microseconds elsewhere.
type Env interface {
	Yield()
	Sleep(ticks int64)
	Now() int64
	Stopped() bool
}

// Workload is a transaction mix.
type Workload interface {
	Name() string
	Schemas() []txn.Schema
	// Load inserts the initial data.
	Load(c *Client) error
	// Step runs one logical transaction, retrying conflicts. It returns an
	// error only for an unexpected failure or a violated invariant.
	Step(c *Client) error
}

// Client is one session issuing transactions.
type Client struct {
	ID    int
	DB    *txn.DB
	Rec   *check.Recorder
	Rand  *rand.Rand
	Env   Env
	Quota int // logical transactions to run; 0 runs until Env.Stopped
	Done  int // logical transactions finished
	Stats Stats
	// OnDone, when set, observes each logical transaction.
	OnDone func(latency int64, retries int, err error)
	seq    int64
}

// Stats counts outcomes.
type Stats struct {
	Commits    int
	Rejected   int // CHECK or overflow at commit
	UserAborts int // the transaction chose to abort
	Conflicts  int // retried after a conflict
	RowConf    int
	BucketConf int
}

// Add accumulates s2 into s.
func (s *Stats) Add(s2 Stats) {
	s.Commits += s2.Commits
	s.Rejected += s2.Rejected
	s.UserAborts += s2.UserAborts
	s.Conflicts += s2.Conflicts
	s.RowConf += s2.RowConf
	s.BucketConf += s2.BucketConf
}

// Run executes the workload until the quota is met or the env stops.
func Run(w Workload, c *Client) error {
	for c.Quota == 0 || c.Done < c.Quota {
		if c.Env.Stopped() {
			return nil
		}
		if err := w.Step(c); err != nil {
			return fmt.Errorf("client %d: %w", c.ID, err)
		}
		c.Done++
	}
	return nil
}

// Uniq returns a value no other client or call produces.
func (c *Client) Uniq() int64 {
	c.seq++
	return int64(c.ID+1)<<32 | c.seq
}

// errAbort is returned by a transaction body that chooses to abort.
var errAbort = errors.New("workload: transaction aborted by client")

// Do runs fn in a transaction, committing if fn succeeds and retrying
// conflicts with randomized exponential backoff.
func (c *Client) Do(fn func(tx *check.Tx) error) error {
	start := c.Env.Now()
	backoff := int64(4)
	for retries := 0; ; retries++ {
		tx := c.Rec.Begin(c.DB, c.ID, c.Env.Yield)
		err := fn(tx)
		if err == nil {
			err = tx.Commit()
		} else {
			tx.Abort()
		}
		var ce *txn.ConflictError
		if errors.As(err, &ce) {
			c.Stats.Conflicts++
			if ce.Bucket {
				c.Stats.BucketConf++
			} else {
				c.Stats.RowConf++
			}
			c.Env.Sleep(1 + c.Rand.Int64N(backoff))
			backoff = min(backoff*2, 1024)
			continue
		}
		switch {
		case err == nil:
			c.Stats.Commits++
		case errors.Is(err, txn.ErrConstraint):
			c.Stats.Rejected++
		case errors.Is(err, errAbort):
			c.Stats.UserAborts++
		}
		if c.OnDone != nil {
			c.OnDone(c.Env.Now()-start, retries, err)
		}
		return err
	}
}

// expect maps the normal outcomes of Do to nil.
func expect(err error) error {
	if err == nil || errors.Is(err, txn.ErrConstraint) || errors.Is(err, errAbort) {
		return nil
	}
	return err
}

// absent maps the normal outcomes of a write on a possibly missing row to nil.
func absent(err error) error {
	if errors.Is(err, txn.ErrNotFound) || errors.Is(err, txn.ErrDuplicateKey) {
		return nil
	}
	return err
}
