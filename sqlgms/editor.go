package sqlgms

import (
	"errors"
	"fmt"
	"math"

	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/expression"

	"cell-tnx/keys"
	"cell-tnx/txn"
)

// --- checks ---

// checkSet is a table's compiled CHECKs at one definition version.
type checkSet struct {
	version int
	list    []compiledCheck
}

type compiledCheck struct {
	name string
	cols txn.ColMask
	expr sql.Expression // evaluated over a full table row
}

func (c compiledCheck) ok(ctx *sql.Context, row sql.Row) (bool, error) {
	res, err := sql.EvaluateCondition(ctx, c.expr, row)
	if err != nil {
		return false, err
	}
	return !sql.IsFalse(res), nil
}

// compile installs the plan's CHECKs, which the analyzer has bound to
// table rows, as the table's statement-time checks and the core's
// commit-time checks.
func (t *tableDef) compile(checks sql.CheckConstraints) {
	t.mu.Lock()
	version := t.version
	t.mu.Unlock()
	if cur := t.compiled.Load(); cur != nil && cur.version == version && len(cur.list) == len(checks) && version > 0 {
		return
	}
	set := &checkSet{version: version}
	var core []txn.Check
	sch := t.sch
	for _, ch := range checks {
		if !ch.Enforced {
			continue
		}
		c := compiledCheck{name: ch.Name, expr: ch.Expr}
		var cols txn.ColMask
		walkExpr(ch.Expr, func(e sql.Expression) {
			if gf, ok := e.(*expression.GetField); ok {
				if i := t.colIndex(gf.Name()); i >= 0 {
					cols |= txn.Col(i)
				}
			}
		})
		if cols == 0 {
			cols = t.all()
		}
		c.cols = cols
		set.list = append(set.list, c)
		core = append(core, txn.Check{Name: c.name, Cols: cols, OK: func(row []txn.Value) bool {
			r, err := fromCoreRow(sch, row)
			if err != nil {
				return false
			}
			ok, err := c.ok(sql.NewEmptyContext(), r)
			return err == nil && ok
		}})
	}
	t.compiled.Store(set)
	t.db.p.core.SetChecks(t.id, core)
}

func walkExpr(e sql.Expression, f func(sql.Expression)) {
	f(e)
	for _, c := range e.Children() {
		walkExpr(c, f)
	}
}

// checkRow evaluates, at statement time, the CHECKs over the written
// columns. A CHECK whose inputs are all final (written outright or
// tracked) decides here. Any other CHECK is evaluated on the
// transaction's view of the row: if it fails, the statement fails and
// reads the CHECK's columns, so commit validates the values the failure
// rests on; if it passes, the commit re-checks the merged row.
func (t *Table) checkRow(ctx *sql.Context, tx *txn.Txn, key string, row sql.Row, written, final txn.ColMask) error {
	for _, c := range t.def.compiled.Load().list {
		if c.cols&written == 0 {
			continue
		}
		ok, err := c.ok(ctx, row)
		if err != nil {
			return err
		}
		if ok {
			continue
		}
		if c.cols&^final != 0 {
			if _, _, err := tx.Get(t.def.id, key, c.cols); err != nil {
				return err
			}
		}
		return sql.ErrCheckConstraintViolated.New(c.name)
	}
	return nil
}

// --- editors ---

// editor applies one statement's row changes to the session's transaction.
// Each StatementBegin opens a savepoint that DiscardChanges rolls back.
type editor struct {
	t   *Table
	sps []int
}

var _ sql.TableEditor = (*editor)(nil)

func (t *Table) Inserter(*sql.Context) sql.RowInserter { return &editor{t: t} }
func (t *Table) Updater(*sql.Context) sql.RowUpdater   { return &editor{t: t} }
func (t *Table) Deleter(*sql.Context) sql.RowDeleter   { return &editor{t: t} }
func (t *Table) Replacer(*sql.Context) sql.RowReplacer { return &editor{t: t} }

// StatementBegin opens a savepoint and a statement view: the statement's
// reads, such as a join's repeated scans, see none of its own writes.
func (e *editor) StatementBegin(ctx *sql.Context) {
	if tx, err := txnOf(ctx); err == nil {
		e.sps = append(e.sps, tx.Savepoint())
		tx.BeginStatement()
	}
}

func (e *editor) DiscardChanges(ctx *sql.Context, _ error) error {
	tx, err := txnOf(ctx)
	if err != nil || len(e.sps) == 0 {
		return err
	}
	tx.RollbackTo(e.sps[len(e.sps)-1])
	tx.EndStatement()
	e.sps = e.sps[:len(e.sps)-1]
	return nil
}

func (e *editor) StatementComplete(ctx *sql.Context) error {
	if len(e.sps) > 0 {
		e.sps = e.sps[:len(e.sps)-1]
		if tx, err := txnOf(ctx); err == nil {
			tx.EndStatement()
		}
	}
	return nil
}

func (e *editor) Close(*sql.Context) error { return nil }

func (e *editor) Insert(ctx *sql.Context, row sql.Row) error {
	tx, err := txnOf(ctx)
	if err != nil {
		return err
	}
	def := e.t.def
	vals, err := toCoreRow(row)
	if err != nil {
		return err
	}
	var key string
	if len(def.pk) == 0 {
		key = keys.U64(def.db.p.core.NextHidden(def.id))
	} else if key, err = def.key(vals); err != nil {
		return err
	}
	if err := e.t.checkRow(ctx, tx, key, row, def.all(), def.all()); err != nil {
		return err
	}
	return e.dup(ctx, tx, tx.Insert(def.id, key, vals))
}

// dup converts a duplicate-key error into the engine's, carrying the
// existing row, which INSERT … ON DUPLICATE KEY UPDATE and REPLACE use.
// The existing row is read in full, tracked, since the statement may use
// any of it.
func (e *editor) dup(ctx *sql.Context, tx *txn.Txn, err error) error {
	var de *txn.DuplicateKeyError
	if !errors.As(err, &de) {
		return err
	}
	def := e.t.def
	row, found, rerr := tx.CurrentRow(def.id, de.Owner, def.all())
	if rerr != nil || !found {
		return fmt.Errorf("sqlgms: duplicate key %s with no existing row: %v", de.Index, rerr)
	}
	existing, cerr := fromCoreRow(def.sch, row)
	if cerr != nil {
		return cerr
	}
	return sql.NewUniqueKeyErr(fmt.Sprint(existing), de.Index == "PRIMARY", existing)
}

// stored finds the key of a row in a keyless table by its values.
func (e *editor) stored(tx *txn.Txn, vals []txn.Value) (string, error) {
	def := e.t.def
	if len(def.pk) > 0 {
		return def.key(vals)
	}
	recs, err := tx.ScanRows(def.id, -1, "", "", def.all())
	if err != nil {
		return "", err
	}
	for _, r := range recs {
		if equalRows(r.Row, vals) {
			return r.PK, nil
		}
	}
	return "", sql.ErrDeleteRowNotFound.New()
}

func equalRows(a, b []txn.Value) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// Update writes the statement's assigned columns, plus any the engine
// changed itself, such as ON UPDATE timestamps. Deltas become Adds of
// new - old, which equals the statement's constant whatever the untracked
// old value was. A changed primary key becomes a delete and an insert.
func (e *editor) Update(ctx *sql.Context, old, new sql.Row) error {
	tx, err := txnOf(ctx)
	if err != nil {
		return err
	}
	def := e.t.def
	ov, err := toCoreRow(old)
	if err != nil {
		return err
	}
	nv, err := toCoreRow(new)
	if err != nil {
		return err
	}
	ok, err := e.stored(tx, ov)
	if err != nil {
		return err
	}
	if len(def.pk) > 0 {
		if nk, err := def.key(nv); err != nil {
			return err
		} else if nk != ok {
			if err := e.t.checkRow(ctx, tx, nk, new, def.all(), def.all()); err != nil {
				return err
			}
			if err := tx.Delete(def.id, ok); err != nil {
				return err
			}
			return e.dup(ctx, tx, tx.Insert(def.id, nk, nv))
		}
	}
	acc := e.t.acc
	if acc == nil {
		acc = &access{read: def.all(), write: def.all()}
	}
	written := acc.write
	for i, c := range def.sch {
		if ov[i] != nv[i] || c.OnUpdate != nil {
			written |= txn.Col(i)
		}
	}
	deltas := acc.delta & written
	if err := e.t.checkRow(ctx, tx, ok, new, written, (written&^deltas)|acc.read); err != nil {
		return err
	}
	var derr error
	deltas.Each(func(c int) {
		if derr == nil {
			var d int64
			if d, derr = diff(ov[c], nv[c]); derr == nil && d != 0 {
				derr = tx.Add(def.id, ok, c, d)
			}
		}
	})
	if derr != nil {
		return derr
	}
	if set := written &^ deltas; set != 0 {
		return e.dup(ctx, tx, tx.Set(def.id, ok, set, nv))
	}
	return nil
}

// diff returns new - old for two integers of the same kind.
func diff(old, new txn.Value) (int64, error) {
	switch o := old.(type) {
	case int64:
		n, _ := new.(int64)
		d := n - o
		if (o > 0 && d > n) || (o < 0 && d < n) {
			return 0, txn.ErrOverflow
		}
		return d, nil
	case uint64:
		n, _ := new.(uint64)
		if n >= o {
			if n-o > math.MaxInt64 {
				return 0, txn.ErrOverflow
			}
			return int64(n - o), nil
		}
		if o-n > math.MaxInt64 {
			return 0, txn.ErrOverflow
		}
		return -int64(o - n), nil
	}
	return 0, fmt.Errorf("sqlgms: delta on %T", old)
}

func (e *editor) Delete(ctx *sql.Context, row sql.Row) error {
	tx, err := txnOf(ctx)
	if err != nil {
		return err
	}
	vals, err := toCoreRow(row)
	if err != nil {
		return err
	}
	key, err := e.stored(tx, vals)
	if err != nil {
		return err
	}
	if err := tx.Delete(e.t.def.id, key); errors.Is(err, txn.ErrNotFound) {
		return sql.ErrDeleteRowNotFound.New()
	} else {
		return err
	}
}

// Truncate deletes every row in the statement's transaction. The engine
// also routes DELETE without WHERE here; scanning the whole key range
// makes concurrent inserts conflict, as for any other full delete.
func (t *Table) Truncate(ctx *sql.Context) (int, error) {
	tx, err := txnOf(ctx)
	if err != nil {
		return 0, err
	}
	recs, err := tx.ScanRows(t.def.id, -1, "", "", 0)
	if err != nil {
		return 0, err
	}
	for _, r := range recs {
		if err := tx.Delete(t.def.id, r.PK); err != nil {
			return 0, err
		}
	}
	return len(recs), nil
}
