package sqlgms

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/dolthub/go-mysql-server/sql"

	"cell-tnx/txn"
)

type cached struct {
	t  *testing.T
	pc *PlanCache
	s  *Session
}

func (c *cached) query(q string) ([]sql.Row, error) {
	ctx := sql.NewContext(context.Background(), sql.WithSession(c.s))
	ctx.SetCurrentDatabase("db")
	defer c.s.CommandEnd()
	_, iter, _, err := c.pc.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	return sql.RowIterToRows(ctx, iter)
}

func (c *cached) rows(q string) string {
	c.t.Helper()
	rows, err := c.query(q)
	if err != nil {
		c.t.Fatalf("%s: %v", q, err)
	}
	return fmt.Sprint(rows)
}

// hit runs q and requires it to run a cached plan, or not.
func (c *cached) hit(q string, want bool) string {
	c.t.Helper()
	before := c.pc.Hits()
	out := c.rows(q)
	if got := c.pc.Hits() > before; got != want {
		c.t.Fatalf("%s: hit %v, want %v", q, got, want)
	}
	return out
}

func TestPlanCache(t *testing.T) {
	v := newEnv(t, txn.CellDelta)
	v.session().exec("CREATE TABLE district (w INT, d INT, ytd BIGINT, nxt INT, PRIMARY KEY (w, d))")
	v.session().exec("INSERT INTO district VALUES (1, 1, 0, 10), (1, 2, 0, 20)")
	pc := NewPlanCache(v.e, v.p)
	pc.Verify = true
	c := &cached{t, pc, NewSession(sql.NewBaseSession(), v.p)}

	c.hit("SELECT name, wallet FROM users WHERE id = 1", false)
	if got := c.hit("SELECT name, wallet FROM users WHERE id = 2", true); got != "[[b 100]]" {
		t.Fatalf("got %s", got)
	}
	if got := c.hit("SELECT name, wallet FROM users WHERE id = 7", true); got != "[]" {
		t.Fatalf("missing row: got %s", got)
	}
	c.hit("SELECT ytd, nxt FROM district WHERE w = 1 AND d = 1", false)
	if got := c.hit("SELECT ytd, nxt FROM district WHERE w = 1 AND d = 2 FOR UPDATE", false); got != "[[0 20]]" {
		t.Fatalf("got %s", got)
	}
	if got := c.hit("SELECT ytd, nxt FROM district WHERE w = 1 AND d = 2", true); got != "[[0 20]]" {
		t.Fatalf("got %s", got)
	}

	c.hit("UPDATE users SET wallet = wallet + 3 WHERE id = 1", false)
	c.hit("UPDATE users SET wallet = wallet + 4 WHERE id = 2", true)
	c.hit("UPDATE district SET nxt = 11, ytd = ytd + 5 WHERE w = 1 AND d = 1", false)
	c.hit("UPDATE district SET nxt = 21, ytd = ytd + 6 WHERE w = 1 AND d = 2", true)
	c.hit("UPDATE users SET name = 'x' WHERE id = 1", false)
	c.hit("UPDATE users SET name = 'y' WHERE id = 2", true)
	if got := c.rows("SELECT id, name, wallet FROM users ORDER BY id"); got != "[[1 x 8] [2 y 104]]" {
		t.Fatalf("users: got %s", got)
	}
	if got := c.rows("SELECT w, d, ytd, nxt FROM district ORDER BY d"); got != "[[1 1 5 11] [1 2 6 21]]" {
		t.Fatalf("district: got %s", got)
	}
	if _, err := c.query("UPDATE users SET wallet = wallet - 9 WHERE id = 1"); !sql.ErrCheckConstraintViolated.Is(err) {
		t.Fatalf("CHECK through a cached plan: got %v", err)
	}

	for range 2 {
		c.rows("START TRANSACTION")
		c.rows("UPDATE users SET wallet = wallet + 1 WHERE id = 1")
		c.rows("COMMIT")
		c.rows("START TRANSACTION")
		c.rows("UPDATE users SET wallet = wallet + 100 WHERE id = 1")
		c.rows("ROLLBACK")
	}
	if got := c.hit("SELECT wallet FROM users WHERE id = 1", false); got != "[[10]]" {
		t.Fatalf("after transactions: got %s", got)
	}

	c.hit("INSERT INTO users VALUES (3, 'c', 'c@x', 7)", false)
	c.hit("INSERT INTO users VALUES (4, 'd', 'd@x', 8)", true)
	c.hit("INSERT INTO users VALUES (5, NULL, 'e@x', 1)", false)
	c.hit("INSERT INTO users VALUES (6, NULL, 'f@x', 2)", true)
	c.hit("INSERT INTO users (id, wallet) VALUES (7, 1), (8, 2)", false)
	c.hit("INSERT INTO users (id, wallet) VALUES (9, 3), (10, 4)", true)
	if got := c.rows("SELECT id, name, email, wallet FROM users WHERE id > 2 ORDER BY id"); got != "[[3 c c@x 7] [4 d d@x 8] [5 <nil> e@x 1] [6 <nil> f@x 2] [7 <nil> <nil> 1] [8 <nil> <nil> 2] [9 <nil> <nil> 3] [10 <nil> <nil> 4]]" {
		t.Fatalf("inserted rows: got %s", got)
	}
	before := c.pc.Hits()
	if _, err := c.query("INSERT INTO users VALUES (4, 'z', 'z@x', 1)"); err == nil || !strings.Contains(strings.ToLower(err.Error()), "duplicate") || c.pc.Hits() == before {
		t.Fatalf("duplicate key through a cached plan: %v", err)
	}

	// A literal of another type, or a schema change, plans afresh.
	c.hit("SELECT wallet FROM users WHERE id = 300", false)
	c.hit("SELECT wallet FROM users WHERE id = 1", true)
	v.session().exec("CREATE INDEX by_name ON users (name)")
	c.hit("SELECT wallet FROM users WHERE id = 1", false)
	c.hit("SELECT wallet FROM users WHERE id = 2", true)

	// Shapes outside the cache never hit.
	for range 2 {
		c.hit("SELECT wallet FROM users WHERE id = 1 ORDER BY id", false)
		c.hit("SELECT wallet FROM users WHERE name = 'x'", false)
		c.hit("UPDATE users SET wallet = wallet * 2 WHERE id = 2", false)
		c.hit("SELECT wallet FROM users WHERE id = 1 AND wallet = 10", false)
	}

	// The analyzer reads an auto-increment column's literals, so such
	// tables stay uncached.
	v.session().exec("CREATE TABLE ai (id INT AUTO_INCREMENT PRIMARY KEY, v INT)")
	for range 2 {
		c.hit("INSERT INTO ai VALUES (0, 1)", false)
	}
	if got := c.rows("SELECT id, v FROM ai ORDER BY id"); got != "[[1 1] [2 1]]" {
		t.Fatalf("auto-increment rows: got %s", got)
	}
}

// Sessions share cached plans across goroutines; run with -race.
func TestPlanCacheConcurrent(t *testing.T) {
	v := newEnv(t, txn.CellDelta)
	const clients, rounds = 8, 200
	for i := 3; i < 3+clients; i++ {
		v.session().exec(fmt.Sprintf("INSERT INTO users VALUES (%d, 'n', 'e', 0)", i))
	}
	pc := NewPlanCache(v.e, v.p)
	var wg sync.WaitGroup
	for i := 3; i < 3+clients; i++ {
		c := &cached{t, pc, NewSession(sql.NewBaseSession(), v.p)}
		wg.Go(func() {
			for r := 1; r <= rounds; r++ {
				for {
					_, err := c.query(fmt.Sprintf("UPDATE users SET wallet = wallet + 1 WHERE id = %d", i))
					if err == nil {
						break
					}
					if !sql.ErrLockDeadlock.Is(err) {
						t.Error(err)
						return
					}
				}
				rows, err := c.query(fmt.Sprintf("SELECT wallet FROM users WHERE id = %d", i))
				if err != nil || fmt.Sprint(rows) != fmt.Sprintf("[[%d]]", r) {
					t.Errorf("client %d round %d: %v %v", i, r, rows, err)
					return
				}
			}
		})
	}
	wg.Wait()
	if pc.Hits() < clients*rounds {
		t.Fatalf("%d hits, want at least %d", pc.Hits(), clients*rounds)
	}
}

// Verify catches a template that rebuilds the wrong plan.
func TestPlanCacheVerify(t *testing.T) {
	v := newEnv(t, txn.CellDelta)
	v.session().exec("CREATE TABLE district (w INT, d INT, nxt INT, PRIMARY KEY (w, d))")
	v.session().exec("INSERT INTO district VALUES (1, 2, 12), (2, 1, 21)")
	pc := NewPlanCache(v.e, v.p)
	pc.Verify = true
	c := &cached{t, pc, NewSession(sql.NewBaseSession(), v.p)}
	c.hit("SELECT nxt FROM district WHERE w = 1 AND d = 2", false)
	pc.plans.Range(func(_, v any) bool {
		if tp := v.(*template); len(tp.keyLits) == 2 {
			tp.keyLits[0], tp.keyLits[1] = tp.keyLits[1], tp.keyLits[0]
		}
		return true
	})
	if _, err := c.query("SELECT nxt FROM district WHERE w = 2 AND d = 1"); err == nil || !strings.Contains(err.Error(), "differs from analyzed plan") {
		t.Fatalf("corrupt template: got %v, want a plan mismatch", err)
	}
}
