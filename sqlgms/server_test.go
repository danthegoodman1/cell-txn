package sqlgms

import (
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
