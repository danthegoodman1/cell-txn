package txn

import "sort"

// Txn is a transaction. A Txn is used by one goroutine at a time.
type Txn struct {
	db     *DB
	readTs uint64
	serial uint64
	done   bool

	reads []read // first-read order, for deterministic validation
	rpos  map[rowKey]int
	scans []scanRange

	writes []wslot // first-write order, for deterministic commits
	wpos   map[rowKey]int
	undo   []undoRec

	// While a statement is open, reads see this transaction's writes as
	// they stood when it began: stmtPrev holds each slot's write from
	// before the statement first changed it. Write-path checks (existence,
	// uniqueness) still see every write.
	stmt     int
	stmtPrev map[int]*write

	// OnInstall, when set, runs under the commit lock once the commit is
	// certain, before it is visible or handed to the writer. Recorders use
	// it so no commit can reach the disk ahead of its log.
	OnInstall func(ts uint64)
}

type rowKey struct {
	tbl int
	pk  string
}

// read is a recorded read. n caches the row's node when it existed at read
// time, so validation skips the skiplist search; nodes are never removed.
type read struct {
	k    rowKey
	n    *node[chain]
	mask ColMask
}

// scanRange is a scanned range of bucket keys; hi == "" is unbounded.
type scanRange struct {
	tbl    int
	lo, hi string
}

type wslot struct {
	k rowKey
	n *node[chain] // the row's node, when it existed at write time
	w *write       // nil when the row has no pending change
}

type undoRec struct {
	i    int
	prev *write
}

type wkind uint8

const (
	wUpdate wkind = iota + 1
	wInsert
	wDelete
)

// write is a pending change to one row. It is replaced, never mutated, so
// undo records can hold old versions.
type write struct {
	kind    wkind
	existed bool    // the row existed in the snapshot
	set     ColMask // update: columns assigned
	delta   ColMask // update: columns with pending deltas
	vals    []Value // update: assigned values; insert: the row
	deltas  []int64 // update: pending delta sums
}

func (w *write) clone() *write {
	c := *w
	c.vals = append([]Value(nil), w.vals...)
	c.deltas = append([]int64(nil), w.deltas...)
	return &c
}

// ReadTs returns the snapshot timestamp.
func (t *Txn) ReadTs() uint64 { return t.readTs }

// SerialTs returns the transaction's serialization point after a successful
// Commit: its commitTs, or its readTs if it wrote nothing.
func (t *Txn) SerialTs() uint64 { return t.serial }

// Wrote reports whether the committed transaction installed writes.
func (t *Txn) Wrote() bool { return t.serial != 0 && t.serial != t.readTs }

func (t *Txn) table(tbl int) *table { return t.db.table(tbl) }

func (t *Txn) record(k rowKey, n *node[chain], mask ColMask) {
	if t.db.opts.Mode == Row {
		mask = t.table(k.tbl).all | Exists
	}
	if i, ok := t.rpos[k]; ok {
		t.reads[i].mask |= mask
		if t.reads[i].n == nil {
			t.reads[i].n = n
		}
		return
	}
	if t.rpos == nil {
		t.rpos = map[rowKey]int{}
	}
	t.rpos[k] = len(t.reads)
	t.reads = append(t.reads, read{k, n, mask})
}

func (t *Txn) pending(k rowKey) *write {
	if i, ok := t.wpos[k]; ok {
		return t.writes[i].w
	}
	return nil
}

// seen returns the write in slot i that reads see.
func (t *Txn) seen(i int) *write {
	if w, ok := t.stmtPrev[i]; ok {
		return w
	}
	return t.writes[i].w
}

// seenPending returns the pending write for k that reads see.
func (t *Txn) seenPending(k rowKey) *write {
	if i, ok := t.wpos[k]; ok {
		return t.seen(i)
	}
	return nil
}

// BeginStatement opens a statement: until the matching EndStatement, reads
// see the transaction's writes as of now. Statements may nest; the
// outermost one sets the view.
func (t *Txn) BeginStatement() {
	if t.stmt == 0 {
		t.stmtPrev = nil
	}
	t.stmt++
}

// EndStatement closes a statement opened by BeginStatement.
func (t *Txn) EndStatement() {
	if t.stmt > 0 {
		if t.stmt--; t.stmt == 0 {
			t.stmtPrev = nil
		}
	}
}

func (t *Txn) put(k rowKey, n *node[chain], w *write) {
	i, ok := t.wpos[k]
	if !ok {
		if t.wpos == nil {
			t.wpos = map[rowKey]int{}
		}
		i = len(t.writes)
		t.wpos[k] = i
		t.writes = append(t.writes, wslot{k: k})
	}
	if t.writes[i].n == nil {
		t.writes[i].n = n
	}
	if t.stmt > 0 {
		if _, ok := t.stmtPrev[i]; !ok {
			if t.stmtPrev == nil {
				t.stmtPrev = map[int]*write{}
			}
			t.stmtPrev[i] = t.writes[i].w
		}
	}
	t.undo = append(t.undo, undoRec{i, t.writes[i].w})
	t.writes[i].w = w
}

// snapshot returns the committed row visible at readTs, or nil.
func (t *Txn) snapshot(n *node[chain]) []Value {
	if v := visible(n, t.readTs); v != nil {
		return v.row
	}
	return nil
}

// view returns the row as this transaction sees it, full width, or nil if
// it doesn't exist. The result may alias committed data; callers copy.
func (t *Txn) view(n *node[chain], w *write) ([]Value, error) {
	if w == nil {
		return t.snapshot(n), nil
	}
	switch w.kind {
	case wInsert:
		return w.vals, nil
	case wDelete:
		return nil, nil
	}
	base := t.snapshot(n)
	t.db.invariant(base != nil, "pending update on a row missing from the snapshot")
	row := make([]Value, len(w.vals))
	copy(row, base)
	w.set.Each(func(c int) { row[c] = w.vals[c] })
	var err error
	w.delta.Each(func(c int) {
		if err == nil {
			row[c], err = addValue(row[c], w.deltas[c])
		}
	})
	return row, err
}

// readMask returns the columns a read of cols depends on, given the
// pending write: columns this transaction assigned are already final.
func readMask(cols ColMask, w *write) ColMask {
	switch {
	case w == nil:
		return cols | Exists
	case w.kind != wUpdate:
		return 0
	case bugDeltaReadUntracked:
		return (cols &^ (w.set | w.delta)) | Exists
	default:
		return (cols &^ w.set) | Exists
	}
}

func project(row []Value, cols ColMask) []Value {
	out := make([]Value, len(row))
	cols.Each(func(c int) { out[c] = row[c] })
	return out
}

func (t *Txn) checkCols(tbl int, cols ColMask) {
	if cols&^t.table(tbl).all != 0 {
		panic(errorf("table %d has no columns %#x", tbl, uint64(cols&^t.table(tbl).all)))
	}
}

func inRange(k, lo, hi string) bool { return k >= lo && (hi == "" || k < hi) }

// Get reads cols of a row. The returned slice is full width with only cols
// filled. It records a read of cols and Exists; a missing row records an
// absent-key read.
func (t *Txn) Get(tbl int, pk string, cols ColMask) ([]Value, bool, error) {
	return t.get(tbl, pk, cols, true, false, false)
}

// GetUntracked reads cols from the transaction's view without recording a
// read.
func (t *Txn) GetUntracked(tbl int, pk string, cols ColMask) ([]Value, bool, error) {
	return t.get(tbl, pk, cols, false, false, false)
}

// GetRow returns the whole row but records a read of only track and
// Exists. The caller must not let untracked columns influence anything it
// writes or returns.
func (t *Txn) GetRow(tbl int, pk string, track ColMask) ([]Value, bool, error) {
	return t.get(tbl, pk, track, true, true, false)
}

// CurrentRow is GetRow including the open statement's own writes, for
// write-path lookups such as the row a duplicate key collided with.
func (t *Txn) CurrentRow(tbl int, pk string, track ColMask) ([]Value, bool, error) {
	return t.get(tbl, pk, track, true, true, true)
}

func (t *Txn) get(tbl int, pk string, cols ColMask, track, full, current bool) ([]Value, bool, error) {
	if t.done {
		return nil, false, ErrDone
	}
	cols &^= Exists
	t.checkCols(tbl, cols)
	k := rowKey{tbl, pk}
	w := t.seenPending(k)
	if current {
		w = t.pending(k)
	}
	n := t.table(tbl).rows.get(pk, t.db.opts.Yield)
	row, err := t.view(n, w)
	if err != nil {
		return nil, false, err
	}
	if track {
		switch {
		case row != nil:
			if m := readMask(cols, w); m != 0 {
				t.record(k, n, m)
			}
		case w == nil && !bugNoAbsentRead:
			t.record(k, n, Exists)
		}
	}
	switch {
	case row == nil:
		return nil, false, nil
	case full:
		return append([]Value(nil), row...), true, nil
	}
	return project(row, cols), true, nil
}

// scanSpec is what a scan records and returns.
type scanSpec struct {
	pred, proj ColMask
	match      func(pk string, row []Value) bool
	full       bool // return whole rows
}

// visit records a row a scan reached and reports it if it matches.
func (t *Txn) visit(k rowKey, n *node[chain], w *write, row []Value, sp scanSpec, out []Rec) []Rec {
	if m := readMask(sp.pred, w); m != 0 {
		t.record(k, n, m)
	}
	if sp.match == nil || sp.match(k.pk, project(row, sp.pred)) {
		if m := readMask(sp.proj, w); m != 0 {
			t.record(k, n, m)
		}
		if sp.full {
			out = append(out, Rec{k.pk, append([]Value(nil), row...)})
		} else {
			out = append(out, Rec{k.pk, project(row, sp.pred|sp.proj)})
		}
	}
	return out
}

// own returns this transaction's pending writes to tbl, as candidates
// sorted by key(view).
type ownRow struct {
	key string
	s   wslot
	row []Value
}

func (t *Txn) own(tbl int, key func(pk string, row []Value) string, lo, hi string) ([]ownRow, error) {
	var out []ownRow
	for i, s := range t.writes {
		if s.w = t.seen(i); s.k.tbl != tbl || s.w == nil {
			continue
		}
		row, err := t.view(s.n, s.w)
		if err != nil {
			return nil, err
		}
		if row == nil {
			continue
		}
		if k := key(s.k.pk, row); inRange(k, lo, hi) {
			out = append(out, ownRow{k, s, row})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].key < out[j].key })
	return out, nil
}

func (t *Txn) recordRange(tb *table, lo, hi string) {
	bits := t.db.opts.BucketBits
	r := scanRange{tbl: tb.id, lo: tb.bucketOf(lo, bits)}
	if hi != "" {
		r.hi = tb.bucketOf(hi, bits)
	}
	t.scans = append(t.scans, r)
}

// Scan visits every row with key in [lo, hi) in key order; hi == "" is
// unbounded. match sees each row with only pred filled and reports whether
// it matches. Scan returns the matches with pred and proj filled. It
// records every bucket overlapping the range, pred and Exists for every row
// visited, and proj for every match.
func (t *Txn) Scan(tbl int, lo, hi string, pred, proj ColMask, match func(pk string, row []Value) bool) ([]Rec, error) {
	return t.scan(tbl, lo, hi, scanSpec{pred: pred, proj: proj, match: match})
}

// ScanRows returns whole rows in [lo, hi) of the primary key (idx < 0) or
// of secondary index idx, recording reads of only track, Exists and the
// indexed columns. Like GetRow, untracked columns must not influence
// anything the caller writes or returns.
func (t *Txn) ScanRows(tbl, idx int, lo, hi string, track ColMask) ([]Rec, error) {
	sp := scanSpec{pred: track, full: true}
	if idx < 0 {
		return t.scan(tbl, lo, hi, sp)
	}
	return t.indexScan(tbl, idx, lo, hi, sp)
}

func (t *Txn) scan(tbl int, lo, hi string, sp scanSpec) ([]Rec, error) {
	if t.done {
		return nil, ErrDone
	}
	sp.pred &^= Exists
	sp.proj &^= Exists
	t.checkCols(tbl, sp.pred|sp.proj)
	tb := t.table(tbl)
	t.recordRange(tb, lo, hi)
	own, err := t.own(tbl, func(pk string, _ []Value) string { return pk }, lo, hi)
	if err != nil {
		return nil, err
	}
	var out []Rec
	n := tb.rows.seek(lo, nil, t.db.opts.Yield)
	for {
		for n != nil && inRange(n.key, lo, hi) && t.seenPending(rowKey{tbl, n.key}) != nil {
			n = n.next[0].Load() // own writes come from the own list
		}
		var k rowKey
		var nd *node[chain]
		var w *write
		var row []Value
		switch {
		case n != nil && inRange(n.key, lo, hi) && (len(own) == 0 || n.key < own[0].key):
			k, nd, row = rowKey{tbl, n.key}, n, t.snapshot(n)
			n = n.next[0].Load()
		case len(own) > 0:
			k, nd, w, row = own[0].s.k, own[0].s.n, own[0].s.w, own[0].row
			own = own[1:]
		default:
			return out, nil
		}
		if row != nil {
			out = t.visit(k, nd, w, row, sp, out)
		}
		t.db.point()
	}
}

// IndexScan visits the rows whose entries in secondary index idx fall in
// [lo, hi), in entry order, with the same recording as Scan plus a read of
// the indexed columns, whose values place each row in the range.
func (t *Txn) IndexScan(tbl, idx int, lo, hi string, pred, proj ColMask, match func(pk string, row []Value) bool) ([]Rec, error) {
	return t.indexScan(tbl, idx, lo, hi, scanSpec{pred: pred, proj: proj, match: match})
}

func (t *Txn) indexScan(tbl, idx int, lo, hi string, sp scanSpec) ([]Rec, error) {
	if t.done {
		return nil, ErrDone
	}
	sp.pred &^= Exists
	sp.proj &^= Exists
	t.checkCols(tbl, sp.pred|sp.proj)
	tb := t.table(tbl)
	ix := tb.indexes()[idx]
	if !bugIndexNoRange {
		t.recordRange(ix.tbl, lo, hi)
	}
	own, err := t.own(tbl, func(pk string, row []Value) string { k, _ := ix.entryKey(row, pk); return k }, lo, hi)
	if err != nil {
		return nil, err
	}
	sp.pred |= ix.def.Cols
	var out []Rec
	e := ix.tbl.rows.seek(lo, nil, t.db.opts.Yield)
	for {
		var ent *version
		for ; e != nil && inRange(e.key, lo, hi); e = e.next[0].Load() {
			if ent = visible(e, t.readTs); ent != nil && ent.row != nil && t.seenPending(rowKey{tbl, ent.row[0].(string)}) == nil {
				break
			}
			ent = nil // missing at readTs, or a row with own writes, which come from the own list
		}
		var k rowKey
		var nd *node[chain]
		var w *write
		var row []Value
		switch {
		case ent != nil && (len(own) == 0 || e.key < own[0].key):
			k = rowKey{tbl, ent.row[0].(string)}
			nd = tb.rows.get(k.pk, t.db.opts.Yield)
			row = t.snapshot(nd)
			t.db.invariant(row != nil, "index %s entry %x has no row", ix.tbl.schema.Name, e.key)
			e = e.next[0].Load()
		case len(own) > 0:
			k, nd, w, row = own[0].s.k, own[0].s.n, own[0].s.w, own[0].row
			own = own[1:]
		default:
			return out, nil
		}
		if row != nil {
			out = t.visit(k, nd, w, row, sp, out)
		}
		t.db.point()
	}
}

// existing looks up a row for a write. It records the read of Exists the
// write depends on and returns the pending write, the row's node and
// whether the row exists.
func (t *Txn) existing(k rowKey) (*write, *node[chain], bool) {
	n := t.table(k.tbl).rows.get(k.pk, t.db.opts.Yield)
	if w := t.pending(k); w != nil {
		return w, n, w.kind != wDelete
	}
	t.record(k, n, Exists)
	return nil, n, t.snapshot(n) != nil
}

func (t *Txn) existingForSet(k rowKey) (*write, *node[chain], bool) {
	if !bugSetNoExists {
		return t.existing(k)
	}
	n := t.table(k.tbl).rows.get(k.pk, t.db.opts.Yield)
	if w := t.pending(k); w != nil {
		return w, n, w.kind != wDelete
	}
	return nil, n, t.snapshot(n) != nil
}

// checkUnique fails if row, this transaction's new view of pk, would
// duplicate a key in a unique index over the written columns. It records
// a read of each claimed entry, so a concurrent claim conflicts.
func (t *Txn) checkUnique(tb *table, pk string, row []Value, written ColMask) error {
	for _, ix := range tb.indexes() {
		if !ix.def.Unique || ix.def.Cols&written == 0 {
			continue
		}
		k, u := ix.def.Key(row)
		if !u {
			continue
		}
		en := ix.tbl.rows.get(k, t.db.opts.Yield)
		if !bugUniqueNoRead {
			t.record(rowKey{ix.tbl.id, k}, en, Exists)
		}
		owner := ""
		if v := visible(en, t.readTs); v != nil && v.row != nil {
			if o := v.row[0].(string); o != pk && t.pending(rowKey{tb.id, o}) == nil {
				owner = o
			}
		}
		for _, s := range t.writes {
			if owner != "" {
				break
			}
			if s.k.tbl != tb.id || s.k.pk == pk || s.w == nil {
				continue
			}
			if r, err := t.view(s.n, s.w); err == nil && r != nil {
				if k2, u2 := ix.def.Key(r); u2 && k2 == k {
					owner = s.k.pk
				}
			}
		}
		if owner != "" {
			return &DuplicateKeyError{Table: tb.id, Index: ix.def.Name, Key: k, Owner: owner}
		}
	}
	return nil
}

// update applies fn to a copy of the pending update for an existing row and
// installs it after checking unique indexes over written.
func (t *Txn) update(tbl int, pk string, written ColMask, fn func(w *write) error) error {
	if t.done {
		return ErrDone
	}
	tb := t.table(tbl)
	k := rowKey{tbl, pk}
	w, n, ok := t.existingForSet(k)
	if !ok {
		return ErrNotFound
	}
	if w == nil {
		w = &write{kind: wUpdate, existed: true, vals: make([]Value, tb.width), deltas: make([]int64, tb.width)}
	} else {
		w = w.clone()
	}
	if err := fn(w); err != nil {
		return err
	}
	if written&tb.indexed() != 0 {
		row, err := t.view(n, w)
		if err != nil {
			return err
		}
		if err := t.checkUnique(tb, pk, row, written); err != nil {
			return err
		}
	}
	t.put(k, n, w)
	return nil
}

// Set assigns cols of an existing row from the full-width vals. It records
// a read of Exists only.
func (t *Txn) Set(tbl int, pk string, cols ColMask, vals []Value) error {
	cols &^= Exists
	t.checkCols(tbl, cols)
	if len(vals) != t.table(tbl).width {
		panic(errorf("table %d vals have %d columns, want %d", tbl, len(vals), t.table(tbl).width))
	}
	return t.update(tbl, pk, cols, func(w *write) error {
		if w.kind == wUpdate {
			// Overwriting a pending delta makes the statements that
			// checked its intermediate value depend on the base value.
			if superseded := w.delta & cols; superseded != 0 {
				t.record(rowKey{tbl, pk}, t.table(tbl).rows.get(pk, t.db.opts.Yield), superseded)
			}
			w.set |= cols
			w.delta &^= cols
		}
		cols.Each(func(c int) { w.vals[c] = vals[c] })
		return nil
	})
}

// Add adds delta to an integer column of an existing row. In CellDelta mode
// it records only a read of Exists, so concurrent Adds commute; in the other
// modes, and on indexed columns, it also reads the column.
func (t *Txn) Add(tbl int, pk string, col int, delta int64) error {
	t.checkCols(tbl, Col(col))
	tb := t.table(tbl)
	return t.update(tbl, pk, Col(col), func(w *write) error {
		if w.kind == wUpdate && w.set&Col(col) == 0 {
			if t.db.opts.Mode != CellDelta || tb.indexed()&Col(col) != 0 {
				t.record(rowKey{tbl, pk}, tb.rows.get(pk, t.db.opts.Yield), Col(col))
			}
			s, ok := addDelta(w.deltas[col], delta)
			if !ok {
				return ErrOverflow
			}
			w.delta |= Col(col)
			w.deltas[col] = s
			return nil
		}
		v, err := addValue(w.vals[col], delta)
		w.vals[col] = v
		return err
	})
}

// Insert creates a row. It records a read of Exists, so a concurrent
// insert of the same key conflicts.
func (t *Txn) Insert(tbl int, pk string, row []Value) error {
	if t.done {
		return ErrDone
	}
	tb := t.table(tbl)
	if len(row) != tb.width {
		panic(errorf("table %d row has %d columns, want %d", tbl, len(row), tb.width))
	}
	k := rowKey{tbl, pk}
	w, n, ok := t.existing(k)
	if ok {
		return &DuplicateKeyError{Table: tbl, Index: "PRIMARY", Key: pk, Owner: pk}
	}
	row = append([]Value(nil), row...)
	if err := t.checkUnique(tb, pk, row, tb.all); err != nil {
		return err
	}
	t.put(k, n, &write{kind: wInsert, existed: w != nil && w.existed, vals: row})
	return nil
}

// Delete removes an existing row.
func (t *Txn) Delete(tbl int, pk string) error {
	if t.done {
		return ErrDone
	}
	k := rowKey{tbl, pk}
	w, n, ok := t.existing(k)
	if !ok {
		return ErrNotFound
	}
	if w != nil && w.kind == wInsert && !w.existed {
		t.put(k, n, nil)
		return nil
	}
	if w != nil && w.kind == wUpdate && w.delta != 0 {
		// Deleting the row discards pending deltas; the statements that
		// checked their intermediate values depend on the base value.
		t.record(k, n, w.delta)
	}
	t.put(k, n, &write{kind: wDelete, existed: true})
	return nil
}

// Savepoint marks the current write state for RollbackTo.
func (t *Txn) Savepoint() int { return len(t.undo) }

// RollbackTo discards writes made after the savepoint. Recorded reads stay,
// since the caller may have used their results.
func (t *Txn) RollbackTo(sp int) {
	for len(t.undo) > sp {
		u := t.undo[len(t.undo)-1]
		t.undo = t.undo[:len(t.undo)-1]
		t.writes[u.i].w = u.prev
	}
}

// Abort ends the transaction without installing anything.
func (t *Txn) Abort() {
	if !t.done {
		t.done = true
		t.db.readers.end(t.readTs)
	}
}
