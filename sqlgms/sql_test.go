package sqlgms

import (
	"context"
	"fmt"
	"testing"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/sql"

	"cell-tnx/txn"
)

type env struct {
	t *testing.T
	e *sqle.Engine
	p *Provider
}

func newEnv(t *testing.T, mode txn.Mode) *env {
	p := NewProvider(txn.Open(txn.Options{Mode: mode, BucketBits: 2}))
	e := NewEngine(p)
	v := &env{t, e, p}
	s := v.session()
	s.exec("CREATE DATABASE db")
	s.exec("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(20), email VARCHAR(40) COLLATE utf8mb4_0900_ai_ci, wallet INT NOT NULL, CHECK (wallet >= 0))")
	s.exec("INSERT INTO users VALUES (1, 'a', 'a@x', 5), (2, 'b', 'b@x', 100)")
	return v
}

type sess struct {
	v *env
	s *Session
}

func (v *env) session() *sess {
	return &sess{v, NewSession(sql.NewBaseSession(), v.p)}
}

func (s *sess) query(q string) ([]sql.Row, error) {
	ctx := sql.NewContext(context.Background(), sql.WithSession(s.s))
	if s.v.p.HasDatabase(ctx, "db") {
		ctx.SetCurrentDatabase("db")
	}
	defer s.s.CommandEnd()
	_, iter, _, err := s.v.e.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	return sql.RowIterToRows(ctx, iter)
}

func (s *sess) exec(q string) []sql.Row {
	s.v.t.Helper()
	rows, err := s.query(q)
	if err != nil {
		s.v.t.Fatalf("%s: %v", q, err)
	}
	return rows
}

func (s *sess) one(q string) string {
	s.v.t.Helper()
	rows := s.exec(q)
	if len(rows) != 1 {
		s.v.t.Fatalf("%s: %d rows", q, len(rows))
	}
	return fmt.Sprint(rows[0]...)
}

func wantDeadlock(t *testing.T, err error) {
	t.Helper()
	if !sql.ErrLockDeadlock.Is(err) {
		t.Fatalf("got %v, want a deadlock error", err)
	}
}

// Scenario 1 and 9 over SQL.
func TestSQLDisjointColumns(t *testing.T) {
	for _, mode := range []txn.Mode{txn.Row, txn.Cell, txn.CellDelta} {
		v := newEnv(t, mode)
		s1, s2 := v.session(), v.session()
		s1.exec("START TRANSACTION")
		s1.exec("SELECT email FROM users WHERE id = 1")
		s1.exec("UPDATE users SET name = 'z' WHERE id = 1")
		s2.exec("UPDATE users SET wallet = 77 WHERE id = 1")
		_, err := s1.query("COMMIT")
		if mode == txn.Row {
			wantDeadlock(t, err)
			continue
		}
		if err != nil {
			t.Fatalf("%s: %v", mode, err)
		}
		if got := s2.one("SELECT name, email, wallet FROM users WHERE id = 1"); got != "za@x77" {
			t.Fatalf("%s: row = %s", mode, got)
		}
	}
}

// Scenario 2 over SQL.
func TestSQLReadWriteConflict(t *testing.T) {
	v := newEnv(t, txn.Cell)
	s1, s2 := v.session(), v.session()
	s2.exec("START TRANSACTION")
	s2.exec("SELECT email FROM users WHERE id = 1")
	s2.exec("UPDATE users SET name = 'q' WHERE id = 2")
	s1.exec("UPDATE users SET email = 'new' WHERE id = 1")
	_, err := s2.query("COMMIT")
	wantDeadlock(t, err)
	// The session is usable again after the failed commit.
	s2.exec("UPDATE users SET name = 'q' WHERE id = 2")
}

// col = col ± k commutes only in cell+delta mode, and the CHECK is
// enforced on the merged row at commit.
func TestSQLDeltas(t *testing.T) {
	for _, mode := range []txn.Mode{txn.Cell, txn.CellDelta} {
		v := newEnv(t, mode)
		s1, s2 := v.session(), v.session()
		s1.exec("START TRANSACTION")
		s2.exec("START TRANSACTION")
		s1.exec("UPDATE users SET wallet = wallet - 2 WHERE id = 1")
		s2.exec("UPDATE users SET wallet = wallet - 2 WHERE id = 1")
		s1.exec("COMMIT")
		_, err := s2.query("COMMIT")
		if mode == txn.Cell {
			wantDeadlock(t, err)
			continue
		}
		if err != nil {
			t.Fatal(err)
		}
		if got := s1.one("SELECT wallet FROM users WHERE id = 1"); got != "1" {
			t.Fatalf("wallet = %s, want 1", got)
		}
		s1.exec("START TRANSACTION")
		s2.exec("START TRANSACTION")
		s1.exec("UPDATE users SET wallet = wallet - 1 WHERE id = 1")
		s2.exec("UPDATE users SET wallet = wallet - 1 WHERE id = 1")
		s1.exec("COMMIT")
		if _, err := s2.query("COMMIT"); !sql.ErrCheckConstraintViolated.Is(err) {
			t.Fatalf("got %v, want a CHECK violation at commit", err)
		}
	}
}

// A CHECK over final values fails at the statement.
func TestSQLStatementCheck(t *testing.T) {
	v := newEnv(t, txn.CellDelta)
	s := v.session()
	if _, err := s.query("UPDATE users SET wallet = 0 - 5 WHERE id = 1"); !sql.ErrCheckConstraintViolated.Is(sql.UnwrapError(err)) {
		t.Fatalf("got %T %v, want a CHECK violation", err, err)
	}
	if _, err := s.query("INSERT INTO users VALUES (3, 'c', 'c@x', -1)"); !sql.ErrCheckConstraintViolated.Is(sql.UnwrapError(err)) {
		t.Fatalf("got %v, want a CHECK violation", err)
	}
	if got := s.one("SELECT COUNT(*) FROM users"); got != "2" {
		t.Fatalf("count = %s", got)
	}
}

// An UPDATE that assigns the value its snapshot holds skips the write, so
// it reads the assigned column: a concurrent change to it conflicts.
func TestSQLRewriteReadsTarget(t *testing.T) {
	v := newEnv(t, txn.CellDelta)
	s1, s2 := v.session(), v.session()
	s1.exec("START TRANSACTION")
	s1.exec("SELECT wallet FROM users WHERE id = 2")
	s2.exec("UPDATE users SET email = 'changed' WHERE id = 1")
	s1.exec("UPDATE users SET email = 'a@x' WHERE id = 1")
	s1.exec("UPDATE users SET wallet = 1 WHERE id = 2")
	_, err := s1.query("COMMIT")
	wantDeadlock(t, err)
}

func TestSQLUniqueIndex(t *testing.T) {
	v := newEnv(t, txn.CellDelta)
	s := v.session()
	s.exec("CREATE UNIQUE INDEX by_email ON users (email)")
	if _, err := s.query("INSERT INTO users VALUES (3, 'c', 'A@X', 1)"); !sql.ErrUniqueKeyViolation.Is(sql.UnwrapError(err)) {
		t.Fatalf("got %v, want a unique key violation (collation-insensitive)", err)
	}
	if _, err := s.query("INSERT INTO users VALUES (1, 'c', 'c@x', 1)"); !sql.ErrPrimaryKeyViolation.Is(sql.UnwrapError(err)) {
		t.Fatalf("got %v, want a primary key violation", err)
	}
	s.exec("INSERT INTO users VALUES (1, 'c', 'c@x', 1) ON DUPLICATE KEY UPDATE wallet = wallet + 10")
	if got := s.one("SELECT wallet FROM users WHERE email = 'a@x'"); got != "15" {
		t.Fatalf("wallet = %s, want 15", got)
	}
	s.exec("REPLACE INTO users VALUES (2, 'r', 'r@x', 9)")
	if got := s.one("SELECT name, wallet FROM users WHERE email = 'r@x'"); got != "r9" {
		t.Fatalf("replaced row = %s", got)
	}
	// Concurrent claims of one email: the second commit conflicts.
	s1, s2 := v.session(), v.session()
	s1.exec("START TRANSACTION")
	s2.exec("START TRANSACTION")
	s1.exec("UPDATE users SET email = 'same' WHERE id = 1")
	s2.exec("UPDATE users SET email = 'same' WHERE id = 2")
	s1.exec("COMMIT")
	_, err := s2.query("COMMIT")
	wantDeadlock(t, err)
	if err := v.p.Core().CheckInvariants(); err != nil {
		t.Fatal(err)
	}
}

// Scenario 4 over SQL: an insert into a scanned range is a phantom.
func TestSQLPhantom(t *testing.T) {
	v := newEnv(t, txn.Cell)
	s1, s2 := v.session(), v.session()
	s1.exec("START TRANSACTION")
	s1.exec("SELECT COUNT(*) FROM users WHERE id BETWEEN 1 AND 3")
	s1.exec("UPDATE users SET name = 'p' WHERE id = 2")
	s2.exec("INSERT INTO users VALUES (3, 'c', 'c@x', 1)")
	_, err := s1.query("COMMIT")
	wantDeadlock(t, err)
}

func TestSQLSavepoint(t *testing.T) {
	v := newEnv(t, txn.Cell)
	s := v.session()
	s.exec("START TRANSACTION")
	s.exec("UPDATE users SET name = 'one' WHERE id = 1")
	s.exec("SAVEPOINT sp")
	s.exec("UPDATE users SET name = 'two' WHERE id = 1")
	s.exec("DELETE FROM users WHERE id = 2")
	s.exec("ROLLBACK TO SAVEPOINT sp")
	s.exec("COMMIT")
	if got := s.one("SELECT name FROM users WHERE id = 1"); got != "one" {
		t.Fatalf("name = %s", got)
	}
	if got := s.one("SELECT COUNT(*) FROM users"); got != "2" {
		t.Fatalf("count = %s", got)
	}
	// A failed statement inside a transaction leaves earlier ones intact.
	s.exec("START TRANSACTION")
	s.exec("UPDATE users SET name = 'three' WHERE id = 1")
	if _, err := s.query("INSERT INTO users VALUES (2, 'dup', 'd', 1)"); err == nil {
		t.Fatal("duplicate insert succeeded")
	}
	s.exec("COMMIT")
	if got := s.one("SELECT name FROM users WHERE id = 1"); got != "three" {
		t.Fatalf("name = %s", got)
	}
}
