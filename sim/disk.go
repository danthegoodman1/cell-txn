package sim

import (
	"errors"
	"math/rand/v2"
	"slices"

	"cell-tnx/txn"
)

var errSyncFailed = errors.New("sim: sync failed")

// disk is a simulated txn.Store. Written commits become durable at Sync;
// a crash keeps the synced commits plus a random prefix of the rest, since
// the real store writes commits atomically and in order.
type disk struct {
	s       *sched
	r       *rand.Rand
	durable []txn.Commit
	written []txn.Commit
	latency int64   // max simulated sync time
	failP   float64 // chance a sync fails
}

func (d *disk) Write(batch []txn.Commit) error {
	d.written = append(d.written, batch...)
	return nil
}

func (d *disk) Sync() error {
	if d.latency > 0 {
		env{d.s}.Sleep(1 + d.r.Int64N(d.latency))
	}
	if d.r.Float64() < d.failP {
		return errSyncFailed
	}
	d.durable = append(d.durable, d.written...)
	d.written = nil
	return nil
}

// crash drops a random suffix of the unsynced commits and returns the
// newest surviving commit timestamp.
func (d *disk) crash() uint64 {
	d.durable = append(d.durable, d.written[:d.r.IntN(len(d.written)+1)]...)
	d.written = nil
	if len(d.durable) == 0 {
		return 0
	}
	return d.durable[len(d.durable)-1].Ts
}

// load replays the durable commits and passes each live row to put, in
// table and key order.
func (d *disk) load(put func(tbl int, key string, row []txn.Value)) error {
	rows := map[int]map[string][]txn.Value{}
	for _, c := range d.durable {
		for _, w := range c.Writes {
			if rows[w.Table] == nil {
				rows[w.Table] = map[string][]txn.Value{}
			}
			if w.Row == nil {
				delete(rows[w.Table], w.Key)
			} else {
				rows[w.Table][w.Key] = w.Row
			}
		}
	}
	var tables []int
	for t := range rows {
		tables = append(tables, t)
	}
	slices.Sort(tables)
	for _, t := range tables {
		var keys []string
		for k := range rows[t] {
			keys = append(keys, k)
		}
		slices.Sort(keys)
		for _, k := range keys {
			put(t, k, rows[t][k])
		}
	}
	return nil
}
