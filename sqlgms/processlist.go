package sqlgms

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/sirupsen/logrus"

	"github.com/dolthub/go-mysql-server/sql"
)

// processList tracks connections and their running statements for SHOW
// PROCESSLIST and KILL, with go-mysql-server's semantics. go-mysql-server's
// own list takes one global lock at least twice per statement; here each
// connection has its own lock, so concurrent statements never wait on one
// another. The connection map is write-locked only to add or remove a
// connection, and progress updates find their connection through a
// sharded map of running query pids.
type processList struct {
	mu    sync.RWMutex
	procs map[uint32]*proc
	pids  [64]pidShard
}

// proc is one connection's entry. Kill, when set, cancels the running
// statement or operation.
type proc struct {
	mu      sync.Mutex
	p       sql.Process
	removed bool
}

type pidShard struct {
	mu sync.Mutex
	m  map[uint64]*proc
}

var _ sql.ProcessList = (*processList)(nil)

func newProcessList() *processList {
	pl := &processList{procs: map[uint32]*proc{}}
	for i := range pl.pids {
		pl.pids[i].m = map[uint64]*proc{}
	}
	return pl
}

func (pl *processList) conn(id uint32) *proc {
	pl.mu.RLock()
	defer pl.mu.RUnlock()
	return pl.procs[id]
}

func (pl *processList) shard(pid uint64) *pidShard { return &pl.pids[pid%uint64(len(pl.pids))] }

// running returns the connection running query pid; with end, it also
// unregisters the pid.
func (pl *processList) running(pid uint64, end bool) *proc {
	s := pl.shard(pid)
	s.mu.Lock()
	defer s.mu.Unlock()
	pr := s.m[pid]
	if end {
		delete(s.m, pid)
	}
	return pr
}

// progress runs f on the progress of query pid while it still runs.
func (pl *processList) progress(pid uint64, f func(map[string]sql.TableProgress)) {
	pr := pl.running(pid, false)
	if pr == nil {
		return
	}
	pr.mu.Lock()
	defer pr.mu.Unlock()
	if pr.p.QueryPid == pid {
		f(pr.p.Progress)
	}
}

// Processes returns a deep copy of every connection's entry.
func (pl *processList) Processes() []sql.Process {
	pl.mu.RLock()
	defer pl.mu.RUnlock()
	out := make([]sql.Process, 0, len(pl.procs))
	for _, pr := range pl.procs {
		pr.mu.Lock()
		p := pr.p
		p.Progress = make(map[string]sql.TableProgress, len(pr.p.Progress))
		for name, tp := range pr.p.Progress {
			parts := make(map[string]sql.PartitionProgress, len(tp.PartitionsProgress))
			for k, v := range tp.PartitionsProgress {
				parts[k] = v
			}
			p.Progress[name] = sql.TableProgress{Progress: tp.Progress, PartitionsProgress: parts}
		}
		pr.mu.Unlock()
		out = append(out, p)
	}
	return out
}

func (pl *processList) AddConnection(id uint32, addr string) {
	sql.StatusVariables.IncrementGlobal("Threads_connected", 1)
	pl.mu.Lock()
	defer pl.mu.Unlock()
	pl.procs[id] = &proc{p: sql.Process{
		Connection: id,
		Command:    sql.ProcessCommandConnect,
		Host:       addr,
		User:       "unauthenticated user",
		StartedAt:  time.Now(),
	}}
}

// ConnectionReady moves a connection from Connect to Sleep. An operation
// already running on it, such as authentication, stays cancelable.
func (pl *processList) ConnectionReady(sess sql.Session) {
	ready := sql.Process{
		Connection: sess.ID(),
		Command:    sql.ProcessCommandSleep,
		Host:       sess.Client().Address,
		User:       sess.Client().User,
		StartedAt:  time.Now(),
		Database:   sess.GetCurrentDatabase(),
	}
	pl.mu.Lock()
	defer pl.mu.Unlock()
	if pr := pl.procs[sess.ID()]; pr != nil {
		pr.mu.Lock()
		ready.Kill = pr.p.Kill
		pr.p = ready
		pr.mu.Unlock()
		return
	}
	pl.procs[sess.ID()] = &proc{p: ready}
}

func (pl *processList) RemoveConnection(id uint32) {
	pl.mu.Lock()
	pr := pl.procs[id]
	delete(pl.procs, id)
	pl.mu.Unlock()
	if pr == nil {
		return
	}
	sql.StatusVariables.IncrementGlobal("Threads_connected", -1)
	pr.mu.Lock()
	defer pr.mu.Unlock()
	pr.removed = true
	if pr.p.Kill != nil {
		pr.p.Kill()
	}
	if pr.p.QueryPid != 0 {
		s := pl.shard(pr.p.QueryPid)
		s.mu.Lock()
		delete(s.m, pr.p.QueryPid)
		s.mu.Unlock()
	}
}

// BeginQuery moves the connection from Sleep to Query and returns a
// context that EndQuery or Kill cancels.
func (pl *processList) BeginQuery(ctx *sql.Context, query string) (*sql.Context, error) {
	if ctx.IsInterpreted() {
		return ctx, nil
	}
	pr := pl.conn(ctx.Session.ID())
	if pr == nil {
		return nil, errors.New("internal error: connection not registered with process list")
	}
	pid := ctx.Pid()
	s := pl.shard(pid)
	pr.mu.Lock()
	defer pr.mu.Unlock()
	if pr.removed {
		return nil, errors.New("internal error: connection not registered with process list")
	}
	s.mu.Lock()
	if _, ok := s.m[pid]; ok {
		s.mu.Unlock()
		return nil, sql.ErrPidAlreadyUsed.New(pid)
	}
	s.m[pid] = pr
	s.mu.Unlock()

	sql.StatusVariables.IncrementGlobal("Threads_running", 1)
	newCtx, cancel := context.WithCancel(ctx)
	ctx = ctx.WithContext(newCtx)
	pr.p.Command = sql.ProcessCommandQuery
	pr.p.Query = query
	pr.p.QueryPid = pid
	pr.p.StartedAt = time.Now()
	pr.p.Kill = cancel
	pr.p.Progress = map[string]sql.TableProgress{}
	return ctx, nil
}

// EndQuery moves the connection back to Sleep and cancels the query's
// context.
func (pl *processList) EndQuery(ctx *sql.Context) {
	if ctx.IsInterpreted() {
		return
	}
	pid := ctx.Pid()
	pr := pl.running(pid, true)
	if pr == nil {
		return
	}
	pr.mu.Lock()
	defer pr.mu.Unlock()
	if pr.p.QueryPid != pid {
		return
	}
	if lq := longQueryTime(); lq > 0 && time.Since(pr.p.StartedAt).Seconds() > lq {
		sql.IncrementStatusVariable(ctx, "Slow_queries", 1)
	}
	sql.StatusVariables.IncrementGlobal("Threads_running", -1)
	pr.p.Command = sql.ProcessCommandSleep
	pr.p.Query = ""
	pr.p.StartedAt = time.Now()
	pr.p.Kill()
	pr.p.Kill = nil
	pr.p.QueryPid = 0
	pr.p.Progress = nil
}

// longQueryTime returns @@long_query_time in seconds, or 0 if unset.
func longQueryTime() float64 {
	_, v, ok := sql.SystemVariables.GetGlobal("long_query_time")
	if !ok {
		return 0
	}
	f, _ := v.(float64)
	return f
}

// BeginOperation registers a cancelable operation, such as a prepare,
// that leaves the connection's command unchanged.
func (pl *processList) BeginOperation(ctx *sql.Context) (*sql.Context, error) {
	if ctx.IsInterpreted() {
		return ctx, nil
	}
	pr := pl.conn(ctx.Session.ID())
	if pr == nil {
		return nil, errors.New("internal error: connection not registered with process list")
	}
	pr.mu.Lock()
	defer pr.mu.Unlock()
	if pr.removed {
		return nil, errors.New("internal error: connection not registered with process list")
	}
	if pr.p.Kill != nil {
		return nil, errors.New("internal error: attempt to begin operation on connection which was already running one")
	}
	newCtx, cancel := ctx.NewSubContext()
	pr.p.Kill = cancel
	return newCtx, nil
}

func (pl *processList) EndOperation(ctx *sql.Context) {
	if ctx.IsInterpreted() {
		return
	}
	pr := pl.conn(ctx.Session.ID())
	if pr == nil {
		return
	}
	pr.mu.Lock()
	defer pr.mu.Unlock()
	if pr.p.Kill != nil {
		pr.p.Kill()
		pr.p.Kill = nil
	}
}

// Kill cancels the connection's running statement or operation.
func (pl *processList) Kill(id uint32) {
	pr := pl.conn(id)
	if pr == nil {
		return
	}
	pr.mu.Lock()
	defer pr.mu.Unlock()
	if pr.p.Kill == nil {
		return
	}
	if pr.p.QueryPid != 0 {
		logrus.Infof("kill query: pid %d", pr.p.QueryPid)
	} else {
		logrus.Infof("canceling context: connID %d", id)
	}
	pr.p.Kill()
}

func (pl *processList) AddTableProgress(pid uint64, name string, total int64) {
	pl.progress(pid, func(m map[string]sql.TableProgress) {
		if tp, ok := m[name]; ok {
			tp.Total = total
			m[name] = tp
		} else {
			m[name] = sql.NewTableProgress(name, total)
		}
	})
}

func (pl *processList) UpdateTableProgress(pid uint64, name string, delta int64) {
	pl.progress(pid, func(m map[string]sql.TableProgress) {
		tp, ok := m[name]
		if !ok {
			tp = sql.NewTableProgress(name, -1)
		}
		tp.Done += delta
		m[name] = tp
	})
}

func (pl *processList) RemoveTableProgress(pid uint64, name string) {
	pl.progress(pid, func(m map[string]sql.TableProgress) { delete(m, name) })
}

func (pl *processList) AddPartitionProgress(pid uint64, table, partition string, total int64) {
	pl.progress(pid, func(m map[string]sql.TableProgress) {
		tp, ok := m[table]
		if !ok {
			return
		}
		pp, ok := tp.PartitionsProgress[partition]
		if !ok {
			pp = sql.PartitionProgress{Progress: sql.Progress{Name: partition}}
		}
		pp.Total = total
		tp.PartitionsProgress[partition] = pp
	})
}

func (pl *processList) UpdatePartitionProgress(pid uint64, table, partition string, delta int64) {
	pl.progress(pid, func(m map[string]sql.TableProgress) {
		tp, ok := m[table]
		if !ok {
			return
		}
		pp, ok := tp.PartitionsProgress[partition]
		if !ok {
			pp = sql.PartitionProgress{Progress: sql.Progress{Name: partition, Total: -1}}
		}
		pp.Done += delta
		tp.PartitionsProgress[partition] = pp
	})
}

func (pl *processList) RemovePartitionProgress(pid uint64, table, partition string) {
	pl.progress(pid, func(m map[string]sql.TableProgress) {
		if tp, ok := m[table]; ok {
			delete(tp.PartitionsProgress, partition)
		}
	})
}
