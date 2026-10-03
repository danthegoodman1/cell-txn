package sim

import (
	"context"
	"fmt"
	"math/rand/v2"
	"runtime/debug"
	"slices"
	"strings"

	"github.com/dolthub/go-mysql-server/memory"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/plan"
	"github.com/dolthub/go-mysql-server/sql/types"

	"cell-tnx/check"
	"cell-tnx/sqlgms"
	"cell-tnx/txn"
	"cell-tnx/workload"
)

// The SQL simulation runs random transactions through go-mysql-server and
// the store, then checks them end to end: every committed transaction's
// statements are replayed, one transaction at a time in serialization
// order, on go-mysql-server's in-memory reference engine, and every result
// and the final tables must match.

var sqlDDL = []string{
	"CREATE TABLE t (id INT PRIMARY KEY, a INT NOT NULL, b INT, c VARCHAR(40), u INT, CHECK (a >= 0))",
	"CREATE INDEX tb ON t (b)",
	"CREATE UNIQUE INDEX tu ON t (u)",
	"CREATE TABLE t2 (id INT PRIMARY KEY, v INT)",
}

// sqlStmt is a statement and its canonical result.
type sqlStmt struct {
	q, res string
}

// sqlTxn is a committed or CHECK-rejected transaction.
type sqlTxn struct {
	kind   check.Kind
	ts     uint64
	client int
	stmts  []sqlStmt
	reason string // why a rejected transaction was rejected
}

// sqlParams are a SQL seed's drawn parameters.
type sqlParams struct {
	Keys     int
	Weights  [nsql]int
	Explicit float64 // share of explicit transactions
	MaxStmts int
	Rollback float64
	Think    float64
}

const (
	sqPoint = iota
	sqRange
	sqIndex
	sqIndexAgg
	sqUnique
	sqJoin
	sqAgg
	sqDelta
	sqMultiDelta
	sqBlind
	sqSetB
	sqSetU
	sqRMW
	sqInsert
	sqUpsert
	sqReplace
	sqDelete
	sqRangeDelete
	sqT2Upsert
	sqSubquery
	nsql
)

func drawSQL(r *rand.Rand) sqlParams {
	p := sqlParams{
		Keys:     4 + r.IntN(20),
		Explicit: r.Float64(),
		MaxStmts: 1 + r.IntN(6),
		Rollback: r.Float64() * 0.1,
		Think:    r.Float64() * 0.2,
	}
	for i := range p.Weights {
		if r.IntN(4) != 0 {
			p.Weights[i] = 1 + r.IntN(8)
		}
	}
	p.Weights[sqPoint]++
	return p
}

// sqlEnv runs statements for one session against an engine.
type sqlEnv struct {
	e interface {
		Query(*sql.Context, string) (sql.Schema, sql.RowIter, *sql.QueryFlags, error)
	}
	s sql.Session
}

func (x sqlEnv) query(q string) ([]sql.Row, error) {
	ctx := sql.NewContext(context.Background(), sql.WithSession(x.s))
	ctx.SetCurrentDatabase("db")
	if l, ok := x.s.(sql.LifecycleAwareSession); ok {
		defer l.CommandEnd()
	}
	_, it, _, err := x.e.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	return sql.RowIterToRows(ctx, it)
}

// canon renders a statement's outcome so both engines' outcomes compare
// as strings. UPDATE reports matched rows, since rows changed depends on
// old values a statement need not read.
func canon(q string, rows []sql.Row, err error) string {
	if err != nil {
		return "error:" + errClass(err)
	}
	if len(rows) == 1 && types.IsOkResult(rows[0]) {
		ok := types.GetOkResult(rows[0])
		if info, isUpdate := ok.Info.(plan.UpdateInfo); isUpdate {
			return fmt.Sprintf("ok matched=%d", info.Matched)
		}
		return fmt.Sprintf("ok affected=%d", ok.RowsAffected)
	}
	lines := make([]string, len(rows))
	for i, r := range rows {
		lines[i] = fmt.Sprint(r...)
	}
	if !strings.Contains(q, "ORDER BY") {
		slices.Sort(lines)
	}
	return strings.Join(lines, "|")
}

func errClass(err error) string {
	e := sql.UnwrapError(err)
	switch {
	case sql.ErrUniqueKeyViolation.Is(e), sql.ErrPrimaryKeyViolation.Is(e), sql.ErrDuplicateEntry.Is(e):
		return "duplicate"
	case sql.ErrCheckConstraintViolated.Is(e):
		return "check"
	case sql.ErrLockDeadlock.Is(e):
		return "deadlock"
	}
	return "other: " + err.Error()
}

type sqlClient struct {
	id    int
	env   sqlEnv
	sess  *sqlgms.Session
	r     *rand.Rand
	sched *sched
	p     sqlParams
	seq   int
	log   func(sqlTxn)
	trace func(string)
	quota int
	done  int
	stats *workload.Stats
}

func (c *sqlClient) exec(q string) ([]sql.Row, error) {
	c.sched.Yield()
	rows, err := c.env.query(q)
	c.trace(q + " => " + canon(q, rows, err))
	return rows, err
}

func (c *sqlClient) key() int { return c.r.IntN(c.p.Keys) }

func (c *sqlClient) uniq() string {
	c.seq++
	return fmt.Sprintf("c%d_%d", c.id, c.seq)
}

func (c *sqlClient) nullable(n int) string {
	if c.r.IntN(5) == 0 {
		return "NULL"
	}
	return fmt.Sprint(c.r.IntN(n))
}

// gen draws a statement. Deltas in one transaction share a sign, so a
// transaction whose final rows pass the CHECK passes it after every
// statement, as the reference engine requires.
func (c *sqlClient) gen(sign string) string {
	total := 0
	for _, w := range c.p.Weights {
		total += w
	}
	n := c.r.IntN(total)
	kind := 0
	for kind = range c.p.Weights {
		if n < c.p.Weights[kind] {
			break
		}
		n -= c.p.Weights[kind]
	}
	k, k2 := c.key(), c.key()
	lo, hi := min(k, k2), max(k, k2)
	row := func() string {
		return fmt.Sprintf("(%d, %d, %s, '%s', %s)", c.key(), c.r.IntN(10), c.nullable(5), c.uniq(), c.nullable(2*c.p.Keys))
	}
	switch kind {
	case sqPoint:
		return fmt.Sprintf("SELECT a, b, c, u FROM t WHERE id = %d", k)
	case sqRange:
		return fmt.Sprintf("SELECT id, a, c FROM t WHERE id BETWEEN %d AND %d ORDER BY id", lo, hi)
	case sqIndex:
		return fmt.Sprintf("SELECT id, a, c FROM t WHERE b = %d ORDER BY id", c.r.IntN(5))
	case sqIndexAgg:
		return fmt.Sprintf("SELECT COUNT(*), SUM(a) FROM t WHERE b BETWEEN %d AND %d", lo%5, hi%5)
	case sqUnique:
		return fmt.Sprintf("SELECT id, c FROM t WHERE u = %d", c.r.IntN(2*c.p.Keys))
	case sqJoin:
		return fmt.Sprintf("SELECT t.id, t.a, t2.v FROM t JOIN t2 ON t.id = t2.id WHERE t.id <= %d ORDER BY t.id", k)
	case sqAgg:
		return "SELECT COUNT(*), SUM(a), MAX(c) FROM t"
	case sqDelta:
		return fmt.Sprintf("UPDATE t SET a = a %s %d WHERE id = %d", sign, 1+c.r.IntN(3), k)
	case sqMultiDelta:
		return fmt.Sprintf("UPDATE t SET a = a %s 1 WHERE b = %d", sign, c.r.IntN(5))
	case sqBlind:
		return fmt.Sprintf("UPDATE t SET c = '%s' WHERE id = %d", c.uniq(), k)
	case sqSetB:
		return fmt.Sprintf("UPDATE t SET b = %s WHERE id = %d", c.nullable(5), k)
	case sqSetU:
		return fmt.Sprintf("UPDATE t SET u = %s WHERE id = %d", c.nullable(2*c.p.Keys), k)
	case sqRMW:
		return fmt.Sprintf("UPDATE t SET c = CONCAT(c, 'x'), b = a WHERE id = %d", k)
	case sqInsert:
		return "INSERT INTO t VALUES " + row()
	case sqUpsert:
		if sign == "-" {
			return fmt.Sprintf("SELECT a FROM t WHERE id = %d", k)
		}
		return "INSERT INTO t VALUES " + row() + " ON DUPLICATE KEY UPDATE a = a + 1"
	case sqReplace:
		return "REPLACE INTO t VALUES " + row()
	case sqDelete:
		return fmt.Sprintf("DELETE FROM t WHERE id = %d", k)
	case sqRangeDelete:
		return fmt.Sprintf("DELETE FROM t WHERE id BETWEEN %d AND %d", lo, min(hi, lo+2))
	case sqT2Upsert:
		return fmt.Sprintf("INSERT INTO t2 VALUES (%d, %d) ON DUPLICATE KEY UPDATE v = v + 1", k, c.r.IntN(100))
	default:
		return fmt.Sprintf("UPDATE t2 SET v = (SELECT a FROM t WHERE t.id = t2.id) WHERE id = %d", k)
	}
}

func (c *sqlClient) take() *sqlgms.CommitInfo {
	info := c.sess.LastCommit
	c.sess.LastCommit = nil
	return info
}

// txn runs one transaction, retrying conflicts, and logs it if it
// committed or was rejected by a CHECK at commit.
func (c *sqlClient) txn() error {
	backoff := int64(4)
	for {
		ok, err := c.attempt()
		if err != nil {
			return err
		}
		if ok {
			return nil
		}
		c.stats.Conflicts++
		env{c.sched}.Sleep(1 + c.r.Int64N(backoff))
		backoff = min(2*backoff, 1024)
	}
}

func (c *sqlClient) attempt() (bool, error) {
	sign := "+"
	if c.r.IntN(2) == 0 {
		sign = "-"
	}
	log := sqlTxn{client: c.id}
	finish := func(err error) (bool, error) {
		info := c.take()
		switch {
		case err != nil && errClass(err) == "deadlock":
			return false, nil
		case info == nil:
			return true, nil // nothing committed
		case info.Rejected:
			log.kind = check.Rejected
			log.reason = info.Check
		case info.Wrote:
			log.kind = check.Writer
		default:
			log.kind = check.Reader
		}
		log.ts = info.Ts
		c.log(log)
		return true, nil
	}
	if c.r.Float64() >= c.p.Explicit {
		q := c.gen(sign)
		rows, err := c.exec(q)
		log.stmts = append(log.stmts, sqlStmt{q, canon(q, rows, err)})
		return finish(err)
	}
	if _, err := c.exec("START TRANSACTION"); err != nil {
		return false, err
	}
	c.take()
	for range 1 + c.r.IntN(c.p.MaxStmts) {
		if c.r.Float64() < c.p.Think {
			env{c.sched}.Sleep(1 + c.r.Int64N(30))
		}
		q := c.gen(sign)
		rows, err := c.exec(q)
		log.stmts = append(log.stmts, sqlStmt{q, canon(q, rows, err)})
	}
	if c.r.Float64() < c.p.Rollback {
		_, err := c.exec("ROLLBACK")
		c.take()
		return err == nil, err
	}
	_, err := c.exec("COMMIT")
	if err != nil && errClass(err) != "deadlock" && errClass(err) != "check" {
		return false, err
	}
	return finish(err)
}

// RunSQL simulates one SQL seed.
func RunSQL(seed uint64, cfg Config) (res Result) {
	res.Seed = seed
	r := rand.New(rand.NewPCG(seed, 0x51))
	p := Params{
		Mode:       txn.Mode(r.IntN(3)),
		Workload:   "sql",
		Clients:    1 + r.IntN(8),
		Quota:      3 + r.IntN(20),
		BucketBits: uint(r.IntN(5)),
		Skew:       r.IntN(3) == 0,
	}
	if cfg.Mode >= 0 {
		p.Mode = txn.Mode(cfg.Mode)
	}
	sp := drawSQL(r)
	p.Detail = fmt.Sprintf("%+v", sp)
	res.Params = p
	maxSteps := cfg.MaxSteps
	if maxSteps == 0 {
		maxSteps = 2_000_000
	}
	child := func() *rand.Rand { return rand.New(rand.NewPCG(r.Uint64(), r.Uint64())) }

	s := &sched{}
	db := txn.Open(txn.Options{Mode: p.Mode, BucketBits: p.BucketBits, Yield: s.Yield, Wait: s.Wait, NoInvariants: cfg.NoInvariants})
	pro := sqlgms.NewProvider(db)
	eng := sqlgms.NewEngine(pro)
	var logs []sqlTxn
	trace := uint64(fnvBasis)
	hash := func(str string) {
		for i := 0; i < len(str); i++ {
			trace = (trace ^ uint64(str[i])) * fnvPrime
		}
	}

	var cos []*co
	defer func() { stopAll(cos) }()
	defer func() {
		if x := recover(); x != nil {
			res.Err = fmt.Errorf("panic: %v\n%s", x, debug.Stack())
		}
	}()

	setup := sqlEnv{eng, sqlgms.NewSession(sql.NewBaseSession(), pro)}
	if _, err := setup.query("CREATE DATABASE db"); err != nil {
		res.Err = err
		return res
	}
	for _, q := range sqlDDL {
		if _, err := setup.query(q); err != nil {
			res.Err = fmt.Errorf("%s: %w", q, err)
			return res
		}
	}
	loader := &sqlClient{id: 0, env: setup, sess: setup.s.(*sqlgms.Session), r: child(), sched: s, p: sp,
		log: func(t sqlTxn) { logs = append(logs, t) }, trace: hash, stats: &workload.Stats{}}
	for k := range sp.Keys {
		if loader.r.IntN(2) == 0 {
			continue
		}
		q := fmt.Sprintf("INSERT INTO t VALUES (%d, %d, %d, '%s', %d)", k, 5+loader.r.IntN(10), loader.r.IntN(5), loader.uniq(), k)
		rows, err := loader.exec(q)
		if err != nil {
			res.Err = fmt.Errorf("%s: %w", q, err)
			return res
		}
		info := loader.take()
		logs = append(logs, sqlTxn{kind: check.Writer, ts: info.Ts, stmts: []sqlStmt{{q, canon(q, rows, nil)}}})
	}

	clients := make([]*sqlClient, p.Clients)
	for i := range clients {
		sess := sqlgms.NewSession(sql.NewBaseSession(), pro)
		c := &sqlClient{id: i + 1, env: sqlEnv{eng, sess}, sess: sess, r: child(), sched: s, p: sp,
			log: func(t sqlTxn) { logs = append(logs, t) }, trace: hash, quota: p.Quota, stats: &res.Stats}
		clients[i] = c
		co := s.spawn(func() error {
			for c.done < c.quota {
				if err := c.txn(); err != nil {
					return fmt.Errorf("client %d: %w", c.id, err)
				}
				c.done++
			}
			return nil
		})
		if p.Skew {
			co.weight = 1 << r.IntN(6)
		}
		cos = append(cos, co)
	}

	runnable := []*co{}
	for {
		runnable = runnable[:0]
		wake, live, total := int64(-1), false, 0
		for _, c := range cos {
			if c.done {
				continue
			}
			live = true
			switch {
			case c.wake > s.now:
				if wake < 0 || c.wake < wake {
					wake = c.wake
				}
			case c.blocked == nil || c.blocked():
				runnable = append(runnable, c)
				total += c.weight
			}
		}
		if !live {
			break
		}
		if len(runnable) == 0 {
			s.now = wake
			continue
		}
		var c *co
		n := r.IntN(total)
		for _, c = range runnable {
			if n < c.weight {
				break
			}
			n -= c.weight
		}
		s.cur = c
		_, ok := c.next()
		s.cur = nil
		if !ok {
			c.done = true
			if c.err != nil {
				res.Err = c.err
				return res
			}
		}
		s.now++
		res.Steps++
		if res.Steps > maxSteps {
			res.Err = fmt.Errorf("liveness: clients unfinished after %d steps", maxSteps)
			return res
		}
		if !cfg.NoInvariants && res.Steps%500 == 0 {
			if res.Err = db.CheckInvariants(); res.Err != nil {
				return res
			}
		}
	}
	if !cfg.NoInvariants {
		if res.Err = db.CheckInvariants(); res.Err != nil {
			return res
		}
	}
	res.Logs = len(logs)
	res.Trace = trace ^ uint64(res.Steps)
	for _, l := range logs {
		switch l.kind {
		case check.Writer:
			res.Stats.Commits++
		case check.Rejected:
			res.Stats.Rejected++
		}
	}
	res.Err = replaySQL(setup, logs, db.LastCommitTs())
	return res
}

const (
	fnvBasis = 14695981039346656037
	fnvPrime = 1099511628211
)

// replaySQL reruns the logged transactions serially on the reference
// engine and compares every result, then the final tables.
func replaySQL(ours sqlEnv, logs []sqlTxn, last uint64) error {
	slices.SortStableFunc(logs, func(a, b sqlTxn) int {
		if a.ts != b.ts {
			return int(int64(a.ts - b.ts))
		}
		return btoi(b.kind == check.Writer) - btoi(a.kind == check.Writer)
	})
	var writers uint64
	for _, l := range logs {
		if l.kind == check.Writer {
			if writers++; l.ts != writers {
				return fmt.Errorf("sql check: writer commit timestamps skip from %d to %d", writers-1, l.ts)
			}
		}
	}
	if writers != last {
		return fmt.Errorf("sql check: %d writers logged, last commit is %d", writers, last)
	}
	ref, err := newReference()
	if err != nil {
		return err
	}
	var committed []sqlTxn // writers so far, in commit order
	for i, l := range logs {
		if l.kind == check.Rejected {
			// The reference engine's ROLLBACK leaves its secondary indexes
			// stale, so a rejected transaction runs on a fresh reference
			// rebuilt from the writers before it, and is then discarded.
			if err := checkRejected(committed, l); err != nil {
				return fmt.Errorf("sql check: txn %d (ts=%d client=%d) rejected at commit by %q: %w", i, l.ts, l.client, l.reason, err)
			}
			continue
		}
		if err := ref.run(l, func(j int, st sqlStmt, got string) error {
			return fmt.Errorf("sql check: txn %d (%s ts=%d client=%d) stmt %d %q: ours %q, serial %q", i, l.kind, l.ts, l.client, j, st.q, st.res, got)
		}); err != nil {
			return err
		}
		if l.kind == check.Writer {
			committed = append(committed, l)
		}
	}
	for _, q := range []string{"SELECT * FROM t ORDER BY id", "SELECT * FROM t2 ORDER BY id"} {
		a, err := ours.query(q)
		if err != nil {
			return err
		}
		b, err := ref.query(q)
		if err != nil {
			return err
		}
		if x, y := canon(q, a, nil), canon(q, b, nil); x != y {
			return fmt.Errorf("sql check: final %q: ours %q, serial %q", q, x, y)
		}
	}
	return nil
}

func newReference() (sqlEnv, error) {
	pro := memory.NewDBProvider()
	ref := sqlEnv{sqlgms.NewDefaultEngine(pro), memory.NewSession(sql.NewBaseSession(), pro)}
	ctx := sql.NewContext(context.Background(), sql.WithSession(ref.s))
	if err := pro.CreateDatabase(ctx, "db"); err != nil {
		return ref, err
	}
	for _, q := range sqlDDL {
		if _, err := ref.query(q); err != nil {
			return ref, fmt.Errorf("reference %s: %w", q, err)
		}
	}
	return ref, nil
}

// run replays one committed transaction, reporting the first statement
// whose result differs.
func (x sqlEnv) run(l sqlTxn, mismatch func(int, sqlStmt, string) error) error {
	if _, err := x.query("START TRANSACTION"); err != nil {
		return err
	}
	for j, st := range l.stmts {
		rows, err := x.query(st.q)
		if got := canon(st.q, rows, err); got != st.res {
			return mismatch(j, st, got)
		}
	}
	_, err := x.query("COMMIT")
	return err
}

// checkRejected verifies that a transaction the store rejected at commit
// violates a CHECK when run serially after the given writers, with every
// earlier statement agreeing.
func checkRejected(committed []sqlTxn, l sqlTxn) error {
	ref, err := newReference()
	if err != nil {
		return err
	}
	for _, c := range committed {
		if err := ref.run(c, func(j int, st sqlStmt, got string) error {
			return fmt.Errorf("rebuilding reference: %q gave %q, logged %q", st.q, got, st.res)
		}); err != nil {
			return err
		}
	}
	if _, err := ref.query("START TRANSACTION"); err != nil {
		return err
	}
	for _, st := range l.stmts {
		rows, err := ref.query(st.q)
		got := canon(st.q, rows, err)
		if got == "error:check" {
			return nil
		}
		if got != st.res {
			return fmt.Errorf("%q: ours %q, serial %q", st.q, st.res, got)
		}
	}
	return fmt.Errorf("every statement passes serially: %v", l.stmts)
}

func btoi(b bool) int {
	if b {
		return 1
	}
	return 0
}
