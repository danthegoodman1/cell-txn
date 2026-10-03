package sqlgms

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/expression"
	"github.com/dolthub/go-mysql-server/sql/plan"
	"github.com/dolthub/go-mysql-server/sql/planbuilder"
	"github.com/dolthub/go-mysql-server/sql/transform"
	"github.com/dolthub/go-mysql-server/sql/types"
	ast "github.com/dolthub/vitess/go/vt/sqlparser"
)

// PlanCache reuses analyzed plans for statements that differ from an
// earlier one only in their literals, so they skip go-mysql-server's plan
// building and analysis. It caches transaction control, SELECTs and
// UPDATEs that pin every primary-key column to a literal, and INSERTs of
// literal rows into tables without an auto-increment column; everything
// else takes the engine's full path.
//
// A hit copies the cached plan, rebuilds its primary-key lookup from the
// new literals with go-mysql-server's range builder, and swaps the new
// literals into an UPDATE's SET expressions or an INSERT's rows. The key
// is the statement with its literals elided, each literal's type, the
// current database, the connection collation, sql_mode and the schema
// version. The analyzer picks an INSERT's first auto-generated row from
// its literals, a choice the plan doesn't show, so tables with an
// auto-increment column stay uncached.
//
// With Verify set, as in the simulator and tests, every new template must
// rebuild its own plan from the same literals, and every hit also runs the
// full analysis and must produce the same plan.
type PlanCache struct {
	*sqle.Engine
	p      *Provider
	Verify bool
	plans  sync.Map // key -> *template
	size   atomic.Int64
	hits   atomic.Int64
	// shapes holds the parsed shape of each statement text run with
	// bindings, so prepared executions skip parsing.
	shapes sync.Map // sql_mode \x00 text -> *preparedShape
}

type preparedShape struct {
	stmt ast.Statement
	sh   shape
	ok   bool
}

// Hits reports how many statements ran a cached plan.
func (c *PlanCache) Hits() int64 { return c.hits.Load() }

// maxPlans bounds the cache; past it, new shapes take the full path.
const maxPlans = 1 << 14

// NewPlanCache returns a cache in front of e, an engine over p.
func NewPlanCache(e *sqle.Engine, p *Provider) *PlanCache {
	return &PlanCache{Engine: e, p: p}
}

// Query runs query, through a cached plan when one fits.
func (c *PlanCache) Query(ctx *sql.Context, query string) (sql.Schema, sql.RowIter, *sql.QueryFlags, error) {
	stmt, _, rest, err := c.Parser.ParseWithOptions(ctx, query, ';', false, sql.LoadSqlMode(ctx).ParserOptions())
	if err != nil || rest != "" {
		return c.Engine.Query(ctx, query)
	}
	n, flags, err := c.Plan(ctx, query, stmt)
	if err != nil {
		return nil, nil, nil, err
	}
	if n == nil {
		return c.Engine.QueryWithBindings(ctx, query, stmt, nil, nil)
	}
	starting(ctx, n)
	return c.PrepQueryPlanForExecution(ctx, query, n, flags)
}

// QueryWithBindings runs a plain query as Query does and a query with
// bindings, as from a prepared statement, through a cached plan when one
// fits; anything with a parsed form or flags takes the engine's full path.
func (c *PlanCache) QueryWithBindings(ctx *sql.Context, query string, parsed ast.Statement, bindings map[string]ast.Expr, qFlags *sql.QueryFlags) (sql.Schema, sql.RowIter, *sql.QueryFlags, error) {
	if parsed != nil || qFlags != nil {
		return c.Engine.QueryWithBindings(ctx, query, parsed, bindings, qFlags)
	}
	if len(bindings) == 0 {
		return c.Query(ctx, query)
	}
	n, flags, err := c.PlanBound(ctx, query, bindings)
	if err != nil {
		return nil, nil, nil, err
	}
	if n == nil {
		return c.Engine.QueryWithBindings(ctx, query, nil, bindings, nil)
	}
	starting(ctx, n)
	return c.PrepQueryPlanForExecution(ctx, query, n, flags)
}

// PlanBound is Plan for a query run with bindings. It parses each query
// text once.
func (c *PlanCache) PlanBound(ctx *sql.Context, query string, bindings map[string]ast.Expr) (sql.Node, *sql.QueryFlags, error) {
	mode := sql.LoadSqlMode(ctx)
	k := mode.String() + "\x00" + query
	v, ok := c.shapes.Load(k)
	if !ok {
		p := &preparedShape{}
		stmt, _, rest, err := c.Parser.ParseWithOptions(ctx, query, ';', false, mode.ParserOptions())
		if err == nil && rest == "" {
			p.stmt = stmt
			p.sh, p.ok = shapeOf(stmt)
		}
		if c.size.Load() < maxPlans {
			c.shapes.Store(k, p)
		}
		v = p
	}
	p := v.(*preparedShape)
	if !p.ok {
		return nil, nil, nil
	}
	return c.plan(ctx, query, p.stmt, p.sh, bindings)
}

// starting does the bookkeeping the engine does for a statement it plans:
// counting it and clearing the previous statement's warnings.
func starting(ctx *sql.Context, n sql.Node) {
	sql.IncrementStatusVariable(ctx, "Questions", 1)
	if plan.NodeRepresentsSelect(n) {
		sql.IncrementStatusVariable(ctx, "Com_select", 1)
	}
	ctx.ClearWarnings()
}

// Plan returns the analyzed plan for stmt from the cache, or nil when the
// statement must take the full path. A miss analyzes the statement to
// cache its plan and still returns nil. Errors come only from Verify.
func (c *PlanCache) Plan(ctx *sql.Context, query string, stmt ast.Statement) (sql.Node, *sql.QueryFlags, error) {
	sh, ok := shapeOf(stmt)
	if !ok {
		return nil, nil, nil
	}
	return c.plan(ctx, query, stmt, sh, nil)
}

// plan looks up stmt's cached plan; a placeholder in its shape takes its
// value from bindings.
func (c *PlanCache) plan(ctx *sql.Context, query string, stmt ast.Statement, sh shape, bindings map[string]ast.Expr) (sql.Node, *sql.QueryFlags, error) {
	// The analyzer rejects writes in read-only transactions, and checks
	// privileges when the server has users; cached plans skip both.
	if tx := ctx.GetTransaction(); (tx != nil && tx.IsReadOnly()) || c.Analyzer.Catalog.MySQLDb.Enabled() {
		return nil, nil, nil
	}
	coll := ctx.GetCollation()
	lits := make([]sql.Expression, len(sh.lits))
	key := make([]byte, 0, len(sh.text)+64)
	key = append(key, sh.text...)
	key = append(key, 0)
	key = append(key, ctx.GetCurrentDatabase()...)
	key = append(key, 0)
	key = strconv.AppendUint(key, uint64(coll), 10)
	key = append(key, 0)
	key = append(key, sql.LoadSqlMode(ctx).String()...)
	key = append(key, 0)
	key = strconv.AppendUint(key, c.p.schema.Load(), 10)
	for i, v := range sh.lits {
		if v.Type == ast.ValArg {
			b, isVal := bindings[strings.TrimPrefix(string(v.Val), ":")].(*ast.SQLVal)
			if !isVal {
				return nil, nil, nil
			}
			v = b
		}
		lit, ok := literal(v, coll)
		if !ok {
			return nil, nil, nil
		}
		lits[i] = lit
		key = append(key, 0)
		key = append(key, lit.Type().String()...)
	}
	k := string(key)

	v, ok := c.plans.Load(k)
	if !ok {
		t, err := c.build(ctx, query, stmt, sh, lits, bindings)
		if err != nil {
			return nil, nil, err
		}
		if c.size.Load() < maxPlans {
			if _, loaded := c.plans.LoadOrStore(k, t); !loaded {
				c.size.Add(1)
			}
		}
		return nil, nil, nil
	}
	t := v.(*template)
	if t.plan == nil {
		return nil, nil, nil
	}
	n, err := t.instantiate(ctx, lits)
	if err != nil || n == nil {
		return nil, nil, nil
	}
	if c.Verify {
		want, _, err := c.analyze(ctx, query, stmt, bindings)
		if err != nil {
			return nil, nil, fmt.Errorf("plan cache: %q: full analysis failed where the cache did not: %w", query, err)
		}
		if got, w := sql.DebugString(n), sql.DebugString(want); got != w {
			return nil, nil, fmt.Errorf("plan cache: %q: cached plan\n%s\ndiffers from analyzed plan\n%s", query, got, w)
		}
	}
	c.hits.Add(1)
	flags := t.flags
	return n, &flags, nil
}

// analyze builds and analyzes stmt with bindings as the engine does.
func (c *PlanCache) analyze(ctx *sql.Context, query string, stmt ast.Statement, bindings map[string]ast.Expr) (sql.Node, *sql.QueryFlags, error) {
	b := planbuilder.New(ctx, c.Analyzer.Catalog, c.EventScheduler, c.Parser)
	if len(bindings) > 0 {
		b.SetBindings(bindings)
	}
	bound, flags, err := b.BindOnly(stmt, query, &sql.QueryFlags{})
	if err != nil {
		return nil, nil, err
	}
	n, err := c.Analyzer.Analyze(ctx, bound, nil, flags)
	return n, flags, err
}

// literal builds an integer or string literal as the plan builder does:
// an integer takes the narrowest type that holds it, a string is longtext
// in the connection collation. Integers too wide for 64 bits, which the
// builder makes decimals, report false.
func literal(v *ast.SQLVal, coll sql.CollationID) (*expression.Literal, bool) {
	s := string(v.Val)
	if v.Type == ast.StrVal {
		return expression.NewLiteral(s, types.CreateLongText(coll)), true
	}
	if i, err := strconv.ParseInt(s, 10, 8); err == nil {
		return expression.NewLiteral(int8(i), types.Int8), true
	}
	if u, err := strconv.ParseUint(s, 10, 8); err == nil {
		return expression.NewLiteral(uint8(u), types.Uint8), true
	}
	if i, err := strconv.ParseInt(s, 10, 16); err == nil {
		return expression.NewLiteral(int16(i), types.Int16), true
	}
	if u, err := strconv.ParseUint(s, 10, 16); err == nil {
		return expression.NewLiteral(uint16(u), types.Uint16), true
	}
	if i, err := strconv.ParseInt(s, 10, 32); err == nil {
		return expression.NewLiteral(int32(i), types.Int32), true
	}
	if u, err := strconv.ParseUint(s, 10, 32); err == nil {
		return expression.NewLiteral(uint32(u), types.Uint32), true
	}
	if i, err := strconv.ParseInt(s, 10, 64); err == nil {
		return expression.NewLiteral(i, types.Int64), true
	}
	if u, err := strconv.ParseUint(s, 10, 64); err == nil {
		return expression.NewLiteral(u, types.Uint64), true
	}
	return nil, false
}

type stmtKind int

const (
	txnControl stmtKind = iota + 1
	pointSelect
	pointUpdate
	insertValues
)

// Cacheable reports whether the plan cache covers stmt's shape.
func Cacheable(stmt ast.Statement) bool {
	_, ok := shapeOf(stmt)
	return ok
}

// shape is a cacheable statement: its text with each literal or
// placeholder printed as ?,
// its literals in print order, and for a point SELECT or UPDATE the
// literal each WHERE equality compares its column to.
type shape struct {
	kind  stmtKind
	text  string
	lits  []*ast.SQLVal
	where map[string]int // lowercased column -> index into lits
}

// shapeOf reports whether stmt is a cacheable shape.
func shapeOf(stmt ast.Statement) (shape, bool) {
	sh := shape{}
	var table string
	var where *ast.Where
	switch s := stmt.(type) {
	case *ast.Begin, *ast.Commit, *ast.Rollback:
		sh.kind = txnControl
	case *ast.Select:
		if s.With != nil || len(s.GroupBy) > 0 || s.Having != nil || len(s.Window) > 0 || len(s.OrderBy) > 0 ||
			s.Limit != nil || s.Into != nil || s.QueryOpts != (ast.QueryOpts{}) {
			return sh, false
		}
		var ok bool
		if table, ok = soleTable(s.From); !ok {
			return sh, false
		}
		for _, se := range s.SelectExprs {
			ae, ok := se.(*ast.AliasedExpr)
			if !ok || !ae.As.IsEmpty() {
				return sh, false
			}
			if col, ok := ae.Expr.(*ast.ColName); !ok || !ofTable(col, table) {
				return sh, false
			}
		}
		sh.kind, where = pointSelect, s.Where
	case *ast.Update:
		if s.With != nil || s.Ignore != "" || len(s.OrderBy) > 0 || s.Limit != nil || len(s.Returning) > 0 {
			return sh, false
		}
		var ok bool
		if table, ok = soleTable(s.TableExprs); !ok {
			return sh, false
		}
		for _, a := range s.Exprs {
			if !ofTable(a.Name, table) || !setValue(a.Expr, table) {
				return sh, false
			}
		}
		sh.kind, where = pointUpdate, s.Where
	case *ast.Insert:
		if s.Action != ast.InsertStr || s.Ignore != "" || s.With != nil || len(s.Partitions) > 0 || len(s.Returning) > 0 ||
			len(s.OnDup) > 0 || !s.Table.DbQualifier.IsEmpty() || !s.Table.SchemaQualifier.IsEmpty() {
			return sh, false
		}
		rows, ok := s.Rows.(*ast.AliasedValues)
		if !ok || !rows.As.IsEmpty() || len(rows.Columns) > 0 {
			return sh, false
		}
		for _, tuple := range rows.Values {
			for _, e := range tuple {
				switch e.(type) {
				case *ast.SQLVal, *ast.NullVal:
				default:
					return sh, false
				}
			}
		}
		sh.kind = insertValues
	default:
		return sh, false
	}

	// Print the statement with literals elided, collecting them in order.
	index := map[*ast.SQLVal]int{}
	buf := ast.NewTrackedBuffer(func(buf *ast.TrackedBuffer, node ast.SQLNode) {
		if v, ok := node.(*ast.SQLVal); ok {
			index[v] = len(sh.lits)
			sh.lits = append(sh.lits, v)
			buf.WriteString("?")
			return
		}
		node.Format(buf)
	})
	buf.Myprintf("%v", stmt)
	sh.text = buf.String()
	for _, v := range sh.lits {
		if v.Type != ast.IntVal && v.Type != ast.StrVal && v.Type != ast.ValArg {
			return sh, false
		}
	}
	switch sh.kind {
	case txnControl:
		return sh, len(sh.lits) == 0
	case insertValues:
		return sh, true
	}
	if where == nil || where.Type != ast.WhereStr {
		return sh, false
	}
	sh.where = map[string]int{}
	if !equalities(where.Expr, table, index, sh.where) {
		return sh, false
	}
	return sh, true
}

// soleTable returns the one unaliased table a statement reads.
func soleTable(from ast.TableExprs) (string, bool) {
	if len(from) != 1 {
		return "", false
	}
	at, ok := from[0].(*ast.AliasedTableExpr)
	if !ok || !at.As.IsEmpty() || at.Partitions != nil || at.Hints != nil || at.AsOf != nil || at.Lateral {
		return "", false
	}
	tn, ok := at.Expr.(ast.TableName)
	if !ok || !tn.DbQualifier.IsEmpty() || !tn.SchemaQualifier.IsEmpty() {
		return "", false
	}
	return strings.ToLower(tn.Name.String()), true
}

func ofTable(col *ast.ColName, table string) bool {
	q := col.Qualifier
	return col.StoredProcVal == nil && q.DbQualifier.IsEmpty() && q.SchemaQualifier.IsEmpty() &&
		(q.Name.IsEmpty() || strings.EqualFold(q.Name.String(), table))
}

// setValue reports whether a SET value is a literal, NULL, a column, or a
// column plus or minus an integer literal.
func setValue(e ast.Expr, table string) bool {
	switch x := e.(type) {
	case *ast.SQLVal, *ast.NullVal:
		return true
	case *ast.ColName:
		return ofTable(x, table)
	case *ast.BinaryExpr:
		col, ok := x.Left.(*ast.ColName)
		v, isLit := x.Right.(*ast.SQLVal)
		return ok && isLit && ofTable(col, table) && (v.Type == ast.IntVal || v.Type == ast.ValArg) &&
			(x.Operator == ast.PlusStr || x.Operator == ast.MinusStr)
	}
	return false
}

// equalities records each conjunct col = literal of e in where, and
// reports whether e is only such conjuncts on distinct columns.
func equalities(e ast.Expr, table string, index map[*ast.SQLVal]int, where map[string]int) bool {
	switch x := e.(type) {
	case *ast.AndExpr:
		return equalities(x.Left, table, index, where) && equalities(x.Right, table, index, where)
	case *ast.ComparisonExpr:
		if x.Operator != ast.EqualStr || x.Escape != nil {
			return false
		}
		col, ok := x.Left.(*ast.ColName)
		v, isLit := x.Right.(*ast.SQLVal)
		if !ok || !isLit {
			col, ok = x.Right.(*ast.ColName)
			v, isLit = x.Left.(*ast.SQLVal)
		}
		if !ok || !isLit || !ofTable(col, table) {
			return false
		}
		name := col.Name.Lowered()
		if _, dup := where[name]; dup {
			return false
		}
		where[name] = index[v]
		return true
	}
	return false
}

// template is a cached plan and how to rebuild it for new literals. A
// template with a nil plan marks a shape whose plan the cache can't
// rebuild.
type template struct {
	plan  sql.Node
	flags sql.QueryFlags

	// For point SELECTs and UPDATEs: the primary-key lookup, the literal
	// for each of its columns, and the nodes above it.
	ita     *plan.IndexedTableAccess
	cols    []string // the index's column expressions, lowercased
	keyLits []int
	proj    *plan.Project      // a SELECT's projection, if any
	upd     *plan.Update       // an UPDATE
	src     *plan.UpdateSource // its SET expressions; their literals are lits[:nset]
	nset    int

	// For INSERTs: the rows, whose non-NULL literals are lits, and the
	// projection over them, if any.
	ins     *plan.InsertInto
	insProj *plan.Project
	vals    *plan.Values
}

// build analyzes stmt and returns its template, whose plan is nil unless
// the analyzed plan has a shape instantiate can rebuild. With Verify set,
// the template must rebuild the analyzed plan from the same literals.
func (c *PlanCache) build(ctx *sql.Context, query string, stmt ast.Statement, sh shape, lits []sql.Expression, bindings map[string]ast.Expr) (*template, error) {
	n, flags, err := c.analyze(ctx, query, stmt, bindings)
	if err != nil {
		return &template{}, nil
	}
	t := c.match(n, *flags, sh, lits)
	if c.Verify && t.plan != nil {
		if got, err := t.instantiate(ctx, lits); err == nil && got != nil && sql.DebugString(got) != sql.DebugString(n) {
			return nil, fmt.Errorf("plan cache: %q: template rebuilds\n%s\ninstead of\n%s", query, sql.DebugString(got), sql.DebugString(n))
		}
	}
	return t, nil
}

// match returns the template for analyzed plan n.
func (c *PlanCache) match(n sql.Node, flags sql.QueryFlags, sh shape, lits []sql.Expression) *template {
	t := &template{flags: flags}
	if sh.kind == txnControl {
		switch n.(type) {
		case *plan.StartTransaction, *plan.Commit, *plan.Rollback:
			t.plan = n
		}
		return t
	}

	var child sql.Node
	switch x := n.(type) {
	case *plan.Project:
		if sh.kind != pointSelect {
			return t
		}
		t.proj, child = x, x.Child
	case *plan.IndexedTableAccess:
		if sh.kind != pointSelect {
			return t
		}
		child = x
	case *plan.Update:
		src, ok := x.Child.(*plan.UpdateSource)
		if sh.kind != pointUpdate || !ok {
			return t
		}
		t.upd, t.src, child = x, src, src.Child
		t.nset = len(lits) - len(sh.where)
		if !sameLiterals(src.Expressions(), lits[:t.nset]) {
			return t
		}
	case *plan.InsertInto:
		if sh.kind != insertValues || x.IsReplace || len(x.OnDupExprs) > 0 || len(x.Returning) > 0 {
			return t
		}
		for _, col := range x.Destination.Schema() {
			if col.AutoIncrement {
				return t
			}
		}
		src := x.Source
		if p, ok := src.(*plan.Project); ok {
			t.insProj, src = p, p.Child
		}
		vals, ok := src.(*plan.Values)
		if !ok || !sameLiterals(nonNull(vals.Expressions()), lits) {
			return t
		}
		for _, e := range vals.Expressions() {
			if _, ok := e.(*expression.Literal); !ok {
				return t
			}
		}
		t.ins, t.vals, t.plan = x, vals, n
		return t
	default:
		return t
	}

	ita, ok := child.(*plan.IndexedTableAccess)
	if !ok || !ita.IsStatic() {
		return t
	}
	if _, ok := ita.TableNode.(*plan.ResolvedTable); !ok {
		return t
	}
	idx := ita.Index()
	if idx == nil || !idx.IsUnique() || !strings.EqualFold(idx.ID(), "PRIMARY") || len(idx.Expressions()) != len(sh.where) {
		return t
	}
	for _, e := range idx.Expressions() {
		e = strings.ToLower(e)
		col := e[strings.LastIndexByte(e, '.')+1:]
		i, ok := sh.where[col]
		if !ok {
			return t
		}
		t.cols = append(t.cols, e)
		t.keyLits = append(t.keyLits, i)
	}
	t.ita, t.plan = ita, n
	return t
}

// sameLiterals reports whether the literals in exprs, in order, are exactly
// lits.
func sameLiterals(exprs []sql.Expression, lits []sql.Expression) bool {
	var found []*expression.Literal
	for _, e := range exprs {
		transform.InspectExpr(e, func(e sql.Expression) bool {
			if l, ok := e.(*expression.Literal); ok {
				found = append(found, l)
			}
			return false
		})
	}
	if len(found) != len(lits) {
		return false
	}
	for i, l := range found {
		want := lits[i].(*expression.Literal)
		if !l.Type().Equals(want.Type()) || l.Value() != want.Value() {
			return false
		}
	}
	return true
}

var errNoLookup = errors.New("plan cache: literals give no single-range lookup")

// nonNull drops literal NULLs, which are part of a statement's shape.
func nonNull(exprs []sql.Expression) []sql.Expression {
	var out []sql.Expression
	for _, e := range exprs {
		if l, ok := e.(*expression.Literal); !ok || l.Value() != nil {
			out = append(out, e)
		}
	}
	return out
}

// instantiate returns the template's plan for new literals.
func (t *template) instantiate(ctx *sql.Context, lits []sql.Expression) (sql.Node, error) {
	if t.ins != nil {
		exprs := t.vals.Expressions()
		rows := make([]sql.Expression, len(exprs))
		i := 0
		for j, e := range exprs {
			if l := e.(*expression.Literal); l.Value() == nil {
				rows[j] = e
			} else {
				rows[j] = lits[i]
				i++
			}
		}
		src, err := t.vals.WithExpressions(rows...)
		if err != nil {
			return nil, err
		}
		if t.insProj != nil {
			if src, err = t.insProj.WithChildren(src); err != nil {
				return nil, err
			}
		}
		return t.ins.WithSource(src), nil
	}
	if t.ita == nil {
		return t.plan, nil
	}
	b := sql.NewMySQLIndexBuilder(t.ita.Index())
	for i, col := range t.cols {
		v, err := lits[t.keyLits[i]].Eval(ctx, nil)
		if err != nil {
			return nil, err
		}
		b.Equals(ctx, col, v)
	}
	ranges := b.Ranges(ctx)
	if len(ranges) != 1 {
		return nil, errNoLookup
	}
	if empty, err := ranges[0].IsEmpty(); err != nil || empty {
		return nil, errNoLookup
	}
	ita, err := plan.NewStaticIndexedAccessForTableNode(ctx, t.ita.TableNode, sql.NewIndexLookup(t.ita.Index(), ranges, false, false, false, false))
	if err != nil {
		return nil, err
	}
	switch {
	case t.proj != nil:
		return t.proj.WithChildren(ita)
	case t.upd != nil:
		i := 0
		exprs := make([]sql.Expression, len(t.src.UpdateExprs))
		for j, e := range t.src.UpdateExprs {
			exprs[j], _, err = transform.Expr(e, func(e sql.Expression) (sql.Expression, transform.TreeIdentity, error) {
				if _, ok := e.(*expression.Literal); ok {
					i++
					return lits[i-1], transform.NewTree, nil
				}
				return e, transform.SameTree, nil
			})
			if err != nil {
				return nil, err
			}
		}
		src, err := t.src.WithExpressions(exprs...)
		if err != nil {
			return nil, err
		}
		if src, err = src.WithChildren(ita); err != nil {
			return nil, err
		}
		return t.upd.WithChildren(src)
	}
	return ita, nil
}
