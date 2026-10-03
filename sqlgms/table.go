package sqlgms

import (
	"errors"
	"fmt"
	"io"
	"slices"
	"strings"

	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/expression"

	"cell-tnx/keys"
	"cell-tnx/txn"
)

// access is what one statement does with a table: the columns it reads,
// the columns it assigns, and the assigned columns it updates as deltas.
// A nil access reads and writes every column.
type access struct {
	read, write, delta txn.ColMask
}

// Table is a per-plan view of a table, optionally annotated with the
// statement's access and an index lookup.
type Table struct {
	def    *tableDef
	acc    *access
	lookup *sql.IndexLookup
}

var (
	_ sql.Table                 = (*Table)(nil)
	_ sql.PrimaryKeyTable       = (*Table)(nil)
	_ sql.InsertableTable       = (*Table)(nil)
	_ sql.UpdatableTable        = (*Table)(nil)
	_ sql.DeletableTable        = (*Table)(nil)
	_ sql.ReplaceableTable      = (*Table)(nil)
	_ sql.IndexAddressableTable = (*Table)(nil)
	_ sql.IndexedTable          = (*Table)(nil)
	_ sql.IndexAlterableTable   = (*Table)(nil)
	_ sql.CheckTable            = (*Table)(nil)
	_ sql.CheckAlterableTable   = (*Table)(nil)
	_ sql.AutoIncrementTable    = (*Table)(nil)
	_ sql.TemporaryTable        = (*Table)(nil)
	_ sql.TruncateableTable     = (*Table)(nil)
)

// IsTemporary reports false. Implementing it also spares the analyzer's
// READ ONLY transaction check, which calls it through a nil interface on
// tables that don't.
func (t *Table) IsTemporary() bool { return false }

func (t *Table) Name() string               { return t.def.name }
func (t *Table) String() string             { return t.def.name }
func (t *Table) Schema() sql.Schema         { return t.def.sch }
func (t *Table) Collation() sql.CollationID { return t.def.coll }
func (t *Table) PrimaryKeySchema() sql.PrimaryKeySchema {
	return sql.NewPrimaryKeySchema(t.def.sch, t.def.pk...)
}

// reads returns the columns this view tracks.
func (t *Table) reads() txn.ColMask {
	if t.acc == nil {
		return t.def.all()
	}
	return t.acc.read
}

func (t *Table) with(acc *access) *Table {
	c := *t
	c.acc = acc
	return &c
}

// txnOf returns the session's transaction.
func txnOf(ctx *sql.Context) (*txn.Txn, error) {
	if tx, ok := ctx.GetTransaction().(*Transaction); ok && tx != nil {
		return tx.t, nil
	}
	return nil, errors.New("sqlgms: no transaction")
}

// --- rows ---

type partition struct{ lookup *sql.IndexLookup }

func (partition) Key() []byte { return nil }

type partitionIter struct{ parts []sql.Partition }

func (p *partitionIter) Next(*sql.Context) (sql.Partition, error) {
	if len(p.parts) == 0 {
		return nil, io.EOF
	}
	part := p.parts[0]
	p.parts = p.parts[1:]
	return part, nil
}

func (p *partitionIter) Close(*sql.Context) error { return nil }

type rowIter struct{ rows []sql.Row }

func (r *rowIter) Next(*sql.Context) (sql.Row, error) {
	if len(r.rows) == 0 {
		return nil, io.EOF
	}
	row := r.rows[0]
	r.rows = r.rows[1:]
	return row, nil
}

func (r *rowIter) Close(*sql.Context) error { return nil }

func (t *Table) Partitions(*sql.Context) (sql.PartitionIter, error) {
	return &partitionIter{parts: []sql.Partition{partition{t.lookup}}}, nil
}

func (t *Table) LookupPartitions(_ *sql.Context, l sql.IndexLookup) (sql.PartitionIter, error) {
	return &partitionIter{parts: []sql.Partition{partition{&l}}}, nil
}

// PartitionRows reads every row, or the rows of the partition's lookup,
// eagerly, so a statement never reads its own later writes.
func (t *Table) PartitionRows(ctx *sql.Context, p sql.Partition) (sql.RowIter, error) {
	tx, err := txnOf(ctx)
	if err != nil {
		return nil, err
	}
	var recs []txn.Rec
	if l := p.(partition).lookup; l != nil {
		recs, err = t.lookupRecs(ctx, tx, *l)
	} else {
		recs, err = tx.ScanRows(t.def.id, -1, "", "", t.reads())
	}
	if err != nil {
		return nil, err
	}
	rows := make([]sql.Row, len(recs))
	for i, r := range recs {
		if rows[i], err = fromCoreRow(t.def.sch, r.Row); err != nil {
			return nil, err
		}
	}
	return &rowIter{rows}, nil
}

// lookupRecs reads the rows in an index lookup's ranges, in index order.
// Each range becomes a key interval plus an exact filter; a point on the
// whole primary key becomes a single-row read.
func (t *Table) lookupRecs(ctx *sql.Context, tx *txn.Txn, l sql.IndexLookup) ([]txn.Rec, error) {
	if l.IsEmptyRange {
		return nil, nil
	}
	ix, ok := l.Index.(*Index)
	if !ok {
		return nil, fmt.Errorf("sqlgms: foreign index %T", l.Index)
	}
	ranges, ok := l.Ranges.(sql.MySQLRangeCollection)
	if !ok {
		return nil, fmt.Errorf("sqlgms: unsupported ranges %T", l.Ranges)
	}
	track := t.reads() | t.def.mask(ix.cols)
	var out []txn.Rec
	for _, r := range ranges {
		lo, hi, point, err := rangeKeys(t.def.sch, ix.cols, r)
		if err != nil {
			return nil, err
		}
		if point && ix.primary {
			row, found, err := tx.GetRow(t.def.id, lo, track)
			if err != nil {
				return nil, err
			}
			if found {
				out = append(out, txn.Rec{PK: lo, Row: row})
			}
			continue
		}
		if lo == emptyRange {
			continue
		}
		recs, err := tx.ScanRows(t.def.id, ix.core, lo, hi, track)
		if err != nil {
			return nil, err
		}
		filter, err := ix.filter(r)
		if err != nil {
			return nil, err
		}
		for _, rec := range recs {
			row, err := fromCoreRow(t.def.sch, rec.Row)
			if err != nil {
				return nil, err
			}
			ok, err := sql.EvaluateCondition(ctx, filter, row)
			if err != nil {
				return nil, err
			}
			if sql.IsTrue(ok) {
				out = append(out, rec)
			}
		}
	}
	if l.IsReverse {
		slices.Reverse(out)
	}
	return out, nil
}

// emptyRange marks a range with no keys.
const emptyRange = "\xff\xff\xff\xffempty"

// rangeKeys turns one range into a key interval [lo, hi) that contains it:
// a prefix of point columns, then the first non-point column's bounds.
// point reports that every index column is pinned to one non-NULL value.
func rangeKeys(sch sql.Schema, cols []int, r sql.MySQLRange) (lo, hi string, point bool, err error) {
	var prefix []byte
	enc := func(c int, key any) ([]byte, error) {
		v, err := toCore(key)
		if err != nil {
			return nil, err
		}
		return appendKey(append([]byte(nil), prefix...), sch[c].Type, v)
	}
	for i, c := range cols {
		if i >= len(r) {
			break
		}
		ce := r[i]
		if b, ok := ce.LowerBound.(sql.Below); ok {
			if a, ok := ce.UpperBound.(sql.Above); ok {
				kb, err := enc(c, b.Key)
				if err != nil {
					return "", "", false, err
				}
				ka, err := enc(c, a.Key)
				if err != nil {
					return "", "", false, err
				}
				if string(kb) == string(ka) {
					prefix = kb
					if i == len(cols)-1 {
						return string(prefix), keys.Succ(string(prefix)), true, nil
					}
					continue
				}
			}
		}
		if _, ok := ce.LowerBound.(sql.BelowNull); ok {
			if _, ok := ce.UpperBound.(sql.AboveNull); ok {
				prefix = keys.AppendNull(prefix)
				continue
			}
		}
		var l, h []byte
		switch b := ce.LowerBound.(type) {
		case sql.BelowNull:
			l = prefix
		case sql.AboveNull:
			l = append(append([]byte(nil), prefix...), 0x02)
		case sql.Below:
			if l, err = enc(c, b.Key); err != nil {
				return "", "", false, err
			}
		case sql.Above:
			k, err := enc(c, b.Key)
			if err != nil {
				return "", "", false, err
			}
			if s := keys.Succ(string(k)); s != "" {
				l = []byte(s)
			} else {
				return emptyRange, "", false, nil
			}
		default:
			return emptyRange, "", false, nil
		}
		switch b := ce.UpperBound.(type) {
		case sql.AboveAll:
			h = []byte(keys.Succ(string(prefix)))
		case sql.Above:
			k, err := enc(c, b.Key)
			if err != nil {
				return "", "", false, err
			}
			h = []byte(keys.Succ(string(k)))
		case sql.Below:
			if h, err = enc(c, b.Key); err != nil {
				return "", "", false, err
			}
		case sql.AboveNull:
			h = append(append([]byte(nil), prefix...), 0x02)
		default:
			return emptyRange, "", false, nil
		}
		return string(l), string(h), false, nil
	}
	return string(prefix), keys.Succ(string(prefix)), false, nil
}

// --- indexes ---

// Index is the primary key or a secondary index.
type Index struct {
	def     *tableDef
	name    string
	cols    []int
	unique  bool
	primary bool
	core    int // core secondary index number; -1 for the primary key
}

var _ sql.OrderedIndex = (*Index)(nil)

func (i *Index) ID() string       { return i.name }
func (i *Index) Database() string { return i.def.db.name }
func (i *Index) Table() string    { return i.def.name }
func (i *Index) Expressions() []string {
	out := make([]string, len(i.cols))
	for j, c := range i.cols {
		out[j] = i.def.name + "." + i.def.sch[c].Name
	}
	return out
}
func (i *Index) IsUnique() bool                             { return i.unique }
func (i *Index) IsSpatial() bool                            { return false }
func (i *Index) IsFullText() bool                           { return false }
func (i *Index) IsVector() bool                             { return false }
func (i *Index) Comment() string                            { return "" }
func (i *Index) IndexType() string                          { return "BTREE" }
func (i *Index) IsGenerated() bool                          { return false }
func (i *Index) CanSupport(*sql.Context, ...sql.Range) bool { return true }
func (i *Index) CanSupportOrderBy(sql.Expression) bool      { return false }
func (i *Index) PrefixLengths() []uint16                    { return nil }
func (i *Index) Order() sql.IndexOrder                      { return sql.IndexOrderAsc }
func (i *Index) Reversible() bool                           { return true }
func (i *Index) ColumnExpressionTypes() []sql.ColumnExpressionType {
	out := make([]sql.ColumnExpressionType, len(i.cols))
	for j, e := range i.Expressions() {
		out[j] = sql.ColumnExpressionType{Expression: e, Type: i.def.sch[i.cols[j]].Type}
	}
	return out
}

// filter is the exact condition for one range, over full table rows.
func (i *Index) filter(r sql.MySQLRange) (sql.Expression, error) {
	exprs := make([]sql.Expression, len(i.cols))
	for j, c := range i.cols {
		col := i.def.sch[c]
		exprs[j] = expression.NewGetFieldWithTable(c, 0, col.Type, i.def.db.name, i.def.name, col.Name, col.Nullable)
	}
	return expression.NewRangeFilterExpr(exprs, []sql.MySQLRange{r})
}

func (t *Table) primary() *Index {
	return &Index{def: t.def, name: "PRIMARY", cols: t.def.pk, unique: true, primary: true, core: -1}
}

func (t *Table) GetIndexes(*sql.Context) ([]sql.Index, error) {
	var out []sql.Index
	if len(t.def.pk) > 0 {
		out = append(out, t.primary())
	}
	t.def.mu.Lock()
	defer t.def.mu.Unlock()
	for _, ix := range t.def.indexes {
		out = append(out, ix)
	}
	return out, nil
}

func (t *Table) IndexedAccess(_ *sql.Context, l sql.IndexLookup) sql.IndexedTable {
	c := *t
	c.lookup = &l
	return &c
}

func (t *Table) PreciseMatch() bool { return true }

// CreateIndex builds a secondary index from the committed rows.
func (t *Table) CreateIndex(ctx *sql.Context, d sql.IndexDef) error {
	if d.Constraint == sql.IndexConstraint_Fulltext || d.Constraint == sql.IndexConstraint_Spatial || d.Constraint == sql.IndexConstraint_Vector {
		return fmt.Errorf("sqlgms: index constraint %d is not supported", d.Constraint)
	}
	def := t.def
	var cols []int
	for _, c := range d.Columns {
		if c.Length > 0 {
			return fmt.Errorf("sqlgms: prefix indexes are not supported")
		}
		i := def.colIndex(c.Name)
		if i < 0 {
			return sql.ErrKeyColumnDoesNotExist.New(c.Name)
		}
		if err := appendable(def.sch[i].Type); err != nil {
			return err
		}
		cols = append(cols, i)
	}
	def.mu.Lock()
	defer def.mu.Unlock()
	for _, ix := range def.indexes {
		if strings.EqualFold(ix.name, d.Name) {
			return sql.ErrDuplicateKey.New(d.Name)
		}
	}
	sch := def.sch
	core := txn.Index{
		Name:   d.Name,
		Cols:   def.mask(cols),
		Unique: d.IsUnique(),
		Key: func(row []txn.Value) (string, bool) {
			k, nonNull, err := encodeKey(sch, cols, row)
			if err != nil {
				panic(err)
			}
			return k, nonNull
		},
		BucketPrefix: bucketPrefix(sch, cols),
	}
	if err := def.db.p.core.CreateIndex(def.id, core); err != nil {
		if errors.Is(err, txn.ErrDuplicateKey) {
			return sql.NewUniqueKeyErr(d.Name, false, nil)
		}
		return err
	}
	def.indexes = append(def.indexes, &Index{def: def, name: d.Name, cols: cols, unique: d.IsUnique(), core: len(def.indexes)})
	return def.db.p.ddl(ctx)
}

func (t *Table) DropIndex(ctx *sql.Context, name string) error {
	def := t.def
	def.mu.Lock()
	defer def.mu.Unlock()
	for i, ix := range def.indexes {
		if strings.EqualFold(ix.name, name) {
			def.db.p.core.DropIndex(def.id, ix.core)
			def.indexes = slices.Delete(def.indexes, i, i+1)
			for j, ix := range def.indexes {
				ix.core = j
			}
			return def.db.p.ddl(ctx)
		}
	}
	return sql.ErrIndexNotFound.New(name)
}

func (t *Table) RenameIndex(ctx *sql.Context, from, to string) error {
	def := t.def
	def.mu.Lock()
	defer def.mu.Unlock()
	for _, ix := range def.indexes {
		if strings.EqualFold(ix.name, from) {
			ix.name = to
			return def.db.p.ddl(ctx)
		}
	}
	return sql.ErrIndexNotFound.New(from)
}

// --- checks ---

func (t *Table) GetChecks(*sql.Context) ([]sql.CheckDefinition, error) {
	t.def.mu.Lock()
	defer t.def.mu.Unlock()
	return slices.Clone(t.def.checks), nil
}

func (t *Table) CreateCheck(ctx *sql.Context, c *sql.CheckDefinition) error {
	t.def.mu.Lock()
	defer t.def.mu.Unlock()
	cc := *c
	if cc.Name == "" {
		cc.Name = fmt.Sprintf("%s_chk_%d", t.def.name, len(t.def.checks)+1)
	}
	t.def.checks = append(t.def.checks, cc)
	t.def.version++
	return t.def.db.p.ddl(ctx)
}

func (t *Table) DropCheck(_ *sql.Context, name string) error {
	t.def.mu.Lock()
	defer t.def.mu.Unlock()
	for i, c := range t.def.checks {
		if strings.EqualFold(c.Name, name) {
			t.def.checks = slices.Delete(t.def.checks, i, i+1)
			t.def.version++
			return nil
		}
	}
	return sql.ErrUnknownConstraint.New(name)
}

// --- auto-increment ---

func (t *Table) PeekNextAutoIncrementValue(*sql.Context) (uint64, error) {
	return t.def.db.p.core.PeekID(t.def.id) + 1, nil
}

func (t *Table) GetNextAutoIncrementValue(_ *sql.Context, given any) (uint64, error) {
	core := t.def.db.p.core
	if given != nil {
		if v, err := toCore(given); err == nil {
			switch x := v.(type) {
			case int64:
				if x > 0 {
					core.BumpID(t.def.id, uint64(x))
					return uint64(x), nil
				}
			case uint64:
				if x > 0 {
					core.BumpID(t.def.id, x)
					return x, nil
				}
			}
		}
	}
	return core.NextID(t.def.id), nil
}

func (t *Table) AutoIncrementSetter(*sql.Context) sql.AutoIncrementSetter {
	return autoIncSetter{t}
}

type autoIncSetter struct{ t *Table }

func (s autoIncSetter) SetAutoIncrementValue(_ *sql.Context, v uint64) error {
	s.t.def.db.p.core.SetID(s.t.def.id, v-1)
	return nil
}
func (s autoIncSetter) AcquireAutoIncrementLock(*sql.Context) (func(), error) { return func() {}, nil }
func (s autoIncSetter) Close(*sql.Context) error                              { return nil }
