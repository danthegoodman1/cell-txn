// Command sqlbench drives identical SQL against any MySQL-protocol server
// (cell-tnx or MySQL), retrying deadlocks and lock timeouts, and reports
// throughput, retries and latency.
//
//	sqlbench -dsn 'root@tcp(127.0.0.1:3307)/' -workload tpcc -warehouses 1 -setup
//	sqlbench -dsn 'root@tcp(127.0.0.1:3308)/' -isolation SERIALIZABLE ...
package main

import (
	"context"
	"database/sql"
	"errors"
	"flag"
	"fmt"
	"math/rand/v2"
	"os"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/go-sql-driver/mysql"

	"cell-tnx/workload"
)

type config struct {
	workload   string
	clients    int
	duration   time.Duration
	warehouses int
	districts  int
	customers  int
	items      int
	lines      int
	rows       int
	theta      float64
	think      time.Duration
	isolation  string
	disjoint   float64
}

func main() {
	var c config
	dsn := flag.String("dsn", "root@tcp(127.0.0.1:3307)/", "server DSN, without a database")
	label := flag.String("label", "", "system label for the report")
	setup := flag.Bool("setup", false, "create and load the schema first")
	csvPath := flag.String("csv", "", "append results to this CSV file")
	flag.StringVar(&c.workload, "workload", "tpcc", "tpcc or users")
	flag.IntVar(&c.clients, "clients", 32, "concurrent clients")
	flag.DurationVar(&c.duration, "duration", 10*time.Second, "measured run time")
	flag.IntVar(&c.warehouses, "warehouses", 1, "tpcc: warehouses")
	flag.IntVar(&c.districts, "districts", 10, "tpcc: districts per warehouse")
	flag.IntVar(&c.customers, "customers", 300, "tpcc: customers per district")
	flag.IntVar(&c.items, "items", 10000, "tpcc: items")
	flag.IntVar(&c.lines, "lines", 10, "tpcc: max order lines")
	flag.IntVar(&c.rows, "rows", 1000, "users: rows")
	flag.Float64Var(&c.theta, "theta", 0.9, "users: Zipf skew")
	flag.DurationVar(&c.think, "think", 0, "pause between statements inside a transaction")
	flag.StringVar(&c.isolation, "isolation", "", "transaction isolation for every connection (MySQL)")
	flag.Float64Var(&c.disjoint, "disjoint", 0.5, "users: share of email updates; the rest touch wallet, the same column")
	flag.Parse()

	db, err := open(*dsn, "", c)
	if err != nil {
		fail(err)
	}
	if *setup {
		if err := load(db, c); err != nil {
			fail(err)
		}
	}
	db.Close()
	if db, err = open(*dsn, "bench", c); err != nil {
		fail(err)
	}
	r := run(db, c)
	fmt.Printf("%-14s %-6s clients=%-3d %8.0f commits/s  retries/txn %.2f  p50 %v  p99 %v  errors %d\n",
		*label, c.workload, c.clients, r.rate, r.retries, r.p50, r.p99, r.errors)
	if *csvPath != "" {
		f, err := os.OpenFile(*csvPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			fail(err)
		}
		if st, _ := f.Stat(); st.Size() == 0 {
			fmt.Fprintln(f, "system,workload,warehouses,theta,disjoint,clients,think_ms,commits_per_s,retries_per_txn,p50_ms,p99_ms,errors")
		}
		fmt.Fprintf(f, "%s,%s,%d,%.2f,%.2f,%d,%.1f,%.0f,%.3f,%.2f,%.2f,%d\n", *label, c.workload, c.warehouses, c.theta, c.disjoint, c.clients,
			float64(c.think)/float64(time.Millisecond), r.rate, r.retries, ms(r.p50), ms(r.p99), r.errors)
		f.Close()
	}
}

func ms(d time.Duration) float64 { return float64(d) / float64(time.Millisecond) }

func fail(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}

func open(dsn, dbName string, c config) (*sql.DB, error) {
	cfg, err := mysql.ParseDSN(dsn)
	if err != nil {
		return nil, err
	}
	cfg.DBName = dbName
	cfg.InterpolateParams = true
	if c.isolation != "" {
		cfg.Params = map[string]string{"transaction_isolation": "'" + c.isolation + "'"}
	}
	db, err := sql.Open("mysql", cfg.FormatDSN())
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(c.clients + 4)
	db.SetMaxIdleConns(c.clients + 4)
	return db, db.Ping()
}

// --- schema and load ---

func load(db *sql.DB, c config) error {
	ctx := context.Background()
	stmts := []string{"DROP DATABASE IF EXISTS bench", "CREATE DATABASE bench", "USE bench"}
	switch c.workload {
	case "users":
		stmts = append(stmts,
			"CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(40), email VARCHAR(60), wallet INT NOT NULL, CHECK (wallet >= 0))",
			"CREATE TABLE audit (id BIGINT PRIMARY KEY, uid INT, email VARCHAR(60))")
	default:
		stmts = append(stmts,
			"CREATE TABLE warehouse (w_id INT PRIMARY KEY, w_tax INT, w_ytd BIGINT, w_name VARCHAR(20))",
			"CREATE TABLE district (d_w_id INT, d_id INT, d_tax INT, d_ytd BIGINT, d_next_o_id INT, d_name VARCHAR(20), PRIMARY KEY (d_w_id, d_id))",
			"CREATE TABLE customer (c_w_id INT, c_d_id INT, c_id INT, c_discount INT, c_credit INT, c_last INT, c_balance BIGINT, c_ytd_payment BIGINT, c_payment_cnt INT, PRIMARY KEY (c_w_id, c_d_id, c_id))",
			"CREATE INDEX c_last ON customer (c_w_id, c_d_id, c_last)",
			"CREATE TABLE history (h_id BIGINT PRIMARY KEY, h_c_id INT, h_d_id INT, h_w_id INT, h_amount INT)",
			"CREATE TABLE orders (o_w_id INT, o_d_id INT, o_id INT, o_c_id INT, o_ol_cnt INT, o_all_local INT, PRIMARY KEY (o_w_id, o_d_id, o_id))",
			"CREATE TABLE new_order (no_w_id INT, no_d_id INT, no_o_id INT, PRIMARY KEY (no_w_id, no_d_id, no_o_id))",
			"CREATE TABLE order_line (ol_w_id INT, ol_d_id INT, ol_o_id INT, ol_number INT, ol_i_id INT, ol_supply_w_id INT, ol_quantity INT, ol_amount INT, PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number))",
			"CREATE TABLE item (i_id INT PRIMARY KEY, i_price INT)",
			"CREATE TABLE stock (s_w_id INT, s_i_id INT, s_quantity INT, s_ytd BIGINT, s_order_cnt INT, s_remote_cnt INT, PRIMARY KEY (s_w_id, s_i_id))")
	}
	conn, err := db.Conn(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()
	for _, q := range stmts {
		if _, err := conn.ExecContext(ctx, q); err != nil {
			return fmt.Errorf("%s: %w", q, err)
		}
	}
	r := rand.New(rand.NewPCG(1, 2))
	ins := func(table string, rows [][]any) error {
		for len(rows) > 0 {
			n := min(len(rows), 500)
			var b strings.Builder
			fmt.Fprintf(&b, "INSERT INTO %s VALUES ", table)
			for i, row := range rows[:n] {
				if i > 0 {
					b.WriteString(",")
				}
				b.WriteString("(")
				for j, v := range row {
					if j > 0 {
						b.WriteString(",")
					}
					if s, ok := v.(string); ok {
						fmt.Fprintf(&b, "'%s'", s)
					} else {
						fmt.Fprint(&b, v)
					}
				}
				b.WriteString(")")
			}
			if _, err := conn.ExecContext(ctx, b.String()); err != nil {
				return fmt.Errorf("load %s: %w", table, err)
			}
			rows = rows[n:]
		}
		return nil
	}
	var rows [][]any
	switch c.workload {
	case "users":
		for i := range c.rows {
			rows = append(rows, []any{i, fmt.Sprintf("n%d", i), fmt.Sprintf("e%d", i), 1000})
		}
		return ins("users", rows)
	}
	for i := 1; i <= c.items; i++ {
		rows = append(rows, []any{i, 1 + r.IntN(100)})
	}
	if err := ins("item", rows); err != nil {
		return err
	}
	for w := 1; w <= c.warehouses; w++ {
		if err := ins("warehouse", [][]any{{w, r.IntN(2000), 30000 * c.districts, fmt.Sprintf("w%d", w)}}); err != nil {
			return err
		}
		rows = rows[:0]
		for i := 1; i <= c.items; i++ {
			rows = append(rows, []any{w, i, 10 + r.IntN(91), 0, 0, 0})
		}
		if err := ins("stock", rows); err != nil {
			return err
		}
		for d := 1; d <= c.districts; d++ {
			if err := ins("district", [][]any{{w, d, r.IntN(2000), 30000, 1, fmt.Sprintf("d%d", d)}}); err != nil {
				return err
			}
			rows = rows[:0]
			for cu := 1; cu <= c.customers; cu++ {
				rows = append(rows, []any{w, d, cu, r.IntN(5000), r.IntN(10) / 9, cu % max(1, c.customers/3), -10, 10, 1})
			}
			if err := ins("customer", rows); err != nil {
				return err
			}
		}
	}
	return nil
}

// --- running ---

type result struct {
	rate, retries float64
	p50, p99      time.Duration
	errors        int64
}

// checkViolation reports a CHECK constraint failure, a business outcome:
// MySQL returns 3819; go-mysql-server returns 1105 with the CHECK's name.
func checkViolation(err error) bool {
	var me *mysql.MySQLError
	return errors.As(err, &me) && (me.Number == 3819 || strings.Contains(me.Message, "Check constraint"))
}

// retryable reports MySQL deadlock (1213) and lock wait timeout (1205).
func retryable(err error) bool {
	var me *mysql.MySQLError
	return errors.As(err, &me) && (me.Number == 1213 || me.Number == 1205)
}

func run(db *sql.DB, c config) result {
	deadline := time.Now().Add(c.duration)
	var commits, retries, errs atomic.Int64
	var mu sync.Mutex
	var lats []time.Duration
	var wg sync.WaitGroup
	for id := range c.clients {
		wg.Go(func() {
			ctx := context.Background()
			conn, err := db.Conn(ctx)
			if err != nil {
				fail(err)
			}
			defer conn.Close()
			cl := &client{id: id, conn: conn, c: c, r: rand.New(rand.NewPCG(uint64(time.Now().UnixNano()), uint64(id))),
				seq: time.Now().UnixMicro()*1000 + int64(id)<<32}
			if c.workload == "users" {
				cl.zipf = workload.NewZipf(uint64(c.rows), c.theta)
			}
			var local []time.Duration
			for time.Now().Before(deadline) {
				start := time.Now()
				tx := cl.next()
				ok := false
				for {
					err := tx(ctx)
					if err == nil || errors.Is(err, errRollback) || checkViolation(err) {
						ok = true
						break
					}
					conn.ExecContext(ctx, "ROLLBACK")
					if !retryable(err) {
						if errs.Add(1) < 5 {
							fmt.Fprintln(os.Stderr, "error:", err)
						}
						break
					}
					retries.Add(1)
					time.Sleep(time.Duration(cl.r.IntN(1000)) * time.Microsecond)
				}
				if ok {
					commits.Add(1)
					local = append(local, time.Since(start))
				}
			}
			mu.Lock()
			lats = append(lats, local...)
			mu.Unlock()
		})
	}
	wg.Wait()
	slices.Sort(lats)
	q := func(p float64) time.Duration {
		if len(lats) == 0 {
			return 0
		}
		return lats[int(p*float64(len(lats)-1))]
	}
	n := commits.Load()
	return result{
		rate:    float64(n) / c.duration.Seconds(),
		retries: float64(retries.Load()) / max(1, float64(n)),
		p50:     q(0.5), p99: q(0.99),
		errors: errs.Load(),
	}
}

var errRollback = errors.New("rollback")

type client struct {
	id   int
	conn *sql.Conn
	c    config
	r    *rand.Rand
	zipf *workload.Zipf
	seq  int64
}

// uniq returns a key no other client or run produces.
func (cl *client) uniq() int64 {
	cl.seq++
	return cl.seq
}

// txn runs fn inside a transaction.
func (cl *client) txn(ctx context.Context, fn func(q func(string, ...any) *sql.Rows, x func(string, ...any) error) error) error {
	if _, err := cl.conn.ExecContext(ctx, "START TRANSACTION"); err != nil {
		return err
	}
	var qerr error
	query := func(s string, args ...any) *sql.Rows {
		cl.pause()
		rows, err := cl.conn.QueryContext(ctx, s, args...)
		if err != nil && qerr == nil {
			qerr = err
		}
		return rows
	}
	exec := func(s string, args ...any) error {
		cl.pause()
		_, err := cl.conn.ExecContext(ctx, s, args...)
		return err
	}
	if err := fn(query, exec); err != nil || qerr != nil {
		if err == nil {
			err = qerr
		}
		if errors.Is(err, errRollback) {
			cl.conn.ExecContext(ctx, "ROLLBACK")
		}
		return err
	}
	_, err := cl.conn.ExecContext(ctx, "COMMIT")
	return err
}

func (cl *client) pause() {
	if cl.c.think > 0 {
		time.Sleep(cl.c.think)
	}
}

// scan reads the single row of rows into dst.
func scan(rows *sql.Rows, dst ...any) (bool, error) {
	if rows == nil {
		return false, errors.New("no result")
	}
	defer rows.Close()
	if !rows.Next() {
		return false, rows.Err()
	}
	return true, rows.Scan(dst...)
}

func (cl *client) next() func(context.Context) error {
	if cl.c.workload == "users" {
		return cl.usersTxn()
	}
	if cl.r.IntN(88) < 45 {
		return cl.newOrder()
	}
	return cl.payment()
}

// usersTxn draws an email update with probability disjoint; otherwise a
// wallet change (pay, deposit, or a read-modify-write audit).
func (cl *client) usersTxn() func(context.Context) error {
	id := cl.zipf.Next(cl.r)
	x := 1 + cl.r.IntN(5)
	k := 5 + cl.r.IntN(3)
	if cl.r.Float64() >= cl.c.disjoint {
		k = []int{0, 1, 3, 4, 8, 9}[cl.r.IntN(6)]
	}
	switch {
	case k < 3:
		return func(ctx context.Context) error {
			_, err := cl.conn.ExecContext(ctx, "UPDATE users SET wallet = wallet - ? WHERE id = ?", x, id)
			return err
		}
	case k < 5:
		return func(ctx context.Context) error {
			_, err := cl.conn.ExecContext(ctx, "UPDATE users SET wallet = wallet + ? WHERE id = ?", x, id)
			return err
		}
	case k < 8:
		email := fmt.Sprintf("e%d", cl.uniq())
		return func(ctx context.Context) error {
			_, err := cl.conn.ExecContext(ctx, "UPDATE users SET email = ? WHERE id = ?", email, id)
			return err
		}
	default:
		aid := cl.uniq()
		return func(ctx context.Context) error {
			return cl.txn(ctx, func(q func(string, ...any) *sql.Rows, x func(string, ...any) error) error {
				var wallet int
				if _, err := scan(q("SELECT wallet FROM users WHERE id = ? FOR UPDATE", id), &wallet); err != nil {
					return err
				}
				if err := x("UPDATE users SET wallet = ? WHERE id = ?", wallet+1, id); err != nil {
					return err
				}
				return x("INSERT INTO audit VALUES (?, ?, ?)", aid, id, "w")
			})
		}
	}
}

func (cl *client) newOrder() func(context.Context) error {
	r, c := cl.r, cl.c
	w, d, cu := 1+r.IntN(c.warehouses), 1+r.IntN(c.districts), 1+r.IntN(c.customers)
	type line struct{ item, supply, qty int }
	lines := make([]line, 5+r.IntN(max(1, c.lines-4)))
	allLocal := 1
	for i := range lines {
		lines[i] = line{1 + r.IntN(c.items), w, 1 + r.IntN(10)}
		if c.warehouses > 1 && r.IntN(100) == 0 {
			lines[i].supply = 1 + r.IntN(c.warehouses)
			allLocal = 0
		}
	}
	if r.IntN(100) == 0 {
		lines[len(lines)-1].item = c.items + 1
	}
	return func(ctx context.Context) error {
		return cl.txn(ctx, func(q func(string, ...any) *sql.Rows, x func(string, ...any) error) error {
			var tax, dtax, next int
			if _, err := scan(q("SELECT w_tax FROM warehouse WHERE w_id = ?", w), &tax); err != nil {
				return err
			}
			if _, err := scan(q("SELECT d_tax, d_next_o_id FROM district WHERE d_w_id = ? AND d_id = ? FOR UPDATE", w, d), &dtax, &next); err != nil {
				return err
			}
			if err := x("UPDATE district SET d_next_o_id = ? WHERE d_w_id = ? AND d_id = ?", next+1, w, d); err != nil {
				return err
			}
			var disc, credit, last int
			if _, err := scan(q("SELECT c_discount, c_credit, c_last FROM customer WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?", w, d, cu), &disc, &credit, &last); err != nil {
				return err
			}
			if err := x("INSERT INTO orders VALUES (?, ?, ?, ?, ?, ?)", w, d, next, cu, len(lines), allLocal); err != nil {
				return err
			}
			if err := x("INSERT INTO new_order VALUES (?, ?, ?)", w, d, next); err != nil {
				return err
			}
			for n, l := range lines {
				var price int
				ok, err := scan(q("SELECT i_price FROM item WHERE i_id = ?", l.item), &price)
				if err != nil {
					return err
				}
				if !ok {
					return errRollback
				}
				var qty int
				if _, err := scan(q("SELECT s_quantity FROM stock WHERE s_w_id = ? AND s_i_id = ? FOR UPDATE", l.supply, l.item), &qty); err != nil {
					return err
				}
				if qty -= l.qty; qty < 10 {
					qty += 91
				}
				remote := 0
				if l.supply != w {
					remote = 1
				}
				if err := x("UPDATE stock SET s_quantity = ?, s_ytd = s_ytd + ?, s_order_cnt = s_order_cnt + 1, s_remote_cnt = s_remote_cnt + ? WHERE s_w_id = ? AND s_i_id = ?",
					qty, l.qty, remote, l.supply, l.item); err != nil {
					return err
				}
				if err := x("INSERT INTO order_line VALUES (?, ?, ?, ?, ?, ?, ?, ?)", w, d, next, n+1, l.item, l.supply, l.qty, l.qty*price); err != nil {
					return err
				}
			}
			return nil
		})
	}
}

func (cl *client) payment() func(context.Context) error {
	r, c := cl.r, cl.c
	w, d := 1+r.IntN(c.warehouses), 1+r.IntN(c.districts)
	cw, cd, cu := w, d, 1+r.IntN(c.customers)
	if c.warehouses > 1 && r.IntN(100) < 15 {
		cw, cd = 1+r.IntN(c.warehouses), 1+r.IntN(c.districts)
	}
	byName, last := r.IntN(100) < 60, r.IntN(max(1, c.customers/3))
	h := 1 + r.IntN(5000)
	hid := cl.uniq()
	return func(ctx context.Context) error {
		return cl.txn(ctx, func(q func(string, ...any) *sql.Rows, x func(string, ...any) error) error {
			if err := x("UPDATE warehouse SET w_ytd = w_ytd + ? WHERE w_id = ?", h, w); err != nil {
				return err
			}
			var name string
			if _, err := scan(q("SELECT w_name FROM warehouse WHERE w_id = ?", w), &name); err != nil {
				return err
			}
			if err := x("UPDATE district SET d_ytd = d_ytd + ? WHERE d_w_id = ? AND d_id = ?", h, w, d); err != nil {
				return err
			}
			if _, err := scan(q("SELECT d_name FROM district WHERE d_w_id = ? AND d_id = ?", w, d), &name); err != nil {
				return err
			}
			id := cu
			if byName {
				rows := q("SELECT c_id FROM customer WHERE c_w_id = ? AND c_d_id = ? AND c_last = ? ORDER BY c_id", cw, cd, last)
				if rows == nil {
					return errors.New("no result")
				}
				var ids []int
				for rows.Next() {
					var v int
					rows.Scan(&v)
					ids = append(ids, v)
				}
				rows.Close()
				if len(ids) == 0 {
					return fmt.Errorf("no customer named %d", last)
				}
				id = ids[(len(ids)-1)/2]
			}
			var credit, disc int
			if _, err := scan(q("SELECT c_credit, c_discount FROM customer WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?", cw, cd, id), &credit, &disc); err != nil {
				return err
			}
			if err := x("UPDATE customer SET c_balance = c_balance - ?, c_ytd_payment = c_ytd_payment + ?, c_payment_cnt = c_payment_cnt + 1 WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?",
				h, h, cw, cd, id); err != nil {
				return err
			}
			return x("INSERT INTO history VALUES (?, ?, ?, ?, ?)", hid, id, cd, cw, h)
		})
	}
}
