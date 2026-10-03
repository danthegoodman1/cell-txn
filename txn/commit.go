package txn

// Commit installs the transaction and waits until it is durable. Errors
// match ErrConflict or ErrConstraint under errors.Is.
func (t *Txn) Commit() error {
	if err := t.Install(); err != nil {
		return err
	}
	return t.WaitDurable()
}

// WaitDurable blocks until the transaction's serialization point is
// durable, so nothing it observed or wrote can be lost to a crash.
func (t *Txn) WaitDurable() error {
	if bugReadOnlyNoWait && !t.Wrote() {
		return nil
	}
	return t.db.waitDurable(t.serial)
}

// Install validates the transaction's reads, merges its writes into the
// newest committed rows, derives index entries from the merged rows, and
// installs everything at a new timestamp, visible to new snapshots. A
// transaction that wrote nothing serializes at its readTs without
// validation.
func (t *Txn) Install() error {
	if t.done {
		return ErrDone
	}
	t.done = true
	db := t.db
	defer db.readers.end(t.readTs)

	var ws []wslot
	for _, s := range t.writes {
		if s.w != nil {
			ws = append(ws, s)
		}
	}
	if len(ws) == 0 {
		t.serial = t.readTs
		return nil
	}

	timed := db.opts.Now != nil && db.opts.OnCommit != nil
	var t0, t1 int64
	if timed {
		t0 = db.opts.Now()
	}
	db.mu.Lock()
	if timed {
		t1 = db.opts.Now()
	}
	err := t.commitLocked(ws)
	if timed {
		t2 := db.opts.Now()
		db.mu.Unlock()
		db.opts.OnCommit(t1-t0, t2-t1)
	} else {
		db.mu.Unlock()
	}
	return err
}

// install is one version to append: a merged row or a derived index entry.
type install struct {
	tb     *table
	key    string
	n      *node[chain]
	v      *version
	bucket bool // an insert or delete, which changes the key's bucket
}

func (t *Txn) commitLocked(ws []wslot) error {
	db := t.db
	ts := db.last.Load() + 1
	if err := t.validate(); err != nil {
		return err
	}
	ins := make([]install, 0, len(ws))
	var entries map[rowKey]int // index entry to its position in ins
	for _, s := range ws {
		tb := db.table(s.k.tbl)
		n := s.n
		if n == nil {
			n = tb.rows.get(s.k.pk, nil)
		}
		var old []Value
		if n != nil {
			old = n.val.head.Load().row
		}
		v, err := t.merge(tb, s, old, ts)
		if err != nil {
			return err
		}
		ins = append(ins, install{tb, s.k.pk, n, v, s.w.kind != wUpdate})
		if s.w.kind == wUpdate && v.mask&tb.indexed() == 0 {
			continue // no indexed column changed, so no entry did
		}
		for _, ix := range tb.indexes() {
			var ok, nk string
			if old != nil {
				ok, _ = ix.entryKey(old, s.k.pk)
			}
			if v.row != nil {
				nk, _ = ix.entryKey(v.row, s.k.pk)
			}
			if old != nil && v.row != nil && ok == nk {
				continue
			}
			// Within one commit an entry may be both vacated and claimed;
			// the claim wins.
			if old != nil {
				ins = entry(ins, &entries, ix.tbl, ok, &version{ts: ts, mask: ix.tbl.all | Exists})
			}
			if v.row != nil {
				ins = entry(ins, &entries, ix.tbl, nk, &version{ts: ts, mask: ix.tbl.all | Exists, row: []Value{s.k.pk}})
			}
		}
	}
	if t.OnInstall != nil {
		t.OnInstall(ts)
	}
	horizon := db.readers.horizon(ts - 1)
	if bugGCEager {
		horizon = ts - 1
	}
	if db.opts.Store != nil {
		c := Commit{Ts: ts}
		for _, in := range ins {
			if !in.tb.isIndex {
				c.Writes = append(c.Writes, Write{in.tb.id, in.key, in.v.row})
			}
		}
		db.enqueue(c)
	}
	for _, in := range ins {
		n := in.n
		if n == nil {
			n = in.tb.rows.getOrInsert(in.key)
		}
		in.v.next.Store(n.val.head.Load())
		n.val.head.Store(in.v)
		trim(in.v, horizon)
		if in.bucket {
			in.tb.buckets.getOrInsert(in.tb.bucketOf(in.key, db.opts.BucketBits)).val = ts
		}
	}
	db.last.Store(ts)
	t.serial = ts
	return nil
}

func entry(ins []install, entries *map[rowKey]int, tb *table, key string, v *version) []install {
	if *entries == nil {
		*entries = map[rowKey]int{}
	}
	k := rowKey{tb.id, key}
	if i, ok := (*entries)[k]; ok {
		if v.row != nil {
			ins[i].v = v
		}
		return ins
	}
	(*entries)[k] = len(ins)
	return append(ins, install{tb, key, tb.rows.get(key, nil), v, true})
}

// validate fails if any version committed after readTs wrote a column this
// transaction read, or if any scanned bucket saw an insert or delete.
// Commits install in timestamp order under the lock, so every such version
// is already in place.
func (t *Txn) validate() error {
	db := t.db
	for _, r := range t.reads {
		n := r.n
		if n == nil {
			// The key was absent when read; a later commit may have added it.
			if n = db.table(r.k.tbl).rows.get(r.k.pk, nil); n == nil {
				continue
			}
		}
		for v := n.val.head.Load(); v != nil && v.ts > t.readTs; v = v.next.Load() {
			if c := v.mask & r.mask; c != 0 {
				return &ConflictError{Table: r.k.tbl, PK: r.k.pk, Cols: c}
			}
		}
	}
	if bugSkipBuckets {
		return nil
	}
	for _, s := range t.scans {
		if db.table(s.tbl).changedSince(s.lo, s.hi, t.readTs) {
			return &ConflictError{Table: s.tbl, Bucket: true}
		}
	}
	return nil
}

// merge builds the version a write installs at ts: an update lays its
// assigned columns and deltas over old, the newest committed row. It
// evaluates every CHECK that references a written column.
func (t *Txn) merge(tb *table, s wslot, old []Value, ts uint64) (*version, error) {
	w := s.w
	v := &version{ts: ts}
	var checks ColMask
	switch w.kind {
	case wDelete:
		v.mask = tb.all | Exists
		return v, nil
	case wInsert:
		v.mask = tb.all | Exists
		v.row = append([]Value(nil), w.vals...)
		checks = tb.all
	case wUpdate:
		t.db.invariant(old != nil, "update of missing row %x in table %s", s.k.pk, tb.schema.Name)
		row := make([]Value, tb.width)
		copy(row, old)
		w.set.Each(func(c int) { row[c] = w.vals[c] })
		var err error
		w.delta.Each(func(c int) {
			if err == nil {
				row[c], err = addValue(row[c], w.deltas[c])
			}
		})
		if err != nil {
			return nil, &ConstraintError{Table: tb.id, PK: s.k.pk, Check: err.Error(), AfterTs: ts - 1}
		}
		v.mask = w.set | w.delta
		v.row = row
		checks = v.mask
		if bugCheckSkipDelta && w.set == 0 {
			checks = 0
		}
	}
	for _, c := range tb.checks {
		if c.Cols&checks != 0 && !c.OK(v.row) {
			return nil, &ConstraintError{Table: tb.id, PK: s.k.pk, Check: c.Name, AfterTs: ts - 1}
		}
	}
	return v, nil
}

// trim drops versions that no active snapshot can see: it keeps every
// version newer than horizon plus the newest one at or below it.
func trim(v *version, horizon uint64) {
	for v != nil && v.ts > horizon {
		v = v.next.Load()
	}
	if v != nil {
		v.next.Store(nil)
	}
}
