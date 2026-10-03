package sqlgms

import (
	"fmt"

	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/expression"
	"github.com/dolthub/go-mysql-server/sql/plan"
	"github.com/dolthub/go-mysql-server/sql/rowexec"
	"github.com/dolthub/go-mysql-server/sql/transform"
	"github.com/dolthub/go-mysql-server/sql/types"

	"cell-tnx/txn"
)

// builder annotates every final plan before execution. The engine passes
// each statement's plan, and each subquery's, through Build, so this is
// the one hook that sees them all.
type builder struct{ inner sql.NodeExecBuilder }

// NewBuilder returns the exec builder that annotates plans with column
// access.
func NewBuilder() sql.NodeExecBuilder {
	return &builder{inner: rowexec.DefaultBuilder}
}

func (b *builder) Build(ctx *sql.Context, n sql.Node, r sql.Row) (sql.RowIter, error) {
	if unsupported(n) {
		return nil, fmt.Errorf("sqlgms: ALTER TABLE column, key and rename changes are not supported")
	}
	n, err := annotate(n)
	if err != nil {
		return nil, err
	}
	return b.inner.Build(ctx, n, r)
}

// unsupported reports schema changes the store cannot make. The engine's
// executors for some of them assume an AlterableTable without checking.
func unsupported(n sql.Node) bool {
	found := false
	transform.Inspect(n, func(n sql.Node) bool {
		switch n.(type) {
		case *plan.AlterDefaultSet, *plan.AlterDefaultDrop, *plan.AlterPK, *plan.AddColumn, *plan.DropColumn,
			*plan.RenameColumn, *plan.ModifyColumn, *plan.AlterTableCollation, *plan.RenameTable:
			found = true
		}
		return !found
	})
	return found
}

// annotation collects one plan's column access per table node.
type annotation struct {
	tables map[sql.TableId]*Table
	reads  map[sql.TableId]txn.ColMask
	writes map[sql.TableId]txn.ColMask
	deltas map[sql.TableId]txn.ColMask
	whole  map[sql.TableId]bool // read every column
	opaque bool                 // a node we cannot annotate safely
}

// annotate attaches each table node's read, write and delta columns to its
// table, compiles and strips CHECKs on our tables, and leaves the plan
// unannotated (every column read and written) when it contains writers
// whose tables we cannot reach.
func annotate(n sql.Node) (sql.Node, error) {
	a := &annotation{
		tables: map[sql.TableId]*Table{},
		reads:  map[sql.TableId]txn.ColMask{},
		writes: map[sql.TableId]txn.ColMask{},
		deltas: map[sql.TableId]txn.ColMask{},
		whole:  map[sql.TableId]bool{},
	}
	walkNodes(n, a.findTables)
	if len(a.tables) == 0 {
		return n, nil
	}
	walkNodes(n, a.collect)
	strip := !a.opaque
	return a.apply(n, strip)
}

// walkNodes visits every node, including INSERT sources.
func walkNodes(n sql.Node, f func(sql.Node)) {
	if n == nil {
		return
	}
	f(n)
	if ii, ok := n.(*plan.InsertInto); ok {
		walkNodes(ii.Source, f)
	}
	for _, c := range n.Children() {
		walkNodes(c, f)
	}
}

func underlying(t sql.Table) *Table {
	for {
		switch x := t.(type) {
		case *Table:
			return x
		case sql.TableWrapper:
			t = x.Underlying()
		default:
			return nil
		}
	}
}

func (a *annotation) findTables(n sql.Node) {
	switch x := n.(type) {
	case *plan.ResolvedTable:
		if t := underlying(x.Table); t != nil {
			a.tables[x.Id()] = t
		}
	case *plan.IndexedTableAccess:
		if t := underlying(x.Table); t != nil {
			a.tables[x.Id()] = t
		}
	case *plan.UpdateJoin, *plan.TriggerExecutor, *plan.ForeignKeyHandler, *plan.LoadData:
		a.opaque = true
	case *plan.Update:
		a.opaque = a.opaque || x.IsJoin
	}
}

// collect records the columns read and written through each table node.
func (a *annotation) collect(n sql.Node) {
	switch x := n.(type) {
	case *plan.Update:
		return // its checks are not reads; its child is visited separately
	case *plan.InsertInto:
		for _, e := range x.OnDupExprs {
			a.expr(e)
		}
		return
	case *plan.UpdateSource:
		for _, e := range x.UpdateExprs {
			a.set(e)
		}
		return
	}
	if ex, ok := n.(sql.Expressioner); ok {
		for _, e := range ex.Expressions() {
			a.expr(e)
		}
	}
}

// set handles one UPDATE assignment: its target is a write, its value a
// read, unless it is col = col ± constant, a delta candidate.
func (a *annotation) set(e sql.Expression) {
	sf, ok := e.(*expression.SetField)
	if !ok {
		a.expr(e)
		return
	}
	target, ok := sf.LeftChild.(*expression.GetField)
	if !ok {
		a.expr(e)
		return
	}
	id := target.TableId()
	t := a.tables[id]
	if t == nil {
		a.expr(sf.RightChild)
		return
	}
	col := t.def.colIndex(target.Name())
	if col < 0 {
		a.whole[id] = true
		a.expr(sf.RightChild)
		return
	}
	a.writes[id] |= txn.Col(col)
	if ar, ok := sf.RightChild.(*expression.Arithmetic); ok && (ar.Op == "+" || ar.Op == "-") {
		for i, side := range []sql.Expression{ar.LeftChild, ar.RightChild} {
			other := []sql.Expression{ar.RightChild, ar.LeftChild}[i]
			gf, ok := side.(*expression.GetField)
			if !ok || gf.TableId() != id || t.def.colIndex(gf.Name()) != col || (i == 1 && ar.Op == "-") || !constant(other) {
				continue
			}
			a.deltas[id] |= txn.Col(col)
			a.expr(other)
			return
		}
	}
	a.expr(sf.RightChild)
}

// constant reports whether e is a literal or bind variable.
func constant(e sql.Expression) bool {
	switch x := e.(type) {
	case *expression.Literal, *expression.BindVar:
		return true
	case *expression.UnaryMinus:
		return constant(x.Child)
	}
	return false
}

// expr records every column e reads, including inside subqueries.
func (a *annotation) expr(e sql.Expression) {
	if e == nil {
		return
	}
	switch x := e.(type) {
	case *expression.GetField:
		if t := a.tables[x.TableId()]; t != nil {
			if c := t.def.colIndex(x.Name()); c >= 0 {
				a.reads[x.TableId()] |= txn.Col(c)
			} else {
				a.whole[x.TableId()] = true
			}
		}
	case *plan.Subquery:
		walkNodes(x.Query, a.collect)
	}
	for _, c := range e.Children() {
		a.expr(c)
	}
}

// access returns the final access for a table node. A delta survives only
// on an integer column the statement reads nowhere else and that no key,
// index or generated column depends on. Other assigned columns count as
// read: the engine skips writing a row whose new image equals the old, so
// the old values of assigned columns decide whether the write happens.
func (a *annotation) access(id sql.TableId) *access {
	t := a.tables[id]
	if a.opaque || a.whole[id] || t.def.generated() {
		return nil
	}
	acc := &access{read: a.reads[id] | a.writes[id]&^a.deltas[id], write: a.writes[id]}
	keyed := t.def.mask(t.def.pk)
	t.def.mu.Lock()
	for _, ix := range t.def.indexes {
		keyed |= t.def.mask(ix.cols)
	}
	t.def.mu.Unlock()
	a.deltas[id].Each(func(c int) {
		if acc.read&txn.Col(c) == 0 && keyed&txn.Col(c) == 0 && types.IsInteger(t.def.sch[c].Type) {
			acc.delta |= txn.Col(c)
		} else {
			acc.read |= txn.Col(c)
		}
	})
	return acc
}

// apply replaces each of our tables with its annotated view, and moves
// CHECKs on our tables from the plan into the store.
func (a *annotation) apply(n sql.Node, strip bool) (sql.Node, error) {
	out, _, err := transform.Node(n, func(n sql.Node) (sql.Node, transform.TreeIdentity, error) {
		switch x := n.(type) {
		case *plan.ResolvedTable:
			t := a.tables[x.Id()]
			if t == nil || !strip {
				return n, transform.SameTree, nil
			}
			nt, err := x.WithTable(t.with(a.access(x.Id())))
			return nt, transform.NewTree, err
		case *plan.IndexedTableAccess:
			t := a.tables[x.Id()]
			rt, ok := x.TableNode.(*plan.ResolvedTable)
			if t == nil || !strip || !ok {
				return n, transform.SameTree, nil
			}
			acc := a.access(x.Id())
			nita := *x
			nita.Table = t.with(acc)
			base := underlying(rt.Table)
			if base == nil {
				return n, transform.SameTree, nil
			}
			tn, err := rt.WithTable(base.with(acc))
			if err != nil {
				return nil, transform.SameTree, err
			}
			nita.TableNode = tn
			return &nita, transform.NewTree, nil
		case *plan.Update:
			if t, err := plan.GetUpdatable(x.Child); err == nil {
				if ours := underlying(t); ours != nil && len(x.Checks()) > 0 {
					ours.def.compile(x.Checks())
					if strip {
						return x.WithChecks(nil), transform.NewTree, nil
					}
				}
			}
		case *plan.InsertInto:
			src, err := a.apply(x.Source, strip)
			if err != nil {
				return nil, transform.SameTree, err
			}
			ii := x.WithSource(src)
			if t, err := plan.GetInsertable(x.Destination); err == nil {
				if ours := underlying(t); ours != nil && len(x.Checks()) > 0 {
					ours.def.compile(x.Checks())
					if strip {
						return ii.WithChecks(nil), transform.NewTree, nil
					}
				}
			}
			return ii, transform.NewTree, nil
		}
		return n, transform.SameTree, nil
	})
	return out, err
}
