package check

import (
	"fmt"
	"slices"
	"sort"

	"cell-tnx/txn"
)

// Replay reruns the logs serially: writers at their commitTs, then readers
// and constraint rejections at the timestamp they saw. Every logged read
// and write outcome must match the model, every committed row must pass
// its CHECKs, every rejection must be justified, and the final model must
// equal dump for every base table.
func Replay(schemas []txn.Schema, logs []Log, last uint64, dump func(tbl int) []txn.Rec) error {
	logs = slices.Clone(logs)
	sort.SliceStable(logs, func(i, j int) bool {
		if logs[i].Ts != logs[j].Ts {
			return logs[i].Ts < logs[j].Ts
		}
		return logs[i].Kind == Writer && logs[j].Kind != Writer
	})
	var writers uint64
	for _, l := range logs {
		if l.Kind == Writer {
			writers++
			if l.Ts != writers {
				return fmt.Errorf("check: writer commit timestamps skip from %d to %d", writers-1, l.Ts)
			}
		}
	}
	if writers != last {
		return fmt.Errorf("check: %d writers logged, last commit is %d", writers, last)
	}
	m := newModel(schemas)
	for i, l := range logs {
		if err := m.run(l); err != nil {
			return fmt.Errorf("check: log %d (%s ts=%d readTs=%d client=%d): %w", i, l.Kind, l.Ts, l.ReadTs, l.Client, err)
		}
	}
	for tbl := range schemas {
		if err := m.compare(tbl, dump(tbl)); err != nil {
			return fmt.Errorf("check: final state: %w", err)
		}
	}
	return nil
}

// model is the reference state: plain maps, changed one transaction at a
// time.
type model struct {
	schemas []txn.Schema
	tables  []mtable
}

type mtable struct {
	rows  map[string][]txn.Value
	keys  []string // sorted, rebuilt when stale
	stale bool
}

type key struct {
	tbl int
	pk  string
}

type undo struct {
	k    key
	prev []txn.Value
}

func newModel(schemas []txn.Schema) *model {
	m := &model{schemas: schemas, tables: make([]mtable, len(schemas))}
	for i := range m.tables {
		m.tables[i].rows = map[string][]txn.Value{}
	}
	return m
}

func (m *model) get(k key) []txn.Value { return m.tables[k.tbl].rows[k.pk] }

func (m *model) put(k key, row []txn.Value, log *[]undo) {
	t := &m.tables[k.tbl]
	prev, had := t.rows[k.pk]
	*log = append(*log, undo{k, prev})
	if row == nil {
		delete(t.rows, k.pk)
	} else {
		t.rows[k.pk] = row
	}
	if had != (row != nil) {
		t.stale = true
	}
}

func (m *model) rollback(log *[]undo, to int) {
	for len(*log) > to {
		u := (*log)[len(*log)-1]
		*log = (*log)[:len(*log)-1]
		t := &m.tables[u.k.tbl]
		_, had := t.rows[u.k.pk]
		if u.prev == nil {
			delete(t.rows, u.k.pk)
		} else {
			t.rows[u.k.pk] = u.prev
		}
		if had != (u.prev != nil) {
			t.stale = true
		}
	}
}

func inRange(k, lo, hi string) bool { return k >= lo && (hi == "" || k < hi) }

func (m *model) sortedKeys(tbl int) []string {
	t := &m.tables[tbl]
	if t.stale || t.keys == nil {
		t.keys = t.keys[:0]
		for pk := range t.rows {
			t.keys = append(t.keys, pk)
		}
		slices.Sort(t.keys)
		t.stale = false
	}
	return t.keys
}

func (m *model) rangeKeys(tbl int, lo, hi string) []string {
	keys := m.sortedKeys(tbl)
	i, _ := slices.BinarySearch(keys, lo)
	j := i
	for j < len(keys) && inRange(keys[j], lo, hi) {
		j++
	}
	return keys[i:j]
}

// entryKey mirrors the store's index entry key.
func entryKey(ix txn.Index, row []txn.Value, pk string) string {
	k, u := ix.Key(row)
	if ix.Unique && u {
		return k
	}
	return k + pk
}

// indexRange returns the primary keys whose entries in index idx fall in
// [lo, hi), in entry order.
func (m *model) indexRange(tbl, idx int, lo, hi string) []string {
	ix := m.schemas[tbl].Indexes[idx]
	type ent struct{ ek, pk string }
	var es []ent
	for pk, row := range m.tables[tbl].rows {
		if ek := entryKey(ix, row, pk); inRange(ek, lo, hi) {
			es = append(es, ent{ek, pk})
		}
	}
	sort.Slice(es, func(i, j int) bool { return es[i].ek < es[j].ek })
	pks := make([]string, len(es))
	for i, e := range es {
		pks[i] = e.pk
	}
	return pks
}

// duplicates reports whether row, the new image of pk, collides with
// another row on a unique index over the written columns.
func (m *model) duplicates(tbl int, pk string, row []txn.Value, written txn.ColMask) bool {
	for _, ix := range m.schemas[tbl].Indexes {
		if !ix.Unique || ix.Cols&written == 0 {
			continue
		}
		k, u := ix.Key(row)
		if !u {
			continue
		}
		for pk2, r2 := range m.tables[tbl].rows {
			if k2, u2 := ix.Key(r2); pk2 != pk && u2 && k2 == k {
				return true
			}
		}
	}
	return false
}

func colsEqual(cols txn.ColMask, got, want []txn.Value) bool {
	ok := true
	cols.Each(func(c int) { ok = ok && got[c] == want[c] })
	return ok
}

// addValue mirrors the store's checked integer addition.
func addValue(v txn.Value, d int64) (txn.Value, bool) {
	switch x := v.(type) {
	case int64:
		s := x + d
		return s, (s > x) == (d > 0)
	case uint64:
		s := x + uint64(d)
		return s, !((d >= 0 && s < x) || (d < 0 && s > x))
	}
	return v, false
}

// run replays one transaction.
func (m *model) run(l Log) error {
	var log []undo
	sps := map[int]int{}
	overflow := false
	exists := func(row []txn.Value) Result {
		if row == nil {
			return NotFound
		}
		return OK
	}
	for i, op := range l.Ops {
		k := key{op.Table, op.PK}
		row := m.get(k)
		fail := func(format string, args ...any) error {
			return fmt.Errorf("op %d %s table=%d pk=%x: %s", i, op.Kind, op.Table, op.PK, fmt.Sprintf(format, args...))
		}
		// write applies a new image of an existing row, unless it would
		// duplicate a unique key.
		write := func(r []txn.Value, written txn.ColMask) error {
			want := OK
			if m.duplicates(op.Table, op.PK, r, written) {
				want = Duplicate
			}
			if want != op.Res {
				return fail("result %d, model says %d", op.Res, want)
			}
			if want == OK {
				m.put(k, r, &log)
			}
			return nil
		}
		switch op.Kind {
		case OpGet:
			if (row != nil) != op.Found {
				return fail("found=%v, model has row=%v", op.Found, row != nil)
			}
			if row != nil && !colsEqual(op.Cols, op.Vals, row) {
				return fail("read %v, model has %v (cols %#x)", op.Vals, row, uint64(op.Cols))
			}
		case OpSet, OpAdd:
			if row == nil {
				if op.Res != NotFound {
					return fail("result %d, model says not found", op.Res)
				}
				continue
			}
			r := slices.Clone(row)
			written := op.Cols
			if op.Kind == OpSet {
				op.Cols.Each(func(c int) { r[c] = op.Vals[c] })
			} else {
				written = txn.Col(op.Col)
				var ok bool
				if r[op.Col], ok = addValue(r[op.Col], op.Delta); !ok {
					overflow = true
				}
			}
			if err := write(r, written); err != nil {
				return err
			}
		case OpInsert:
			if row != nil {
				if op.Res != Duplicate {
					return fail("result %d, model has the row", op.Res)
				}
				continue
			}
			if err := write(slices.Clone(op.Vals), ^txn.Exists); err != nil {
				return err
			}
		case OpDelete:
			if got := exists(row); got != op.Res {
				return fail("result %d, model says %d", op.Res, got)
			}
			if row != nil {
				m.put(k, nil, &log)
			}
		case OpScan, OpIndexScan:
			var pks []string
			if op.Kind == OpScan {
				pks = m.rangeKeys(op.Table, op.Lo, op.Hi)
			} else {
				pks = m.indexRange(op.Table, op.Index, op.Lo, op.Hi)
			}
			if len(pks) != len(op.Rows) {
				return fail("%s saw %d rows, model has %d", op.Kind, len(op.Rows), len(pks))
			}
			for j, sr := range op.Rows {
				if sr.PK != pks[j] {
					return fail("row %d is pk %x, model has %x", j, sr.PK, pks[j])
				}
				cols := op.Cols
				if sr.Match {
					cols |= op.Proj
				}
				if r := m.get(key{op.Table, pks[j]}); !colsEqual(cols, sr.Vals, r) {
					return fail("pk %x read %v, model has %v", sr.PK, sr.Vals, r)
				}
			}
		case OpSavepoint:
			sps[op.SP] = len(log)
		case OpRollback:
			to, ok := sps[op.SP]
			if !ok {
				return fail("rollback to unknown savepoint %d", op.SP)
			}
			m.rollback(&log, to)
		}
	}

	// Rows this transaction changed, after rollbacks.
	var touched []key
	seen := map[key]bool{}
	for _, u := range log {
		if !seen[u.k] {
			seen[u.k] = true
			touched = append(touched, u.k)
		}
	}
	violation := ""
	for _, k := range touched {
		row := m.get(k)
		if row == nil {
			continue
		}
		for _, c := range m.schemas[k.tbl].Checks {
			if !c.OK(row) && violation == "" {
				violation = fmt.Sprintf("table %d pk %x violates %s: %v", k.tbl, k.pk, c.Name, row)
			}
		}
	}

	switch l.Kind {
	case Writer:
		if overflow {
			return fmt.Errorf("committed an overflowing delta")
		}
		if violation != "" {
			return fmt.Errorf("committed a row that %s", violation)
		}
	case Reader:
		for _, u := range log {
			if !slices.Equal(m.get(u.k), m.before(u.k, log)) {
				return fmt.Errorf("read-only transaction changed table %d pk %x", u.k.tbl, u.k.pk)
			}
		}
		m.rollback(&log, 0)
	case Rejected:
		if violation == "" && !overflow {
			return fmt.Errorf("rejected, but every changed row passes its checks")
		}
		m.rollback(&log, 0)
	}
	return nil
}

// before returns k's row before the first logged change to it.
func (m *model) before(k key, log []undo) []txn.Value {
	for _, u := range log {
		if u.k == k {
			return u.prev
		}
	}
	return m.get(k)
}

func (m *model) compare(tbl int, dump []txn.Rec) error {
	keys := m.sortedKeys(tbl)
	if len(keys) != len(dump) {
		return fmt.Errorf("table %d has %d rows, model has %d", tbl, len(dump), len(keys))
	}
	for i, r := range dump {
		if r.PK != keys[i] || !slices.Equal(r.Row, m.get(key{tbl, keys[i]})) {
			return fmt.Errorf("table %d row %x = %v, model has pk %x = %v", tbl, r.PK, r.Row, keys[i], m.get(key{tbl, keys[i]}))
		}
	}
	return nil
}
