package workload

import (
	"math/rand/v2"

	"cell-tnx/check"
	"cell-tnx/keys"
	"cell-tnx/txn"
)

// Random issues random transactions over a small key space using every
// API call. Column 0 of table 0 holds small values under CHECK >= 0 when
// Check is set. Table 0 may index column 0 (non-unique) and column 1
// (unique, drawn from a small domain so claims collide). Every other
// written value is unique.
type Random struct {
	Widths    []int  // columns per table
	Keys      uint64 // keys per table
	Check     bool
	Index0    bool // non-unique index on table 0 column 0
	Unique1   bool // unique index on table 0 column 1
	MaxOps    int
	Weights   [nops]int
	ReadOnly  float64 // share of long read-only transactions
	Abandon   float64 // per-operation chance of abandoning the transaction
	Save      float64 // per-operation chance of a savepoint around it
	Think     float64 // per-operation chance of a pause
	Unbounded float64 // share of scans with no upper bound
}

const (
	opGet = iota
	opUntracked
	opScan
	opIndexScan
	opSet
	opAdd
	opInsert
	opDelete
	nops
)

// NewRandom draws a configuration from r.
func NewRandom(r *rand.Rand) *Random {
	w := &Random{
		Keys:      uint64(2 + r.IntN(40)),
		Check:     r.IntN(4) != 0,
		Index0:    r.IntN(2) == 0,
		Unique1:   r.IntN(2) == 0,
		MaxOps:    1 + r.IntN(8),
		ReadOnly:  r.Float64() * 0.2,
		Abandon:   r.Float64() * 0.02,
		Save:      r.Float64() * 0.2,
		Think:     r.Float64() * 0.3,
		Unbounded: r.Float64() * 0.3,
	}
	for range 1 + r.IntN(2) {
		w.Widths = append(w.Widths, 1+r.IntN(5))
	}
	if w.Widths[0] < 2 {
		w.Unique1 = false
	}
	for i := range w.Weights {
		if r.IntN(5) != 0 {
			w.Weights[i] = 1 + r.IntN(10)
		}
	}
	if w.Weights[opGet] == 0 && w.Weights[opSet] == 0 {
		w.Weights[opSet] = 1
	}
	return w
}

func (w *Random) Name() string { return "random" }

func intKey(c int) func(r []txn.Value) (string, bool) {
	return func(r []txn.Value) (string, bool) {
		x, _ := r[c].(int64)
		return string(keys.AppendInt(nil, x)), true
	}
}

func (w *Random) Schemas() []txn.Schema {
	var ss []txn.Schema
	for i, width := range w.Widths {
		s := txn.Schema{Name: "t", Cols: make([]string, width)}
		if i == 0 {
			if w.Check {
				s.Checks = []txn.Check{{Name: "c0 >= 0", Cols: txn.Col(0), OK: func(r []txn.Value) bool { x, _ := r[0].(int64); return x >= 0 }}}
			}
			if w.Index0 {
				s.Indexes = append(s.Indexes, txn.Index{Name: "c0", Cols: txn.Col(0), Key: intKey(0), BucketPrefix: 9})
			}
			if w.Unique1 {
				s.Indexes = append(s.Indexes, txn.Index{Name: "c1", Cols: txn.Col(1), Unique: true, Key: intKey(1), BucketPrefix: 9})
			}
		}
		ss = append(ss, s)
	}
	return ss
}

func (w *Random) value(c *Client, tbl, col int) int64 {
	switch {
	case tbl == 0 && col == 0 && (w.Check || w.Index0):
		return c.Rand.Int64N(20)
	case tbl == 0 && col == 1 && w.Unique1:
		return c.Rand.Int64N(int64(2 * w.Keys))
	}
	return c.Uniq()
}

func (w *Random) newRow(c *Client, tbl int) []txn.Value {
	r := make([]txn.Value, w.Widths[tbl])
	for col := range r {
		r[col] = w.value(c, tbl, col)
	}
	return r
}

func (w *Random) Load(c *Client) error {
	return c.Do(func(tx *check.Tx) error {
		for tbl := range w.Widths {
			for pk := range w.Keys {
				if c.Rand.IntN(2) == 0 {
					if err := absent(tx.Insert(tbl, keys.U64(pk), w.newRow(c, tbl))); err != nil {
						return err
					}
				}
			}
		}
		return nil
	})
}

func (w *Random) Step(c *Client) error {
	r := c.Rand
	readOnly := r.Float64() < w.ReadOnly
	return expect(c.Do(func(tx *check.Tx) error {
		n := 1 + r.IntN(w.MaxOps)
		if readOnly {
			n = 5 + r.IntN(20)
		}
		for range n {
			if r.Float64() < w.Think {
				c.Env.Sleep(1 + r.Int64N(50))
			}
			if r.Float64() < w.Abandon {
				return errAbort
			}
			if readOnly || r.Float64() >= w.Save {
				if err := w.op(c, tx, readOnly); err != nil {
					return err
				}
				continue
			}
			sp := tx.Savepoint()
			for range 1 + r.IntN(3) {
				if err := w.op(c, tx, false); err != nil {
					return err
				}
			}
			if r.IntN(2) == 0 {
				tx.RollbackTo(sp)
			}
		}
		return nil
	}))
}

func (w *Random) mask(r *rand.Rand, width int) txn.ColMask {
	return txn.ColMask(r.Uint64N(1<<width)) | txn.Col(r.IntN(width))
}

func (w *Random) pick(r *rand.Rand, readOnly bool) int {
	limit := nops
	if readOnly {
		limit = opIndexScan + 1
	}
	total := 0
	for _, x := range w.Weights[:limit] {
		total += x
	}
	if total == 0 {
		return opGet
	}
	n := r.IntN(total)
	for op, x := range w.Weights[:limit] {
		if n < x {
			return op
		}
		n -= x
	}
	panic("unreachable")
}

func (w *Random) op(c *Client, tx *check.Tx, readOnly bool) error {
	r := c.Rand
	tbl := r.IntN(len(w.Widths))
	width := w.Widths[tbl]
	k := r.Uint64N(w.Keys)
	pk := keys.U64(k)
	switch op := w.pick(r, readOnly); op {
	case opGet:
		_, _, err := tx.Get(tbl, pk, w.mask(r, width))
		return err
	case opUntracked:
		_, _, err := tx.GetUntracked(tbl, pk, w.mask(r, width))
		return err
	case opScan, opIndexScan:
		pred, proj := w.mask(r, width), txn.ColMask(r.Uint64N(1<<width))
		col := 0
		for pred&txn.Col(col) == 0 {
			col++
		}
		mod := int64(2 + r.IntN(2))
		match := func(_ string, row []txn.Value) bool { x, _ := row[col].(int64); return x%mod == 0 }
		nidx := 0
		if tbl == 0 {
			nidx = btoi(w.Index0) + btoi(w.Unique1)
		}
		if nidx == 0 || op == opScan {
			hi := keys.U64(k + 1 + r.Uint64N(w.Keys/2+1))
			if r.Float64() < w.Unbounded {
				hi = ""
			}
			_, err := tx.Scan(tbl, pk, hi, pred, proj, match)
			return err
		}
		lo := r.Int64N(int64(2 * w.Keys))
		hi := string(keys.AppendInt(nil, lo+1+r.Int64N(10)))
		if r.Float64() < w.Unbounded {
			hi = ""
		}
		_, err := tx.IndexScan(tbl, r.IntN(nidx), string(keys.AppendInt(nil, lo)), hi, pred, proj, match)
		return err
	case opSet:
		cols := w.mask(r, width)
		vals := make([]txn.Value, width)
		cols.Each(func(col int) { vals[col] = w.value(c, tbl, col) })
		return absent(tx.Set(tbl, pk, cols, vals))
	case opAdd:
		return absent(tx.Add(tbl, pk, r.IntN(width), r.Int64N(13)-6))
	case opInsert:
		return absent(tx.Insert(tbl, pk, w.newRow(c, tbl)))
	default:
		return absent(tx.Delete(tbl, pk))
	}
}

func btoi(b bool) int {
	if b {
		return 1
	}
	return 0
}
