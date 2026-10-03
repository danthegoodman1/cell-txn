// Package sqlgms serves cell-tnx through go-mysql-server. Tables map to
// core tables keyed by their encoded primary keys; secondary indexes and
// CHECKs map to core indexes and checks. Each statement's plan is
// annotated with the columns it reads, writes and updates as deltas, so
// conflicts are tracked per column.
package sqlgms

import (
	"fmt"
	"slices"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/dolthub/go-mysql-server/sql"

	"cell-tnx/txn"
)

// Provider is the catalog: databases and their tables over one core DB.
type Provider struct {
	core *txn.DB
	mu   sync.RWMutex
	dbs  map[string]*Database
	// OnDDL, when set, receives the text of each successful catalog
	// change, once per statement, so a durable server can replay them.
	OnDDL   func(db, query string) error
	ddlMu   sync.Mutex
	lastDDL *sql.Context
}

// ddl reports a catalog change made by ctx's statement.
func (p *Provider) ddl(ctx *sql.Context) error {
	p.ddlMu.Lock()
	defer p.ddlMu.Unlock()
	if p.OnDDL == nil || ctx == nil || ctx == p.lastDDL {
		return nil
	}
	p.lastDDL = ctx
	return p.OnDDL(ctx.GetCurrentDatabase(), ctx.Query())
}

var (
	_ sql.MutableDatabaseProvider = (*Provider)(nil)
	_ sql.TableCreator            = (*Database)(nil)
	_ sql.TableDropper            = (*Database)(nil)
	_ sql.CollatedDatabase        = (*Database)(nil)
	_ sql.ViewDatabase            = (*Database)(nil)
	_ sql.TriggerDatabase         = (*Database)(nil)
)

// NewProvider returns a catalog over core.
func NewProvider(core *txn.DB) *Provider {
	return &Provider{core: core, dbs: map[string]*Database{}}
}

// Core returns the transaction layer.
func (p *Provider) Core() *txn.DB { return p.core }

func (p *Provider) Database(_ *sql.Context, name string) (sql.Database, error) {
	p.mu.RLock()
	defer p.mu.RUnlock()
	if d, ok := p.dbs[strings.ToLower(name)]; ok {
		return d, nil
	}
	if strings.EqualFold(name, "mysql") {
		// The planner looks up "mysql" for privileges on every statement,
		// and a fresh error captures a stack trace.
		return nil, errNoMySQLDB
	}
	return nil, sql.ErrDatabaseNotFound.New(name)
}

var errNoMySQLDB = sql.ErrDatabaseNotFound.New("mysql")

func (p *Provider) HasDatabase(_ *sql.Context, name string) bool {
	p.mu.RLock()
	defer p.mu.RUnlock()
	_, ok := p.dbs[strings.ToLower(name)]
	return ok
}

func (p *Provider) AllDatabases(*sql.Context) []sql.Database {
	p.mu.RLock()
	defer p.mu.RUnlock()
	var out []sql.Database
	for _, d := range p.dbs {
		out = append(out, d)
	}
	slices.SortFunc(out, func(a, b sql.Database) int { return strings.Compare(a.Name(), b.Name()) })
	return out
}

func (p *Provider) CreateDatabase(ctx *sql.Context, name string) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	k := strings.ToLower(name)
	if _, ok := p.dbs[k]; ok {
		return sql.ErrDatabaseExists.New(name)
	}
	p.dbs[k] = &Database{p: p, name: name, tables: map[string]*tableDef{}}
	return p.ddl(ctx)
}

// DropDatabase forgets a database. Its core tables stay allocated, since
// core table IDs are never reused.
func (p *Provider) DropDatabase(ctx *sql.Context, name string) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	k := strings.ToLower(name)
	if _, ok := p.dbs[k]; !ok {
		return sql.ErrDatabaseNotFound.New(name)
	}
	delete(p.dbs, k)
	return p.ddl(ctx)
}

// Database is a namespace of tables.
type Database struct {
	p      *Provider
	name   string
	mu     sync.RWMutex
	tables map[string]*tableDef
	coll   sql.CollationID
	views  map[string]sql.ViewDefinition
}

func (d *Database) Name() string { return d.name }

// CreateView stores a view's definition; the engine expands it on use.
func (d *Database) CreateView(ctx *sql.Context, name, selectStmt, createStmt string) error {
	d.mu.Lock()
	k := strings.ToLower(name)
	if _, ok := d.views[k]; ok {
		d.mu.Unlock()
		return sql.ErrExistingView.New(d.name, name)
	}
	if d.views == nil {
		d.views = map[string]sql.ViewDefinition{}
	}
	d.views[k] = sql.ViewDefinition{Name: name, TextDefinition: selectStmt, CreateViewStatement: createStmt}
	d.mu.Unlock()
	return d.p.ddl(ctx)
}

func (d *Database) DropView(ctx *sql.Context, name string) error {
	d.mu.Lock()
	k := strings.ToLower(name)
	if _, ok := d.views[k]; !ok {
		d.mu.Unlock()
		return sql.ErrViewDoesNotExist.New(d.name, name)
	}
	delete(d.views, k)
	d.mu.Unlock()
	return d.p.ddl(ctx)
}

func (d *Database) GetViewDefinition(_ *sql.Context, name string) (sql.ViewDefinition, bool, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	v, ok := d.views[strings.ToLower(name)]
	return v, ok, nil
}

func (d *Database) AllViews(*sql.Context) ([]sql.ViewDefinition, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	var out []sql.ViewDefinition
	for _, v := range d.views {
		out = append(out, v)
	}
	slices.SortFunc(out, func(a, b sql.ViewDefinition) int { return strings.Compare(a.Name, b.Name) })
	return out, nil
}

// GetTriggers reports no triggers: they are outside this prototype.
func (d *Database) GetTriggers(*sql.Context) ([]sql.TriggerDefinition, error) { return nil, nil }

func (d *Database) CreateTrigger(*sql.Context, sql.TriggerDefinition) error {
	return fmt.Errorf("sqlgms: triggers are not supported")
}

func (d *Database) DropTrigger(_ *sql.Context, name string) error {
	return sql.ErrTriggerDoesNotExist.New(name)
}

// GetCollation returns the default collation for new tables.
func (d *Database) GetCollation(*sql.Context) sql.CollationID {
	d.mu.RLock()
	defer d.mu.RUnlock()
	if d.coll == 0 {
		return sql.Collation_Default
	}
	return d.coll
}

// SetCollation sets the default collation for new tables.
func (d *Database) SetCollation(ctx *sql.Context, c sql.CollationID) error {
	d.mu.Lock()
	d.coll = c
	d.mu.Unlock()
	return d.p.ddl(ctx)
}

func (d *Database) GetTableInsensitive(_ *sql.Context, name string) (sql.Table, bool, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	if t, ok := d.tables[strings.ToLower(name)]; ok {
		return &Table{def: t}, true, nil
	}
	return nil, false, nil
}

func (d *Database) GetTableNames(*sql.Context) ([]string, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	var names []string
	for _, t := range d.tables {
		names = append(names, t.name)
	}
	slices.Sort(names)
	return names, nil
}

// CreateTable creates a core table outside any transaction. Rows are keyed
// by their encoded primary key, or by a hidden counter when there is none.
func (d *Database) CreateTable(ctx *sql.Context, name string, sch sql.PrimaryKeySchema, coll sql.CollationID, _ string) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	k := strings.ToLower(name)
	if _, ok := d.tables[k]; ok {
		return sql.ErrTableAlreadyExists.New(name)
	}
	if len(sch.Schema) > txn.MaxCols {
		return fmt.Errorf("sqlgms: table %s has %d columns, at most %d supported", name, len(sch.Schema), txn.MaxCols)
	}
	def := &tableDef{db: d, name: name, coll: coll, pk: sch.PkOrdinals, autoInc: -1}
	for i, c := range sch.Schema {
		cc := *c
		cc.Source = name
		def.sch = append(def.sch, &cc)
		if c.AutoIncrement {
			def.autoInc = i
		}
	}
	for _, c := range def.pk {
		if err := appendable(def.sch[c].Type); err != nil {
			return err
		}
	}
	cols := make([]string, len(def.sch))
	for i, c := range def.sch {
		cols[i] = c.Name
	}
	prefix := 8
	if len(def.pk) > 0 {
		prefix = bucketPrefix(def.sch, def.pk)
	}
	def.id = d.p.core.CreateTable(txn.Schema{Name: d.name + "." + name, Cols: cols, BucketPrefix: prefix})
	def.compiled.Store(&checkSet{})
	d.tables[k] = def
	return d.p.ddl(ctx)
}

// appendable reports why a type cannot be part of a key, or nil.
func appendable(typ sql.Type) error {
	if _, err := appendKey(nil, typ, ""); err != nil {
		return err
	}
	return nil
}

func (d *Database) DropTable(ctx *sql.Context, name string) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	k := strings.ToLower(name)
	if _, ok := d.tables[k]; !ok {
		return sql.ErrTableNotFound.New(name)
	}
	delete(d.tables, k)
	return d.p.ddl(ctx)
}

// tableDef is a table's shared definition; Table values are per-plan
// views of it.
type tableDef struct {
	db      *Database
	name    string
	id      int // core table ID
	sch     sql.Schema
	pk      []int // primary key ordinals; empty for keyless tables
	coll    sql.CollationID
	autoInc int // auto-increment column, or -1

	mu      sync.Mutex // guards indexes and checks
	indexes []*Index   // secondary, in core order
	checks  []sql.CheckDefinition
	version int // bumped when checks change

	compiled atomic.Pointer[checkSet]
}

// all is every column of the table.
func (t *tableDef) all() txn.ColMask { return txn.Col(len(t.sch)) - 1 }

func (t *tableDef) colIndex(name string) int {
	for i, c := range t.sch {
		if strings.EqualFold(c.Name, name) {
			return i
		}
	}
	return -1
}

func (t *tableDef) mask(cols []int) txn.ColMask {
	var m txn.ColMask
	for _, c := range cols {
		m |= txn.Col(c)
	}
	return m
}

// key returns a stored row's primary key; keyless tables have none.
func (t *tableDef) key(row []txn.Value) (string, error) {
	k, _, err := encodeKey(t.sch, t.pk, row)
	return k, err
}

func (t *tableDef) generated() bool {
	for _, c := range t.sch {
		if c.Generated != nil {
			return true
		}
	}
	return false
}
