// Package txn is a serializable optimistic transaction layer that tracks
// conflicts per (row, column), merges rows at commit, and commits
// commutative integer deltas without conflicts.
package txn

import (
	"errors"
	"fmt"
	"math/bits"
	"sync"
	"sync/atomic"
)

// Value is a column value: nil, int64, uint64, float64 or string. Values
// compare with ==.
type Value = any

// ColMask is a set of columns. Bit 63 is Exists, the row's existence.
type ColMask uint64

// Exists is the pseudo-column for row existence.
const Exists ColMask = 1 << 63

// MaxCols is the most columns a table can have.
const MaxCols = 63

// Col returns the mask for column i.
func Col(i int) ColMask { return 1 << i }

// Cols returns the mask for the given columns.
func Cols(cs ...int) ColMask {
	var m ColMask
	for _, c := range cs {
		m |= Col(c)
	}
	return m
}

// Each calls fn for every column in m, in order, excluding Exists.
func (m ColMask) Each(fn func(c int)) {
	for m &^= Exists; m != 0; m &= m - 1 {
		fn(bits.TrailingZeros64(uint64(m)))
	}
}

// Mode selects how conflicts are tracked.
type Mode uint8

const (
	// Row widens every read to the whole row and makes deltas read their
	// column. It is the row-level baseline.
	Row Mode = iota
	// Cell tracks reads per column; deltas still read their column.
	Cell
	// CellDelta tracks reads per column and lets deltas commute.
	CellDelta
)

func (m Mode) String() string {
	return [...]string{"row", "cell", "cell+delta"}[m]
}

// Schema describes a table. Primary keys are byte strings whose order is
// the key order.
type Schema struct {
	Name    string
	Cols    []string
	Checks  []Check
	Indexes []Index
	// BucketPrefix is how many leading key bytes bucketOf uses; default 8.
	BucketPrefix int
}

// Check is a CHECK constraint over the columns in Cols. OK runs under the
// commit lock, so calls never overlap.
type Check struct {
	Name string
	Cols ColMask
	OK   func(row []Value) bool
}

// Index is a secondary index. Its columns never take commuting deltas.
type Index struct {
	Name   string
	Cols   ColMask
	Unique bool
	// Key encodes the row's indexed columns as an order-preserving,
	// prefix-free key; it must read only the columns in Cols. unique=false
	// exempts the row from uniqueness, as a NULL does in MySQL.
	Key          func(row []Value) (key string, unique bool)
	BucketPrefix int
}

// Options configures a DB.
type Options struct {
	Mode Mode
	// BucketBits clears that many low bits of a key's bucket prefix, so
	// each bucket covers 2^BucketBits adjacent prefixes.
	BucketBits uint
	// Yield, when set, is called at each interleaving point. The simulator
	// uses it to switch coroutines.
	Yield func()
	// Interleave, when set, is called at fine-grained points inside
	// lock-free reads and inside commits, where the simulator may switch
	// coroutines. Commits call it while holding the commit lock, so it
	// requires Wait, which makes that lock cooperative.
	Interleave func()
	// Now and OnCommit, when both set, report commit-lock wait and hold
	// times in Now's units.
	Now      func() int64
	OnCommit func(wait, hold int64)
	// NoInvariants turns off internal assertions, so simulator self-tests
	// must catch deliberate bugs through the checker alone.
	NoInvariants bool
	// Store, when set, persists every commit. A commit is acknowledged once
	// it is synced, and a read-only commit once its snapshot is, unless
	// NoSync is set.
	Store  Store
	NoSync bool
	// CommitDelay, when set, runs on the writer before each flush. Hosts
	// sleep there, so commits arriving meanwhile share the sync: it trades
	// that much commit latency for fewer syncs.
	CommitDelay func()
	// Wait, when set, replaces blocking waits: it must return once cond
	// holds. The simulator uses it to yield instead of block.
	Wait func(cond func() bool)
}

var (
	ErrNotFound     = errors.New("txn: row not found")
	ErrDuplicateKey = errors.New("txn: duplicate key")
	ErrOverflow     = errors.New("txn: integer overflow")
	ErrNotInteger   = errors.New("txn: delta on a non-integer value")
	ErrDone         = errors.New("txn: transaction already finished")
	ErrConflict     = errors.New("txn: conflict")
	ErrConstraint   = errors.New("txn: constraint violation")
)

// ConflictError reports the read that failed validation.
type ConflictError struct {
	Table  int
	PK     string // for a row read
	Cols   ColMask
	Bucket bool // a scanned bucket changed
}

func (e *ConflictError) Error() string {
	if e.Bucket {
		return fmt.Sprintf("txn: conflict on a scanned range of table %d", e.Table)
	}
	return fmt.Sprintf("txn: conflict on table %d row %x cols %#x", e.Table, e.PK, uint64(e.Cols))
}

func (e *ConflictError) Is(target error) bool { return target == ErrConflict }

// ConstraintError reports a merged row that failed a CHECK or overflowed.
// AfterTs is the last commit the merge saw.
type ConstraintError struct {
	Table   int
	PK      string
	Check   string
	AfterTs uint64
}

func (e *ConstraintError) Error() string {
	return fmt.Sprintf("txn: table %d row %x violates %s", e.Table, e.PK, e.Check)
}

func (e *ConstraintError) Is(target error) bool { return target == ErrConstraint }

// DuplicateKeyError reports a write that would duplicate a primary key
// (Index "PRIMARY") or a unique index key held by the row Owner.
type DuplicateKeyError struct {
	Table int
	Index string
	Key   string
	Owner string
}

func (e *DuplicateKeyError) Error() string {
	return fmt.Sprintf("txn: duplicate key %x in table %d index %s", e.Key, e.Table, e.Index)
}

func (e *DuplicateKeyError) Is(target error) bool { return target == ErrDuplicateKey }

func errorf(format string, args ...any) error { return fmt.Errorf("txn: "+format, args...) }

// chain is a row's version list, newest first.
type chain struct {
	head atomic.Pointer[version]
}

// version is one committed image of a row. Everything except next is
// immutable once published; next changes only when GC trims the chain.
type version struct {
	ts   uint64
	mask ColMask // columns this commit wrote
	row  []Value // nil for a tombstone
	next atomic.Pointer[version]
}

// visible returns the newest version of n at or below ts, or nil.
func visible(n *node[chain], ts uint64) *version {
	if n == nil {
		return nil
	}
	for v := n.val.head.Load(); v != nil; v = v.next.Load() {
		if v.ts <= ts {
			return v
		}
	}
	return nil
}

type table struct {
	id      int
	schema  Schema
	width   int
	all     ColMask // every column
	rows    *skiplist[chain]
	buckets *skiplist[uint64] // bucket key to lastTs; committer only
	prefix  int
	nextID  atomic.Uint64
	hidden  atomic.Uint64 // keys for tables without a primary key
	isIndex bool
	checks  []Check // guarded by the commit lock
	// ixs holds the secondary indexes over this table; DDL replaces it.
	ixs atomic.Pointer[indexSet]
}

type indexSet struct {
	list    []*index
	indexed ColMask // columns covered by any index
}

func (t *table) indexes() []*index { return t.ixs.Load().list }
func (t *table) indexed() ColMask  { return t.ixs.Load().indexed }

// index is a secondary index, stored as a hidden table whose rows are
// [primary key] under the entry key.
type index struct {
	def Index
	tbl *table
}

// entryKey returns the index entry key for row with primary key pk.
func (ix *index) entryKey(row []Value, pk string) (key string, unique bool) {
	k, u := ix.def.Key(row)
	if ix.def.Unique && u {
		return k, true
	}
	return k + pk, false
}

// DB is an in-memory multiversion store.
type DB struct {
	opts Options
	// tables lists every table by ID, base and index alike; DDL replaces
	// the slice under the commit lock.
	tables     atomic.Pointer[[]*table]
	mu         sync.Mutex // the global commit lock
	committing bool       // the commit lock, when Wait makes it cooperative
	last       atomic.Uint64
	readers    registry
	dur        durability
}

func newTable(id int, s Schema, prefix int) *table {
	if len(s.Cols) == 0 || len(s.Cols) > MaxCols {
		panic(fmt.Sprintf("txn: table %s has %d columns", s.Name, len(s.Cols)))
	}
	if prefix == 0 {
		prefix = 8
	}
	t := &table{
		id:      id,
		schema:  s,
		width:   len(s.Cols),
		all:     Col(len(s.Cols)) - 1,
		rows:    newSkiplist[chain](),
		buckets: newSkiplist[uint64](),
		prefix:  prefix,
		checks:  s.Checks,
	}
	t.ixs.Store(&indexSet{})
	return t
}

// Open creates a DB with the given tables. Base tables take IDs 0..n-1 in
// schema order; their index tables follow.
func Open(opts Options, schemas ...Schema) *DB {
	db := &DB{opts: opts}
	db.initDurability()
	ts := []*table{}
	for i, s := range schemas {
		ts = append(ts, newTable(i, s, s.BucketPrefix))
	}
	db.tables.Store(&ts)
	for i, s := range schemas {
		for _, d := range s.Indexes {
			db.addIndex(i, d)
		}
	}
	return db
}

func (db *DB) table(i int) *table { return (*db.tables.Load())[i] }

func (db *DB) appendTable(t *table) {
	ts := append(append([]*table(nil), *db.tables.Load()...), t)
	db.tables.Store(&ts)
}

// addIndex creates an index table for d. The caller holds the commit lock
// or has the DB to itself.
func (db *DB) addIndex(tbl int, d Index) *index {
	t := db.table(tbl)
	it := newTable(len(*db.tables.Load()), Schema{Name: t.schema.Name + "." + d.Name, Cols: []string{"pk"}}, d.BucketPrefix)
	it.isIndex = true
	db.appendTable(it)
	ix := &index{def: d, tbl: it}
	old := t.ixs.Load()
	t.ixs.Store(&indexSet{list: append(append([]*index(nil), old.list...), ix), indexed: old.indexed | d.Cols})
	return ix
}

// CreateTable adds a table outside any transaction and returns its ID.
func (db *DB) CreateTable(s Schema) int {
	db.lock()
	defer db.unlock()
	id := len(*db.tables.Load())
	db.appendTable(newTable(id, s, s.BucketPrefix))
	for _, d := range s.Indexes {
		db.addIndex(id, d)
	}
	return id
}

// CreateIndex adds a secondary index outside any transaction, building it
// from the latest committed rows. It fails if a unique index would hold a
// duplicate.
func (db *DB) CreateIndex(tbl int, d Index) error {
	db.lock()
	defer db.unlock()
	t := db.table(tbl)
	last := db.last.Load()
	tmp := &index{def: d}
	seen := map[string]bool{}
	type ent struct{ key, pk string }
	var ents []ent
	for n := t.rows.first(); n != nil; n = n.next[0].Load() {
		if v := visible(n, last); v != nil && v.row != nil {
			k, _ := tmp.entryKey(v.row, n.key)
			if seen[k] {
				return &DuplicateKeyError{Table: tbl, Index: d.Name, Key: k}
			}
			seen[k] = true
			ents = append(ents, ent{k, n.key})
		}
	}
	ix := db.addIndex(tbl, d)
	for _, e := range ents {
		ix.tbl.rows.getOrInsert(e.key, nil).val.head.Store(&version{ts: last, mask: ix.tbl.all | Exists, row: []Value{e.pk}})
	}
	return nil
}

// DropIndex removes secondary index idx outside any transaction. Later
// indexes shift down one position. A transaction that began before the
// drop must not scan the table's indexes by position.
func (db *DB) DropIndex(tbl, idx int) {
	db.lock()
	defer db.unlock()
	t := db.table(tbl)
	old := t.ixs.Load().list
	set := &indexSet{list: append(append([]*index(nil), old[:idx]...), old[idx+1:]...)}
	for _, ix := range set.list {
		set.indexed |= ix.def.Cols
	}
	t.ixs.Store(set)
}

// SetChecks replaces a table's CHECK constraints for future commits.
func (db *DB) SetChecks(tbl int, checks []Check) {
	db.lock()
	defer db.unlock()
	db.table(tbl).checks = checks
}

// TableID returns the ID of the base table with the given name.
func (db *DB) TableID(name string) (int, bool) {
	for _, t := range *db.tables.Load() {
		if !t.isIndex && t.schema.Name == name {
			return t.id, true
		}
	}
	return 0, false
}

// Mode returns the DB's conflict-tracking mode.
func (db *DB) Mode() Mode { return db.opts.Mode }

// LastCommitTs returns the newest published commit timestamp.
func (db *DB) LastCommitTs() uint64 { return db.last.Load() }

// NextID returns the table's next auto-increment value. It runs outside
// transactions, so aborted transactions leave gaps.
func (db *DB) NextID(tbl int) uint64 { return db.table(tbl).nextID.Add(1) }

// NextHidden returns the table's next hidden key number, for tables that
// have no primary key. It is independent of the auto-increment counter.
func (db *DB) NextHidden(tbl int) uint64 { return db.table(tbl).hidden.Add(1) }

// PeekID returns the table's last auto-increment value.
func (db *DB) PeekID(tbl int) uint64 { return db.table(tbl).nextID.Load() }

// SetID sets the table's last auto-increment value.
func (db *DB) SetID(tbl int, id uint64) { db.table(tbl).nextID.Store(id) }

// BumpID raises the table's auto-increment counter to at least id.
func (db *DB) BumpID(tbl int, id uint64) {
	c := &db.table(tbl).nextID
	for {
		cur := c.Load()
		if cur >= id || c.CompareAndSwap(cur, id) {
			return
		}
	}
}

// Begin starts a transaction reading the latest committed snapshot.
func (db *DB) Begin() *Txn {
	return &Txn{db: db, readTs: db.readers.begin(db.last.Load)}
}

// lock takes the commit lock. With Wait set, as in the simulator, it is a
// flag a coroutine waits on, so a commit can interleave with other
// coroutines while holding it.
func (db *DB) lock() {
	if db.opts.Wait == nil {
		db.mu.Lock()
		return
	}
	db.opts.Wait(func() bool { return !db.committing })
	db.committing = true
}

func (db *DB) unlock() {
	if db.opts.Wait == nil {
		db.mu.Unlock()
		return
	}
	db.committing = false
}

// Committing reports whether a commit holds the cooperative commit lock;
// the simulator checks invariants only between commits.
func (db *DB) Committing() bool { return db.committing }

func (db *DB) interleave() {
	if db.opts.Interleave != nil {
		db.opts.Interleave()
	}
}

func (db *DB) point() {
	if db.opts.Yield != nil {
		db.opts.Yield()
	}
}

func (db *DB) invariant(ok bool, format string, args ...any) {
	if !ok && !db.opts.NoInvariants {
		panic(errorf("invariant: "+format, args...))
	}
}

// Rec is a row returned by a scan or Dump.
type Rec struct {
	PK  string
	Row []Value
}

// Dump returns every live row of a table at the latest commit, in key
// order. It is meant for a quiescent DB.
func (db *DB) Dump(tbl int) []Rec {
	ts := db.last.Load()
	var out []Rec
	for n := db.table(tbl).rows.first(); n != nil; n = n.next[0].Load() {
		if v := visible(n, ts); v != nil && v.row != nil {
			out = append(out, Rec{n.key, append([]Value(nil), v.row...)})
		}
	}
	return out
}

// bucketOf maps a key to its bucket: the first prefix bytes, zero-padded,
// with the low BucketBits bits cleared. It is monotone, so a key range maps
// to a contiguous bucket range.
func (t *table) bucketOf(key string, bits uint) string {
	b := make([]byte, t.prefix)
	copy(b, key)
	for i := len(b) - 1; i >= 0 && bits > 0; i-- {
		if bits >= 8 {
			b[i], bits = 0, bits-8
		} else {
			b[i] &^= byte(1)<<bits - 1
			bits = 0
		}
	}
	return string(b)
}

// changedSince reports whether any bucket in [lo, hi] saw an insert or
// delete after ts. hi == "" means unbounded.
func (t *table) changedSince(lo, hi string, ts uint64) bool {
	for n := t.buckets.seek(lo, nil, nil); n != nil && (hi == "" || n.key <= hi); n = n.next[0].Load() {
		if n.val > ts {
			return true
		}
	}
	return false
}

// CheckInvariants verifies the store's internal structure and that every
// secondary index matches its table at the latest commit.
func (db *DB) CheckInvariants() error {
	db.lock()
	defer db.unlock()
	last := db.last.Load()
	for _, t := range *db.tables.Load() {
		if !t.rows.check() || !t.buckets.check() {
			return errorf("table %s: skiplist out of order", t.schema.Name)
		}
		for n := t.rows.first(); n != nil; n = n.next[0].Load() {
			v := n.val.head.Load()
			if v == nil {
				return errorf("table %s row %x has no versions", t.schema.Name, n.key)
			}
			if v.ts > last {
				return errorf("table %s row %x has version %d past last commit %d", t.schema.Name, n.key, v.ts, last)
			}
			for w := v.next.Load(); w != nil; v, w = w, w.next.Load() {
				if w.ts >= v.ts {
					return errorf("table %s row %x versions out of order", t.schema.Name, n.key)
				}
			}
		}
		for n := t.buckets.first(); n != nil; n = n.next[0].Load() {
			if n.val > last {
				return errorf("table %s bucket %x ts %d past last commit %d", t.schema.Name, n.key, n.val, last)
			}
		}
	}
	for _, t := range *db.tables.Load() {
		for _, ix := range t.indexes() {
			want := map[string]string{}
			for n := t.rows.first(); n != nil; n = n.next[0].Load() {
				if v := visible(n, last); v != nil && v.row != nil {
					k, _ := ix.entryKey(v.row, n.key)
					if _, dup := want[k]; dup {
						return errorf("index %s has duplicate key %x", ix.tbl.schema.Name, k)
					}
					want[k] = n.key
				}
			}
			got := 0
			for n := ix.tbl.rows.first(); n != nil; n = n.next[0].Load() {
				if v := visible(n, last); v != nil && v.row != nil {
					got++
					if want[n.key] != v.row[0] {
						return errorf("index %s entry %x points to %x, want %x", ix.tbl.schema.Name, n.key, v.row[0], want[n.key])
					}
				}
			}
			if got != len(want) {
				return errorf("index %s has %d entries, table has %d rows", ix.tbl.schema.Name, got, len(want))
			}
		}
	}
	if d := db.DurableTs(); d > last {
		return errorf("durable ts %d past last commit %d", d, last)
	}
	return db.readers.check()
}

// addValue returns v+delta for an int64 or uint64 v.
func addValue(v Value, delta int64) (Value, error) {
	switch x := v.(type) {
	case int64:
		s := x + delta
		if (s > x) != (delta > 0) {
			return nil, ErrOverflow
		}
		return s, nil
	case uint64:
		s := x + uint64(delta)
		if (delta >= 0 && s < x) || (delta < 0 && s > x) {
			return nil, ErrOverflow
		}
		return s, nil
	}
	return nil, ErrNotInteger
}

// addDelta returns a+b for two pending int64 deltas.
func addDelta(a, b int64) (int64, bool) {
	s := a + b
	return s, (s > a) == (b > 0)
}
