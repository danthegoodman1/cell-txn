package txn

import (
	"errors"
	"testing"

	"cell-tnx/keys"
)

// people(name, email, age) with a unique index on email and a non-unique
// index on age.
var peopleSchema = Schema{
	Name: "people",
	Cols: []string{"name", "email", "age"},
	Indexes: []Index{
		{Name: "email", Cols: Col(email), Unique: true, Key: func(r []Value) (string, bool) {
			if r[email] == nil {
				return string(keys.AppendNull(nil)), false
			}
			return string(keys.AppendInt(nil, r[email].(int64))), true
		}},
		{Name: "age", Cols: Col(wallet), Key: func(r []Value) (string, bool) {
			return string(keys.AppendInt(nil, r[wallet].(int64))), false
		}},
	},
}

const age = wallet

func openPeople(t *testing.T) *DB {
	t.Helper()
	db := Open(Options{Mode: CellDelta, BucketBits: 4}, peopleSchema)
	tx := db.Begin()
	must(t, tx.Insert(0, A, ints(1, 100, 30)))
	must(t, tx.Insert(0, B, ints(2, 200, 40)))
	must(t, tx.Commit())
	return db
}

func ageRange(lo, hi int64) (string, string) {
	return string(keys.AppendInt(nil, lo)), string(keys.AppendInt(nil, hi))
}

func wantDup(t *testing.T, err error) {
	t.Helper()
	if !errors.Is(err, ErrDuplicateKey) {
		t.Fatalf("got %v, want duplicate key", err)
	}
}

func TestUniqueIndex(t *testing.T) {
	db := openPeople(t)
	tx := db.Begin()
	wantDup(t, tx.Set(0, B, Col(email), vals(email, 100)))
	wantDup(t, tx.Insert(0, k(3), ints(3, 100, 1)))
	// Moving A off 100 frees it within the transaction.
	must(t, tx.Set(0, A, Col(email), vals(email, 101)))
	must(t, tx.Set(0, B, Col(email), vals(email, 100)))
	must(t, tx.Set(0, A, Col(email), vals(email, 200)))
	must(t, tx.Commit())
	must(t, db.CheckInvariants())

	// Concurrent claims of the same free key: the second commit conflicts.
	t1, t2 := db.Begin(), db.Begin()
	must(t, t1.Set(0, A, Col(email), vals(email, 500)))
	must(t, t2.Set(0, B, Col(email), vals(email, 500)))
	must(t, t1.Commit())
	wantConflict(t, t2.Commit())

	// A blind rewrite of the same value conflicts with a concurrent move away
	// and a claim of the value by another row.
	t1 = db.Begin()
	must(t, t1.Set(0, A, Col(email), vals(email, 500)))
	t2 = db.Begin()
	must(t, t2.Set(0, A, Col(email), vals(email, 600)))
	must(t, t2.Commit())
	t3 := db.Begin()
	must(t, t3.Insert(0, k(9), ints(9, 500, 1)))
	must(t, t3.Commit())
	wantConflict(t, t1.Commit())
	must(t, db.CheckInvariants())
}

func TestIndexScan(t *testing.T) {
	db := openPeople(t)
	tx := db.Begin()
	must(t, tx.Insert(0, k(3), ints(3, 300, 35)))
	must(t, tx.Set(0, B, Col(age), vals(age, 20)))
	lo, hi := ageRange(25, 50)
	recs, err := tx.IndexScan(0, 1, lo, hi, 0, Col(name), nil)
	must(t, err)
	var names []Value
	for _, r := range recs {
		names = append(names, r.Row[name])
	}
	if len(names) != 2 || names[0] != int64(1) || names[1] != int64(3) {
		t.Fatalf("names = %v, want [1 3]", names)
	}
	must(t, tx.Commit())
	must(t, db.CheckInvariants())

	// A concurrent insert into the scanned range is a phantom.
	t1, t2 := db.Begin(), db.Begin()
	_, err = t1.IndexScan(0, 1, lo, hi, 0, 0, nil)
	must(t, err)
	must(t, t1.Set(0, A, Col(name), vals(name, 7)))
	must(t, t2.Insert(0, k(4), ints(4, 400, 45)))
	must(t, t2.Commit())
	wantConflict(t, t1.Commit())

	// So is moving a row into the range.
	t1, t2 = db.Begin(), db.Begin()
	_, err = t1.IndexScan(0, 1, lo, hi, 0, 0, nil)
	must(t, err)
	must(t, t1.Set(0, A, Col(name), vals(name, 8)))
	must(t, t2.Set(0, B, Col(age), vals(age, 26)))
	must(t, t2.Commit())
	wantConflict(t, t1.Commit())
	must(t, db.CheckInvariants())
}

// Deltas on indexed columns read the column, so they never commute.
func TestIndexedDeltasConflict(t *testing.T) {
	db := openPeople(t)
	t1, t2 := db.Begin(), db.Begin()
	must(t, t1.Add(0, A, age, 1))
	must(t, t2.Add(0, A, age, 1))
	must(t, t2.Commit())
	wantConflict(t, t1.Commit())
	must(t, db.CheckInvariants())
}
