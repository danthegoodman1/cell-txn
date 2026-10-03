package sqlgms

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	gms "github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/variables"
)

// The process list updates go-mysql-server's global status variables,
// which engine construction otherwise initializes.
func init() { variables.InitStatusVariables() }

func readySession(pl *processList, id uint32, host, user, db string) gms.Session {
	pl.AddConnection(id, host)
	sess := gms.NewBaseSessionWithClientServer("0.0.0.0:3306", gms.Client{Address: host, User: user}, id)
	sess.SetCurrentDatabase(db)
	pl.ConnectionReady(sess)
	return sess
}

func begin(t *testing.T, pl *processList, sess gms.Session, pid uint64, query string) *gms.Context {
	t.Helper()
	ctx, err := pl.BeginQuery(gms.NewContext(context.Background(), gms.WithPid(pid), gms.WithSession(sess)), query)
	if err != nil {
		t.Fatal(err)
	}
	return ctx
}

func processes(pl *processList) []gms.Process {
	ps := pl.Processes()
	for i := range ps {
		ps[i].Kill = nil
	}
	sort.Slice(ps, func(i, j int) bool { return ps[i].Connection < ps[j].Connection })
	return ps
}

// Ported from go-mysql-server's TestProcessList.
func TestProcessList(t *testing.T) {
	variables.InitStatusVariables()
	pl := newProcessList()
	s1 := readySession(pl, 1, "127.0.0.1:34567", "foo", "test_db")
	ctx := begin(t, pl, s1, 1, "SELECT foo")
	pl.AddTableProgress(1, "a", 5)
	pl.AddTableProgress(1, "b", 6)

	ps := processes(pl)
	want := gms.Process{
		QueryPid:   1,
		Connection: 1,
		Host:       "127.0.0.1:34567",
		Progress: map[string]gms.TableProgress{
			"a": {Progress: gms.Progress{Name: "a", Total: 5}, PartitionsProgress: map[string]gms.PartitionProgress{}},
			"b": {Progress: gms.Progress{Name: "b", Total: 6}, PartitionsProgress: map[string]gms.PartitionProgress{}},
		},
		User:      "foo",
		Query:     "SELECT foo",
		Command:   gms.ProcessCommandQuery,
		StartedAt: ps[0].StartedAt,
		Database:  "test_db",
	}
	if len(ps) != 1 || !reflect.DeepEqual(ps[0], want) {
		t.Fatalf("processes = %+v, want %+v", ps, want)
	}

	pl.AddPartitionProgress(1, "b", "b-1", -1)
	pl.AddPartitionProgress(1, "b", "b-2", -1)
	pl.AddPartitionProgress(1, "b", "b-3", -1)
	pl.UpdatePartitionProgress(1, "b", "b-2", 1)
	pl.RemovePartitionProgress(1, "b", "b-3")
	wantProgress := map[string]gms.TableProgress{
		"a": {Progress: gms.Progress{Name: "a", Total: 5}, PartitionsProgress: map[string]gms.PartitionProgress{}},
		"b": {Progress: gms.Progress{Name: "b", Total: 6}, PartitionsProgress: map[string]gms.PartitionProgress{
			"b-1": {Progress: gms.Progress{Name: "b-1", Total: -1}},
			"b-2": {Progress: gms.Progress{Name: "b-2", Done: 1, Total: -1}},
		}},
	}
	if got := processes(pl)[0].Progress; !reflect.DeepEqual(got, wantProgress) {
		t.Fatalf("progress = %+v, want %+v", got, wantProgress)
	}

	s2 := readySession(pl, 2, "127.0.0.1:34568", "foo", "")
	ctx2 := begin(t, pl, s2, 2, "SELECT bar")
	pl.AddTableProgress(2, "foo", 2)
	pl.UpdateTableProgress(1, "a", 3)
	pl.UpdateTableProgress(1, "a", 1)
	pl.UpdateTableProgress(1, "b", 2)
	pl.UpdateTableProgress(2, "foo", 1)
	ps = processes(pl)
	if len(ps) != 2 || ps[0].Progress["a"].Done != 4 || ps[0].Progress["b"].Done != 2 || ps[1].Progress["foo"].Done != 1 {
		t.Fatalf("processes = %+v", ps)
	}

	pl.EndQuery(ctx2)
	ps = processes(pl)
	if len(ps) != 2 || ps[1].Command != gms.ProcessCommandSleep || ps[1].QueryPid != 0 || len(ps[1].Progress) != 0 {
		t.Fatalf("after EndQuery: %+v", ps[1])
	}
	if ctx2.Err() == nil {
		t.Fatal("EndQuery left the query context live")
	}
	if ctx.Err() != nil {
		t.Fatal("EndQuery canceled another connection's query")
	}
}

// Progress for a finished query never reaches the connection's next one.
func TestProcessListStaleProgress(t *testing.T) {
	pl := newProcessList()
	s := readySession(pl, 1, "h", "u", "")
	pl.EndQuery(begin(t, pl, s, 7, "q1"))
	ctx := begin(t, pl, s, 8, "q2")
	pl.AddTableProgress(7, "t", 5)
	pl.UpdateTableProgress(7, "t", 1)
	if p := processes(pl)[0]; len(p.Progress) != 0 {
		t.Fatalf("stale progress applied: %+v", p.Progress)
	}
	if _, err := pl.BeginQuery(gms.NewContext(context.Background(), gms.WithPid(8), gms.WithSession(s)), "dup"); !gms.ErrPidAlreadyUsed.Is(err) {
		t.Fatalf("reused pid: err = %v", err)
	}
	pl.EndQuery(ctx)
}

// Ported from go-mysql-server's TestKillConnection.
func TestProcessListKill(t *testing.T) {
	pl := newProcessList()
	s1 := readySession(pl, 1, "", "", "")
	s2 := readySession(pl, 2, "", "", "")
	c1 := begin(t, pl, s1, 3, "foo")
	c2 := begin(t, pl, s2, 4, "foo")
	pl.Kill(1)
	if c1.Err() == nil || c2.Err() != nil {
		t.Fatalf("Kill(1): ctx1 err %v, ctx2 err %v", c1.Err(), c2.Err())
	}
	if n := len(pl.Processes()); n != 2 {
		t.Fatalf("%d processes after Kill, want 2", n)
	}
	pl.RemoveConnection(2)
	if c2.Err() == nil {
		t.Fatal("RemoveConnection left the query context live")
	}
	if _, err := pl.BeginQuery(gms.NewContext(context.Background(), gms.WithPid(5), gms.WithSession(s2)), "foo"); err == nil {
		t.Fatal("BeginQuery on a removed connection succeeded")
	}
}

// Ported from go-mysql-server's TestBeginEndOperation.
func TestProcessListOperations(t *testing.T) {
	known := gms.NewBaseSessionWithClientServer("", gms.Client{}, 1)
	unknown := gms.NewBaseSessionWithClientServer("", gms.Client{}, 2)
	pl := newProcessList()
	pl.AddConnection(1, "")

	if _, err := pl.BeginOperation(gms.NewContext(context.Background(), gms.WithSession(unknown))); err == nil {
		t.Fatal("operation on an unknown connection succeeded")
	}
	ctx := gms.NewContext(context.Background(), gms.WithSession(known))
	op := func() *gms.Context {
		t.Helper()
		sub, err := pl.BeginOperation(ctx)
		if err != nil {
			t.Fatal(err)
		}
		return sub
	}

	pl.EndOperation(op())
	// An operation spanning ConnectionReady stays cancelable.
	sub := op()
	pl.ConnectionReady(known)
	pl.EndOperation(sub)
	if sub.Err() == nil {
		t.Fatal("EndOperation after ConnectionReady left the operation live")
	}
	sub = op()
	pl.Kill(1)
	if sub.Err() == nil {
		t.Fatal("Kill left the operation live")
	}
	pl.EndOperation(sub)
	sub = op()
	if _, err := pl.BeginOperation(ctx); err == nil {
		t.Fatal("second concurrent operation succeeded")
	}
	pl.EndOperation(sub)
}

// Ported from go-mysql-server's TestSlowQueryTracking.
func TestProcessListSlowQuery(t *testing.T) {
	variables.InitStatusVariables()
	pl := newProcessList()
	s := readySession(pl, 1, "h", "u", "")
	ctx := begin(t, pl, s, 1, "SELECT foo")
	_, old, _ := gms.SystemVariables.GetGlobal("long_query_time")
	if err := gms.SystemVariables.SetGlobal(ctx, "long_query_time", 0.05); err != nil {
		t.Fatal(err)
	}
	defer gms.SystemVariables.SetGlobal(ctx, "long_query_time", old)
	time.Sleep(100 * time.Millisecond)
	pl.EndQuery(ctx)
	for range 20 {
		if _, v, _ := gms.StatusVariables.GetGlobal("Slow_queries"); v == uint64(1) {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatal("Slow_queries never reached 1")
}

// Connections run statements concurrently while another goroutine lists
// and kills them; run with -race.
func TestProcessListConcurrent(t *testing.T) {
	pl := newProcessList()
	const conns, stmts = 16, 2000
	var wg sync.WaitGroup
	stop := make(chan struct{})
	go func() {
		for i := uint32(0); ; i++ {
			select {
			case <-stop:
				return
			default:
			}
			for _, p := range pl.Processes() {
				_ = len(p.Progress)
			}
			pl.Kill(1 + i%conns)
		}
	}()
	for c := range uint32(conns) {
		s := readySession(pl, c+1, "h", "u", "")
		wg.Go(func() {
			for i := range uint64(stmts) {
				pid := uint64(c)*stmts + i + 1
				ctx, err := pl.BeginQuery(gms.NewContext(context.Background(), gms.WithPid(pid), gms.WithSession(s)), "q")
				if err != nil {
					t.Error(err)
					return
				}
				pl.AddTableProgress(pid, "t", 10)
				pl.AddPartitionProgress(pid, "t", "p", -1)
				pl.UpdatePartitionProgress(pid, "t", "p", 1)
				pl.UpdateTableProgress(pid, "t", 1)
				pl.EndQuery(ctx)
			}
		})
	}
	wg.Wait()
	close(stop)
	for _, p := range processes(pl) {
		if p.Command != gms.ProcessCommandSleep || p.QueryPid != 0 {
			t.Fatalf("connection %d left as %+v", p.Connection, p)
		}
	}
	for i := range pl.pids {
		if n := len(pl.pids[i].m); n != 0 {
			t.Fatalf("pid shard %d holds %d entries", i, n)
		}
	}
}

// Over the wire, SHOW PROCESSLIST lists a running statement and KILL QUERY
// interrupts it while the connection stays usable.
func TestServerKillQuery(t *testing.T) {
	s, db := start(t, "")
	defer s.Close()
	defer db.Close()
	ctx := context.Background()
	a, err := db.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	b, err := db.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()
	var aID int64
	if err := a.QueryRowContext(ctx, "SELECT CONNECTION_ID()").Scan(&aID); err != nil {
		t.Fatal(err)
	}

	done := make(chan error, 1)
	t0 := time.Now()
	go func() {
		_, err := a.ExecContext(ctx, "SELECT SLEEP(30)")
		done <- err
	}()
	if err := waitForQuery(ctx, b, aID, "SLEEP(30)"); err != nil {
		t.Fatal(err)
	}
	if _, err := b.ExecContext(ctx, fmt.Sprintf("KILL QUERY %d", aID)); err != nil {
		t.Fatal(err)
	}
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("KILL QUERY did not interrupt SLEEP")
	}
	if d := time.Since(t0); d > 10*time.Second {
		t.Fatalf("SLEEP ran %v after KILL QUERY", d)
	}
	var one int
	if err := a.QueryRowContext(ctx, "SELECT 1").Scan(&one); err != nil || one != 1 {
		t.Fatalf("connection unusable after KILL QUERY: %v", err)
	}
}

// waitForQuery polls SHOW PROCESSLIST on c until connection id runs a
// statement containing text.
func waitForQuery(ctx context.Context, c *sql.Conn, id int64, text string) error {
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		rows, err := c.QueryContext(ctx, "SHOW PROCESSLIST")
		if err != nil {
			return err
		}
		cols, _ := rows.Columns()
		vals := make([]sql.NullString, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		found := false
		for rows.Next() {
			if err := rows.Scan(ptrs...); err != nil {
				rows.Close()
				return err
			}
			row := map[string]string{}
			for i, col := range cols {
				row[strings.ToLower(col)] = vals[i].String
			}
			if row["id"] == fmt.Sprint(id) && row["command"] == "Query" && strings.Contains(row["info"], text) {
				found = true
			}
		}
		rows.Close()
		if found {
			return nil
		}
		time.Sleep(10 * time.Millisecond)
	}
	return errors.New("SHOW PROCESSLIST never showed the running statement")
}
