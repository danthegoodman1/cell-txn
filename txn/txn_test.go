package txn

import (
	"errors"
	"sync"
	"testing"

	"cell-tnx/keys"
)

const (
	name = iota
	email
	wallet
)

const (
	users = iota
	other
)

var A, B = k(1), k(2)

func k(i uint64) string { return keys.U64(i) }

var usersSchema = Schema{
	Name: "users",
	Cols: []string{"name", "email", "wallet"},
	Checks: []Check{{
		Name: "wallet >= 0",
		Cols: Col(wallet),
		OK:   func(r []Value) bool { return r[wallet].(int64) >= 0 },
	}},
}

var otherSchema = Schema{Name: "other", Cols: []string{"v"}}

// open returns a DB with users A and B, wallets set to walletA and 100.
func open(t *testing.T, mode Mode, walletA int64) *DB {
	t.Helper()
	db := Open(Options{Mode: mode, BucketBits: 4}, usersSchema, otherSchema)
	tx := db.Begin()
	must(t, tx.Insert(users, A, ints(10, 11, walletA)))
	must(t, tx.Insert(users, B, ints(20, 21, 100)))
	must(t, tx.Commit())
	return db
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

func ints(vs ...int64) []Value {
	r := make([]Value, len(vs))
	for i, v := range vs {
		r[i] = v
	}
	return r
}

func vals(c int, v int64) []Value {
	r := make([]Value, 3)
	r[c] = v
	return r
}

func row(t *testing.T, db *DB, pk string) []Value {
	t.Helper()
	tx := db.Begin()
	defer tx.Abort()
	r, ok, err := tx.Get(users, pk, usersSchema.all())
	must(t, err)
	if !ok {
		t.Fatalf("row %x missing", pk)
	}
	return r
}

func (s Schema) all() ColMask { return Col(len(s.Cols)) - 1 }

func wantConflict(t *testing.T, err error) {
	t.Helper()
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("got %v, want conflict", err)
	}
}

// Scenario 1 and 9: a read of A.email and a write of A.wallet are disjoint.
func TestDisjointColumnsMerge(t *testing.T) {
	for _, mode := range []Mode{Row, Cell, CellDelta} {
		t.Run(mode.String(), func(t *testing.T) {
			db := open(t, mode, 50)
			t1, t2 := db.Begin(), db.Begin()
			_, _, err := t1.Get(users, A, Col(email))
			must(t, err)
			must(t, t1.Set(users, A, Col(name), vals(name, 99)))
			must(t, t2.Set(users, A, Col(wallet), vals(wallet, 77)))
			must(t, t2.Commit())
			err = t1.Commit()
			if mode == Row {
				wantConflict(t, err)
				return
			}
			must(t, err)
			if got := row(t, db, A); got[name] != int64(99) || got[email] != int64(11) || got[wallet] != int64(77) {
				t.Fatalf("A = %v, want both changes", got)
			}
		})
	}
}

// Scenario 2.
func TestReadWriteConflict(t *testing.T) {
	db := open(t, Cell, 50)
	t1, t2 := db.Begin(), db.Begin()
	_, _, err := t2.Get(users, A, Col(email))
	must(t, err)
	must(t, t2.Set(users, B, Col(name), vals(name, 1)))
	must(t, t1.Set(users, A, Col(email), vals(email, 12)))
	must(t, t1.Commit())
	wantConflict(t, t2.Commit())
}

// Scenario 3.
func TestWriteSkew(t *testing.T) {
	db := open(t, CellDelta, 50)
	t1, t2 := db.Begin(), db.Begin()
	for _, tx := range []*Txn{t1, t2} {
		for _, pk := range []string{A, B} {
			_, _, err := tx.Get(users, pk, Col(wallet))
			must(t, err)
		}
	}
	must(t, t1.Set(users, A, Col(wallet), vals(wallet, 0)))
	must(t, t2.Set(users, B, Col(wallet), vals(wallet, 0)))
	must(t, t1.Commit())
	wantConflict(t, t2.Commit())
}

// Scenario 4.
func TestPhantom(t *testing.T) {
	for _, tc := range []struct {
		insert uint64
		abort  bool
	}{{5, true}, {40, false}} {
		tc := tc
		db := open(t, Cell, 50)
		t1, t2 := db.Begin(), db.Begin()
		recs, err := t1.Scan(users, k(0), k(16), Col(wallet), Col(email), nil)
		must(t, err)
		if len(recs) != 2 {
			t.Fatalf("scan found %d rows, want 2", len(recs))
		}
		must(t, t1.Set(users, B, Col(name), vals(name, 1)))
		must(t, t2.Insert(users, k(tc.insert), ints(0, 0, 0)))
		must(t, t2.Commit())
		if err := t1.Commit(); tc.abort {
			wantConflict(t, err)
		} else {
			must(t, err)
		}
	}
}

// Scenario 5.
func TestAbsentKeys(t *testing.T) {
	for _, tc := range []struct {
		insert uint64
		abort  bool
	}{{7, true}, {8, false}} {
		db := open(t, Cell, 50)
		t1, t2 := db.Begin(), db.Begin()
		if _, ok, err := t1.Get(users, k(7), Col(name)); err != nil || ok {
			t.Fatalf("Get(7) = %v, %v; want missing", ok, err)
		}
		must(t, t1.Set(users, A, Col(name), vals(name, 1)))
		must(t, t2.Insert(users, k(tc.insert), ints(0, 0, 0)))
		must(t, t2.Commit())
		if err := t1.Commit(); tc.abort {
			wantConflict(t, err)
		} else {
			must(t, err)
		}
	}

	db := Open(Options{Mode: Cell, BucketBits: 8}, usersSchema, otherSchema)
	var txs []*Txn
	for i := range 100 {
		tx := db.Begin()
		must(t, tx.Insert(other, k(uint64(256+i)), ints(int64(i)))) // all in bucket 1
		txs = append(txs, tx)
	}
	for _, tx := range txs {
		must(t, tx.Commit())
	}

	t1, t2 := db.Begin(), db.Begin()
	must(t, t1.Insert(other, k(5000), ints(1)))
	must(t, t2.Insert(other, k(5000), ints(2)))
	must(t, t1.Commit())
	wantConflict(t, t2.Commit())
}

// Scenario 6.
func TestSetConflictsWithDelete(t *testing.T) {
	db := open(t, CellDelta, 50)
	t1, t2 := db.Begin(), db.Begin()
	must(t, t1.Set(users, A, Col(email), vals(email, 5)))
	must(t, t2.Delete(users, A))
	must(t, t2.Commit())
	wantConflict(t, t1.Commit())
}

// Scenario 7, on real goroutines.
func TestConcurrentDeltas(t *testing.T) {
	db := open(t, CellDelta, 1000)
	var wg sync.WaitGroup
	for range 100 {
		wg.Go(func() {
			tx := db.Begin()
			if err := tx.Add(users, A, wallet, -1); err != nil {
				t.Error(err)
			}
			if err := tx.Commit(); err != nil {
				t.Error(err)
			}
		})
	}
	wg.Wait()
	if got := row(t, db, A)[wallet]; got != int64(900) {
		t.Fatalf("wallet = %d, want 900", got)
	}
}

// Scenario 8, on real goroutines.
func TestDeltaCheck(t *testing.T) {
	db := open(t, CellDelta, 5)
	var mu sync.Mutex
	var ok, constraint int
	var wg sync.WaitGroup
	for range 10 {
		wg.Go(func() {
			tx := db.Begin()
			if err := tx.Add(users, A, wallet, -1); err != nil {
				t.Error(err)
			}
			err := tx.Commit()
			mu.Lock()
			defer mu.Unlock()
			switch {
			case err == nil:
				ok++
			case errors.Is(err, ErrConstraint):
				constraint++
			default:
				t.Error(err)
			}
		})
	}
	wg.Wait()
	if ok != 5 || constraint != 5 {
		t.Fatalf("%d commits, %d constraint failures; want 5 and 5", ok, constraint)
	}
}

// Deltas conflict unless the mode lets them commute.
func TestDeltaModes(t *testing.T) {
	for _, mode := range []Mode{Row, Cell, CellDelta} {
		db := open(t, mode, 50)
		t1, t2 := db.Begin(), db.Begin()
		must(t, t1.Add(users, A, wallet, 1))
		must(t, t2.Add(users, A, wallet, 1))
		must(t, t2.Commit())
		if err := t1.Commit(); mode == CellDelta {
			must(t, err)
		} else {
			wantConflict(t, err)
		}
	}
}

// Reading a column with a pending delta makes the delta read its column.
func TestReadingDeltaRecordsRead(t *testing.T) {
	db := open(t, CellDelta, 50)
	t1, t2 := db.Begin(), db.Begin()
	must(t, t1.Add(users, A, wallet, 1))
	r, _, err := t1.Get(users, A, Col(wallet))
	must(t, err)
	if r[wallet] != int64(51) {
		t.Fatalf("wallet = %d, want 51", r[wallet])
	}
	must(t, t2.Add(users, A, wallet, 1))
	must(t, t2.Commit())
	wantConflict(t, t1.Commit())
}

func TestOverflowIsConstraint(t *testing.T) {
	db := open(t, CellDelta, 1<<62)
	tx := db.Begin()
	must(t, tx.Add(users, A, wallet, 1<<62))
	if err := tx.Commit(); !errors.Is(err, ErrConstraint) {
		t.Fatalf("got %v, want constraint", err)
	}
}

func TestSavepoint(t *testing.T) {
	db := open(t, Cell, 50)
	tx := db.Begin()
	must(t, tx.Set(users, A, Col(name), vals(name, 1)))
	sp := tx.Savepoint()
	must(t, tx.Set(users, A, Col(name), vals(name, 2)))
	must(t, tx.Delete(users, B))
	must(t, tx.Insert(users, k(3), ints(3, 3, 3)))
	tx.RollbackTo(sp)
	must(t, tx.Commit())
	if got := row(t, db, A)[name]; got != int64(1) {
		t.Fatalf("name = %d, want 1", got)
	}
	row(t, db, B)
	if _, ok, _ := db.Begin().Get(users, k(3), Col(name)); ok {
		t.Fatal("row 3 survived rollback")
	}
}

// A long reader keeps its snapshot while GC trims behind newer writers.
func TestGCKeepsSnapshots(t *testing.T) {
	db := open(t, Cell, 50)
	old := db.Begin()
	for i := range 10 {
		tx := db.Begin()
		must(t, tx.Set(users, A, Col(wallet), vals(wallet, int64(i))))
		must(t, tx.Commit())
	}
	if r, ok, _ := old.Get(users, A, Col(wallet)); !ok || r[wallet] != int64(50) {
		t.Fatalf("old snapshot sees %v", r)
	}
	old.Abort()
	tx := db.Begin()
	must(t, tx.Set(users, A, Col(wallet), vals(wallet, 99)))
	must(t, tx.Commit())
	n := 0
	for v := db.table(users).rows.get(A, nil).val.head.Load(); v != nil; v = v.next.Load() {
		n++
	}
	if n != 2 {
		t.Fatalf("chain has %d versions after GC, want 2", n)
	}
	must(t, db.CheckInvariants())
}

// Scans see the transaction's own inserts, updates and deletes.
func TestScanOwnWrites(t *testing.T) {
	db := open(t, Cell, 50)
	tx := db.Begin()
	must(t, tx.Insert(users, k(0), ints(0, 0, 1)))
	must(t, tx.Set(users, A, Col(wallet), vals(wallet, 7)))
	must(t, tx.Delete(users, B))
	must(t, tx.Insert(users, k(9), ints(9, 9, 9)))
	recs, err := tx.Scan(users, k(0), k(100), Col(wallet), 0, nil)
	must(t, err)
	var got [][2]any
	for _, r := range recs {
		got = append(got, [2]any{r.PK, r.Row[wallet]})
	}
	want := [][2]any{{k(0), int64(1)}, {A, int64(7)}, {k(9), int64(9)}}
	if len(got) != len(want) {
		t.Fatalf("scan = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("scan = %v, want %v", got, want)
		}
	}
}

func TestReadOnlyCommitsAtReadTs(t *testing.T) {
	db := open(t, Cell, 50)
	tx := db.Begin()
	_, _, err := tx.Get(users, A, Col(wallet))
	must(t, err)
	w := db.Begin()
	must(t, w.Set(users, A, Col(wallet), vals(wallet, 1)))
	must(t, w.Commit())
	must(t, tx.Commit())
	if tx.Wrote() || tx.SerialTs() != tx.ReadTs() {
		t.Fatalf("read-only txn serialized at %d, readTs %d", tx.SerialTs(), tx.ReadTs())
	}
}
