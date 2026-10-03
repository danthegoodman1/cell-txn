// Package check verifies serializability. A Recorder logs what every
// transaction observed and wrote; Replay reruns the committed transactions
// one at a time in their claimed serialization order against an
// independent model and fails on the first divergence.
package check

import (
	"errors"
	"fmt"
	"math"
	"slices"
	"sync"

	"cell-tnx/txn"
)

// Kind classifies a logged transaction.
type Kind uint8

const (
	// Writer committed at Ts = commitTs.
	Writer Kind = iota
	// Reader wrote nothing and committed at Ts = readTs.
	Reader
	// Rejected failed a CHECK or overflowed while merging after commit Ts.
	Rejected
)

func (k Kind) String() string { return [...]string{"writer", "reader", "rejected"}[k] }

// OpKind is a logged operation.
type OpKind uint8

const (
	OpGet OpKind = iota
	OpSet
	OpAdd
	OpInsert
	OpDelete
	OpScan
	OpSavepoint
	OpRollback
	OpIndexScan
)

func (k OpKind) String() string {
	return [...]string{"get", "set", "add", "insert", "delete", "scan", "savepoint", "rollback", "indexscan"}[k]
}

// Result is a write's outcome.
type Result uint8

const (
	OK Result = iota
	NotFound
	Duplicate
)

// Op is one operation and what it returned.
type Op struct {
	Kind  OpKind
	Table int
	Index int // index scans
	PK    string
	Cols  txn.ColMask // get and set columns; scan predicate columns
	Proj  txn.ColMask // scan projection columns
	Col   int         // add column
	Delta int64
	Vals  []txn.Value // get result; set and insert arguments
	Found bool        // get result
	Res   Result      // write result
	Lo    string      // scan range [Lo, Hi); Hi "" is unbounded
	Hi    string
	Rows  []ScanRow
	SP    int
}

// ScanRow is a row a scan visited. Vals holds the predicate columns, plus
// the projection columns when the row matched.
type ScanRow struct {
	PK    string
	Match bool
	Vals  []txn.Value
}

// Log is one finished transaction. Acked reports whether its commit was
// acknowledged to the client, which promises durability.
type Log struct {
	Kind   Kind
	Ts     uint64
	ReadTs uint64
	Client int
	Ops    []Op
	Acked  bool
}

// Recorder collects logs from concurrent clients and hashes every
// operation's outcome into a trace for determinism checks.
type Recorder struct {
	mu    sync.Mutex
	logs  []Log
	trace uint64
	// Off disables logging and tracing, for benchmarks.
	Off bool
	// OnAdd and OnAck, when set, observe each log as it is added and
	// acknowledged, under the recorder's lock.
	OnAdd func(i int, l Log)
	OnAck func(i int)
}

// NewRecorder returns an empty recorder.
func NewRecorder() *Recorder { return &Recorder{trace: fnvOffset} }

// Logs returns the logs recorded so far.
func (r *Recorder) Logs() []Log {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]Log(nil), r.logs...)
}

// Trace returns the hash of every recorded outcome.
func (r *Recorder) Trace() uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.trace
}

const (
	fnvOffset = 14695981039346656037
	fnvPrime  = 1099511628211
)

func (r *Recorder) hash(words ...uint64) {
	if r.Off {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, w := range words {
		for range 8 {
			r.trace = (r.trace ^ (w & 0xff)) * fnvPrime
			w >>= 8
		}
	}
}

func (r *Recorder) add(l Log) int {
	if r.Off {
		return -1
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.logs = append(r.logs, l)
	if r.OnAdd != nil {
		r.OnAdd(len(r.logs)-1, l)
	}
	return len(r.logs) - 1
}

func (r *Recorder) ack(i int) {
	if i < 0 {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.logs[i].Acked = true
	if r.OnAck != nil {
		r.OnAck(i)
	}
}

// SetLogs replaces the recorded logs, so a recovered process can continue
// a history.
func (r *Recorder) SetLogs(logs []Log) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.logs = logs
}

// Truncate drops the logs a crash erased: everything serialized after
// last, the recovered commit timestamp. With durable set, it fails if any
// dropped transaction was acknowledged.
func (r *Recorder) Truncate(last uint64, durable bool) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	kept := r.logs[:0]
	for _, l := range r.logs {
		if l.Ts <= last {
			kept = append(kept, l)
			continue
		}
		if durable && l.Acked && l.Kind != Rejected {
			return fmt.Errorf("check: acknowledged %s at ts %d lost; recovered through %d", l.Kind, l.Ts, last)
		}
	}
	r.logs = kept
	return nil
}

// Tx wraps a transaction, logging each operation. It calls yield before
// every operation, so the simulator can interleave clients there.
type Tx struct {
	t      *txn.Txn
	rec    *Recorder
	yield  func()
	client int
	ops    []Op
}

// Begin starts a recorded transaction.
func (r *Recorder) Begin(db *txn.DB, client int, yield func()) *Tx {
	if yield == nil {
		yield = func() {}
	}
	yield()
	t := db.Begin()
	r.hash(uint64(client), t.ReadTs())
	return &Tx{t: t, rec: r, yield: yield, client: client}
}

// Txn returns the underlying transaction.
func (x *Tx) Txn() *txn.Txn { return x.t }

func (x *Tx) log(op Op) {
	if x.rec.Off {
		return
	}
	x.ops = append(x.ops, op)
	x.rec.hash(uint64(op.Kind), uint64(op.Table), hashString(op.PK), uint64(op.Res), uint64(len(op.Rows)))
	for _, v := range op.Vals {
		x.rec.hash(hashValue(v))
	}
	for _, r := range op.Rows {
		x.rec.hash(hashString(r.PK))
	}
}

func hashString(s string) uint64 {
	h := uint64(fnvOffset)
	for i := 0; i < len(s); i++ {
		h = (h ^ uint64(s[i])) * fnvPrime
	}
	return h
}

func hashValue(v txn.Value) uint64 {
	switch x := v.(type) {
	case int64:
		return uint64(x)
	case uint64:
		return x
	case float64:
		return math.Float64bits(x)
	case string:
		return hashString(x)
	}
	return 0
}

func result(err error) (Result, bool) {
	switch {
	case err == nil:
		return OK, true
	case errors.Is(err, txn.ErrNotFound):
		return NotFound, true
	case errors.Is(err, txn.ErrDuplicateKey):
		return Duplicate, true
	}
	return 0, false
}

// Get is a tracked read; see txn.Txn.Get.
func (x *Tx) Get(tbl int, pk string, cols txn.ColMask) ([]txn.Value, bool, error) {
	x.yield()
	row, ok, err := x.t.Get(tbl, pk, cols)
	if err == nil {
		x.log(Op{Kind: OpGet, Table: tbl, PK: pk, Cols: cols, Vals: slices.Clone(row), Found: ok})
	}
	return row, ok, err
}

// GetUntracked is a snapshot read the checker ignores, since its values
// carry no serializability guarantee.
func (x *Tx) GetUntracked(tbl int, pk string, cols txn.ColMask) ([]txn.Value, bool, error) {
	x.yield()
	return x.t.GetUntracked(tbl, pk, cols)
}

// Scan is a tracked range read; see txn.Txn.Scan.
func (x *Tx) Scan(tbl int, lo, hi string, pred, proj txn.ColMask, match func(pk string, row []txn.Value) bool) ([]txn.Rec, error) {
	return x.scan(OpScan, tbl, 0, lo, hi, pred, proj, match)
}

// IndexScan is a tracked secondary-index range read; see txn.Txn.IndexScan.
func (x *Tx) IndexScan(tbl, idx int, lo, hi string, pred, proj txn.ColMask, match func(pk string, row []txn.Value) bool) ([]txn.Rec, error) {
	return x.scan(OpIndexScan, tbl, idx, lo, hi, pred, proj, match)
}

func (x *Tx) scan(kind OpKind, tbl, idx int, lo, hi string, pred, proj txn.ColMask, match func(pk string, row []txn.Value) bool) ([]txn.Rec, error) {
	x.yield()
	var rows []ScanRow
	m := func(pk string, row []txn.Value) bool {
		ok := match == nil || match(pk, row)
		rows = append(rows, ScanRow{PK: pk, Match: ok, Vals: slices.Clone(row)})
		return ok
	}
	var recs []txn.Rec
	var err error
	if kind == OpScan {
		recs, err = x.t.Scan(tbl, lo, hi, pred, proj, m)
	} else {
		recs, err = x.t.IndexScan(tbl, idx, lo, hi, pred, proj, m)
	}
	if err != nil {
		return nil, err
	}
	j := 0
	for i := range rows {
		if rows[i].Match {
			rows[i].Vals = slices.Clone(recs[j].Row)
			j++
		}
	}
	x.log(Op{Kind: kind, Table: tbl, Index: idx, Lo: lo, Hi: hi, Cols: pred, Proj: proj, Rows: rows})
	return recs, nil
}

// Set assigns columns; see txn.Txn.Set.
func (x *Tx) Set(tbl int, pk string, cols txn.ColMask, vals []txn.Value) error {
	x.yield()
	err := x.t.Set(tbl, pk, cols, vals)
	if res, ok := result(err); ok {
		x.log(Op{Kind: OpSet, Table: tbl, PK: pk, Cols: cols, Vals: slices.Clone(vals), Res: res})
	}
	return err
}

// Add adds a delta; see txn.Txn.Add.
func (x *Tx) Add(tbl int, pk string, col int, delta int64) error {
	x.yield()
	err := x.t.Add(tbl, pk, col, delta)
	if res, ok := result(err); ok {
		x.log(Op{Kind: OpAdd, Table: tbl, PK: pk, Col: col, Delta: delta, Res: res})
	}
	return err
}

// Insert creates a row; see txn.Txn.Insert.
func (x *Tx) Insert(tbl int, pk string, row []txn.Value) error {
	x.yield()
	err := x.t.Insert(tbl, pk, row)
	if res, ok := result(err); ok {
		x.log(Op{Kind: OpInsert, Table: tbl, PK: pk, Vals: slices.Clone(row), Res: res})
	}
	return err
}

// Delete removes a row; see txn.Txn.Delete.
func (x *Tx) Delete(tbl int, pk string) error {
	x.yield()
	err := x.t.Delete(tbl, pk)
	if res, ok := result(err); ok {
		x.log(Op{Kind: OpDelete, Table: tbl, PK: pk, Res: res})
	}
	return err
}

// Savepoint marks the write state; see txn.Txn.Savepoint.
func (x *Tx) Savepoint() int {
	sp := x.t.Savepoint()
	x.log(Op{Kind: OpSavepoint, SP: sp})
	return sp
}

// RollbackTo discards writes after a savepoint; see txn.Txn.RollbackTo.
func (x *Tx) RollbackTo(sp int) {
	x.t.RollbackTo(sp)
	x.log(Op{Kind: OpRollback, SP: sp})
}

// Commit commits and logs the transaction if it committed or was rejected
// by a constraint. The log is written when the commit installs, before
// the durability wait, so a crash cannot hide a commit from the checker.
func (x *Tx) Commit() error {
	x.yield()
	i := -1
	x.t.OnInstall = func(ts uint64) {
		i = x.rec.add(Log{Kind: Writer, Ts: ts, ReadTs: x.t.ReadTs(), Client: x.client, Ops: x.ops})
	}
	err := x.t.Install()
	var ce *txn.ConstraintError
	switch {
	case err == nil:
		if !x.t.Wrote() {
			i = x.rec.add(Log{Kind: Reader, Ts: x.t.SerialTs(), ReadTs: x.t.ReadTs(), Client: x.client, Ops: x.ops})
		}
		x.rec.hash(1, x.t.SerialTs())
		if err = x.t.WaitDurable(); err == nil {
			x.rec.ack(i)
		}
	case errors.As(err, &ce):
		x.rec.add(Log{Kind: Rejected, Ts: ce.AfterTs, ReadTs: x.t.ReadTs(), Client: x.client, Ops: x.ops, Acked: true})
		x.rec.hash(2, ce.AfterTs)
	default:
		x.rec.hash(3)
	}
	return err
}

// Abort abandons the transaction.
func (x *Tx) Abort() {
	x.t.Abort()
	x.rec.hash(4)
}
