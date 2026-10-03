package sqlgms

import (
	"context"
	"database/sql"
	"fmt"
	"net"
	"testing"

	_ "github.com/go-sql-driver/mysql"

	"cell-tnx/txn"
)

func freeAddr(t *testing.T) string {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().String()
}

func start(t *testing.T, dir string) (*Server, *sql.DB) {
	t.Helper()
	addr := freeAddr(t)
	s, err := Serve(ServerConfig{Addr: addr, Mode: txn.CellDelta, BucketBits: 2, Dir: dir})
	if err != nil {
		t.Fatal(err)
	}
	go s.Start()
	db, err := sql.Open("mysql", fmt.Sprintf("root@tcp(%s)/?interpolateParams=true", addr))
	if err != nil {
		t.Fatal(err)
	}
	return s, db
}

// A durable server recovers its catalog, rows, secondary indexes and
// auto-increment counter after a restart.
func TestServerRestart(t *testing.T) {
	dir := t.TempDir()
	s, db := start(t, dir)
	for _, q := range []string{
		"CREATE DATABASE app",
		"USE app",
		"CREATE TABLE acct (id INT AUTO_INCREMENT PRIMARY KEY, owner VARCHAR(20), balance INT NOT NULL, CHECK (balance >= 0))",
		"CREATE UNIQUE INDEX by_owner ON acct (owner)",
		"INSERT INTO acct (owner, balance) VALUES ('ann', 10), ('bob', 20)",
		"UPDATE acct SET balance = balance - 3 WHERE owner = 'ann'",
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	db.Close()
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}

	s, db = start(t, dir)
	defer s.Close()
	defer db.Close()
	conn, err := db.Conn(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if _, err := conn.ExecContext(t.Context(), "USE app"); err != nil {
		t.Fatal(err)
	}
	if _, err := conn.ExecContext(t.Context(), "INSERT INTO acct (owner, balance) VALUES ('cat', 5)"); err != nil {
		t.Fatal(err)
	}
	var id, balance int
	if err := conn.QueryRowContext(t.Context(), "SELECT id, balance FROM acct WHERE owner = 'ann'").Scan(&id, &balance); err != nil || balance != 7 {
		t.Fatalf("ann = %d, %d, %v; want balance 7", id, balance, err)
	}
	if err := conn.QueryRowContext(t.Context(), "SELECT id FROM acct WHERE owner = 'cat'").Scan(&id); err != nil || id != 3 {
		t.Fatalf("cat id = %d, %v; want 3", id, err)
	}
	if _, err := conn.ExecContext(t.Context(), "INSERT INTO acct (owner, balance) VALUES ('bob', 1)"); err == nil {
		t.Fatal("duplicate owner accepted after restart")
	}
	if _, err := conn.ExecContext(t.Context(), "UPDATE acct SET balance = balance - 100 WHERE owner = 'bob'"); err == nil {
		t.Fatal("CHECK not enforced after restart")
	}
}

// Over the wire, cached statements return the same results, affected-row
// counts, errors and warnings as fully planned ones.
func TestServerPlanCache(t *testing.T) {
	s, db := start(t, "")
	defer s.Close()
	defer db.Close()
	ctx := context.Background()
	c, err := db.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	exec := func(q string) (int64, error) {
		res, err := c.ExecContext(ctx, q)
		if err != nil {
			return 0, err
		}
		return res.RowsAffected()
	}
	for _, q := range []string{
		"CREATE DATABASE app", "USE app",
		"CREATE TABLE acct (id INT PRIMARY KEY, owner VARCHAR(20), balance INT NOT NULL, CHECK (balance >= 0))",
		"INSERT INTO acct VALUES (1, 'ann', 10), (2, 'bob', 20)",
	} {
		if _, err := exec(q); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	var firstErr string
	for i := range 3 {
		id := 1 + i%2
		if n, err := exec(fmt.Sprintf("UPDATE acct SET balance = balance + %d WHERE id = %d", i+1, id)); err != nil || n != 1 {
			t.Fatalf("round %d update: %d rows, %v", i, n, err)
		}
		if n, err := exec("UPDATE acct SET balance = balance + 1 WHERE id = 9"); err != nil || n != 0 {
			t.Fatalf("round %d missing row: %d rows, %v", i, n, err)
		}
		for _, q := range []string{"START TRANSACTION", "UPDATE acct SET owner = 'x' WHERE id = 1", "ROLLBACK"} {
			if _, err := exec(q); err != nil {
				t.Fatalf("round %d %s: %v", i, q, err)
			}
		}
		_, err := exec("UPDATE acct SET balance = balance - 100 WHERE id = 2")
		if err == nil {
			t.Fatalf("round %d: CHECK violation succeeded", i)
		}
		if i == 0 {
			firstErr = err.Error()
		} else if err.Error() != firstErr {
			t.Fatalf("round %d: cached plan errs %q, planned errs %q", i, err, firstErr)
		}
		// A cached statement clears the previous statement's warnings.
		var v int
		if err := c.QueryRowContext(ctx, "SELECT CAST('7x' AS SIGNED)").Scan(&v); err != nil {
			t.Fatal(err)
		}
		var owner string
		if err := c.QueryRowContext(ctx, fmt.Sprintf("SELECT owner FROM acct WHERE id = %d", id)).Scan(&owner); err != nil {
			t.Fatal(err)
		}
		rows, err := c.QueryContext(ctx, "SHOW WARNINGS")
		if err != nil {
			t.Fatal(err)
		}
		n := 0
		for rows.Next() {
			n++
		}
		rows.Close()
		if n != 0 {
			t.Fatalf("round %d: %d warnings after a cached SELECT", i, n)
		}
	}
	var b1, b2 int
	if err := c.QueryRowContext(ctx, "SELECT balance FROM acct WHERE id = 1").Scan(&b1); err != nil {
		t.Fatal(err)
	}
	if err := c.QueryRowContext(ctx, "SELECT balance FROM acct WHERE id = 2").Scan(&b2); err != nil {
		t.Fatal(err)
	}
	if b1 != 14 || b2 != 22 {
		t.Fatalf("balances %d, %d; want 14, 22", b1, b2)
	}
	if s.Cache.Hits() < 10 {
		t.Fatalf("%d cache hits over the wire", s.Cache.Hits())
	}
}

// Prepared statements run through the cache over the wire.
func TestServerPreparedPlanCache(t *testing.T) {
	s, db := start(t, "")
	defer s.Close()
	defer db.Close()
	for _, q := range []string{"CREATE DATABASE app", "USE app",
		"CREATE TABLE acct (id INT PRIMARY KEY, owner VARCHAR(20), balance INT NOT NULL)",
		"INSERT INTO acct VALUES (1, 'ann', 10), (2, 'bob', 20)"} {
		if _, err := db.Exec(q); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	pdb, err := sql.Open("mysql", fmt.Sprintf("root@tcp(%s)/app", s.Listener.Addr()))
	if err != nil {
		t.Fatal(err)
	}
	defer pdb.Close()
	pdb.SetMaxOpenConns(1)
	for i := range 4 {
		id := 1 + i%2
		if _, err := pdb.Exec("UPDATE acct SET balance = balance + ? WHERE id = ?", i+1, id); err != nil {
			t.Fatal(err)
		}
		if _, err := pdb.Exec("INSERT INTO acct VALUES (?, ?, ?)", 10+i, "x", i); err != nil {
			t.Fatal(err)
		}
		var owner string
		var bal int
		if err := pdb.QueryRow("SELECT owner, balance FROM acct WHERE id = ?", id).Scan(&owner, &bal); err != nil {
			t.Fatal(err)
		}
	}
	var b1, b2, n int
	pdb.QueryRow("SELECT balance FROM acct WHERE id = ?", 1).Scan(&b1)
	pdb.QueryRow("SELECT balance FROM acct WHERE id = ?", 2).Scan(&b2)
	pdb.QueryRow("SELECT COUNT(*) FROM acct").Scan(&n)
	if b1 != 14 || b2 != 26 || n != 6 {
		t.Fatalf("balances %d, %d and %d rows; want 14, 26 and 6", b1, b2, n)
	}
	if s.Cache.Hits() < 8 {
		t.Fatalf("%d cache hits for prepared statements", s.Cache.Hits())
	}
}
