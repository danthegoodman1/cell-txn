package txn

import (
	"errors"
	"sync"
)

// Store persists committed base-table writes. The writer calls Write and
// then Sync with batches in commit order; neither runs under the commit
// lock. A crash may lose a suffix of the commits written since the last
// Sync, but never part of a commit or a commit before a surviving one.
type Store interface {
	Write(batch []Commit) error
	Sync() error
}

// Commit is one transaction's base-table writes at its commit timestamp.
// Index entries are derived from the rows on recovery.
type Commit struct {
	Ts     uint64
	Writes []Write
}

// Write is one row image; Row is nil for a delete.
type Write struct {
	Table int
	Key   string
	Row   []Value
}

// durability is the commit queue between the committer and the writer.
type durability struct {
	mu      sync.Mutex
	cond    *sync.Cond
	queue   []Commit
	durable uint64 // every commit at or below is synced
	closed  bool
	err     error // a failed write or sync; the process must stop
}

// ErrClosed is returned by waits on a closed or failed DB.
var ErrClosed = errors.New("txn: database closed")

func (db *DB) initDurability() {
	db.dur.cond = sync.NewCond(&db.dur.mu)
}

// wait blocks until cond holds, through Options.Wait when set. cond runs
// under the durability mutex.
func (db *DB) wait(cond func() bool) {
	d := &db.dur
	if db.opts.Wait != nil {
		db.opts.Wait(func() bool {
			d.mu.Lock()
			defer d.mu.Unlock()
			return cond()
		})
		return
	}
	d.mu.Lock()
	for !cond() {
		d.cond.Wait()
	}
	d.mu.Unlock()
}

// enqueue hands a commit to the writer. It runs under the commit lock, so
// the queue is in commit order.
func (db *DB) enqueue(c Commit) {
	d := &db.dur
	d.mu.Lock()
	d.queue = append(d.queue, c)
	d.mu.Unlock()
	d.cond.Broadcast()
}

// waitDurable blocks until every commit at or below ts is synced. It
// returns immediately without a Store or with NoSync.
func (db *DB) waitDurable(ts uint64) error {
	if db.opts.Store == nil || db.opts.NoSync || bugAckBeforeSync {
		return nil
	}
	var err error
	db.wait(func() bool {
		if db.dur.err != nil || db.dur.closed {
			err = ErrClosed
			return true
		}
		return db.dur.durable >= ts
	})
	return err
}

// DurableTs returns the newest synced commit timestamp.
func (db *DB) DurableTs() uint64 {
	db.dur.mu.Lock()
	defer db.dur.mu.Unlock()
	return db.dur.durable
}

// WaitWork blocks until the writer has commits to flush or the DB closes.
// It reports whether to keep running.
func (db *DB) WaitWork() bool {
	open := true
	db.wait(func() bool {
		open = !db.dur.closed && db.dur.err == nil
		return !open || len(db.dur.queue) > 0
	})
	return open
}

// Flush writes and syncs every queued commit, then advances DurableTs. A
// write or sync error is permanent: the DB stops acknowledging commits and
// the process must restart and recover.
func (db *DB) Flush() error {
	d := &db.dur
	d.mu.Lock()
	batch := d.queue
	d.queue = nil
	d.mu.Unlock()
	if len(batch) == 0 {
		return nil
	}
	err := db.opts.Store.Write(batch)
	if err == nil {
		err = db.opts.Store.Sync()
	}
	d.mu.Lock()
	if err != nil {
		d.err = err
	} else {
		d.durable = batch[len(batch)-1].Ts
	}
	d.mu.Unlock()
	d.cond.Broadcast()
	return err
}

// RunWriter flushes until the DB closes or a flush fails. Hosts run it on
// its own goroutine; the simulator runs it as a coroutine.
func (db *DB) RunWriter() error {
	for db.WaitWork() {
		if err := db.Flush(); err != nil {
			return err
		}
	}
	return nil
}

// Close stops the writer and fails pending waits.
func (db *DB) Close() {
	db.dur.mu.Lock()
	db.dur.closed = true
	db.dur.mu.Unlock()
	db.dur.cond.Broadcast()
}

// Recover loads the newest image of every row, as of commit ts last, into
// an empty DB: fn supplies the rows. Index entries are rebuilt from the
// rows, and each table's auto-increment and hidden-key counters resume
// past its largest 8-byte key.
func (db *DB) Recover(last uint64, load func(put func(tbl int, key string, row []Value)) error) error {
	if db.last.Load() != 0 {
		return errorf("recover into a non-empty database")
	}
	err := load(func(tbl int, key string, row []Value) {
		t := db.table(tbl)
		v := &version{ts: last, mask: t.all | Exists, row: row}
		t.rows.getOrInsert(key).val.head.Store(v)
		for _, ix := range t.indexes() {
			k, _ := ix.entryKey(row, key)
			e := &version{ts: last, mask: ix.tbl.all | Exists, row: []Value{key}}
			ix.tbl.rows.getOrInsert(k).val.head.Store(e)
		}
		if len(key) == 8 {
			if id := uint64(key[0])<<56 | uint64(key[1])<<48 | uint64(key[2])<<40 | uint64(key[3])<<32 |
				uint64(key[4])<<24 | uint64(key[5])<<16 | uint64(key[6])<<8 | uint64(key[7]); id > t.nextID.Load() {
				t.nextID.Store(id)
				t.hidden.Store(id)
			}
		}
	})
	if err != nil {
		return err
	}
	db.last.Store(last)
	db.dur.durable = last
	return nil
}
