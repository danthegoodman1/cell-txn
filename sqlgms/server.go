package sqlgms

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/server"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/vitess/go/mysql"
	"github.com/dolthub/vitess/go/sqltypes"
	ast "github.com/dolthub/vitess/go/vt/sqlparser"

	"cell-tnx/store/badger"
	"cell-tnx/txn"
)

// ServerConfig configures a MySQL-protocol server over the store.
type ServerConfig struct {
	Addr       string
	Mode       txn.Mode
	BucketBits uint
	// Dir, when set, makes the server durable: commits persist in Badger
	// and catalog changes in a statement log, both under Dir.
	Dir    string
	NoSync bool // acknowledge commits before they sync
	// CommitDelay holds each sync open that long so concurrent commits
	// share it.
	CommitDelay time.Duration
	// NoPlanCache plans every statement in full.
	NoPlanCache bool
}

// Server is a running MySQL-protocol server.
type Server struct {
	*server.Server
	Engine   *sqle.Engine
	Provider *Provider
	Cache    *PlanCache // nil with NoPlanCache
	store    *badger.Store
	catalog  *os.File
}

// Serve opens the store, recovers it when durable, and starts listening.
func Serve(cfg ServerConfig) (*Server, error) {
	opts := txn.Options{Mode: cfg.Mode, BucketBits: cfg.BucketBits, NoSync: cfg.NoSync}
	if cfg.CommitDelay > 0 {
		opts.CommitDelay = func() { time.Sleep(cfg.CommitDelay) }
	}
	s := &Server{}
	if cfg.Dir != "" {
		st, err := badger.Open(filepath.Join(cfg.Dir, "data"))
		if err != nil {
			return nil, err
		}
		s.store, opts.Store = st, st
	}
	core := txn.Open(opts)
	s.Provider = NewProvider(core)
	s.Engine = NewEngine(s.Provider)
	if cfg.Dir != "" {
		if err := s.recover(cfg.Dir); err != nil {
			return nil, err
		}
		go func() {
			if err := core.RunWriter(); err != nil && !errors.Is(err, txn.ErrClosed) {
				fmt.Fprintln(os.Stderr, "cell-tnx: writer failed, stopping:", err)
				os.Exit(1)
			}
		}()
	}
	h := &cachedHandler{}
	if !cfg.NoPlanCache {
		h.cache = NewPlanCache(s.Engine, s.Provider)
		s.Cache = h.cache
	}
	sb := func(ctx context.Context, conn *mysql.Conn, addr string) (sql.Session, error) {
		base, err := sql.BaseSessionFromConnection(ctx, conn, addr)
		if err != nil {
			return nil, err
		}
		sess := NewSession(base, s.Provider)
		h.sessions.Store(conn.ConnectionID, sess)
		return sess, nil
	}
	wrap := func(inner mysql.Handler) (mysql.Handler, error) {
		gh, ok := inner.(*server.Handler)
		if !ok {
			return nil, fmt.Errorf("sqlgms: unexpected handler %T", inner)
		}
		h.Handler = gh
		return h, nil
	}
	srv, err := server.NewServerWithHandler(server.Config{Protocol: "tcp", Address: cfg.Addr}, s.Engine, sql.NewContext, sb, nil, wrap)
	if err != nil {
		return nil, err
	}
	s.Server = srv
	return s, nil
}

// recover replays the catalog's statements, loads the rows Badger holds,
// and resumes auto-increment counters, then logs new catalog changes.
func (s *Server) recover(dir string) error {
	path := filepath.Join(dir, "catalog.jsonl")
	if f, err := os.Open(path); err == nil {
		sess := NewSession(sql.NewBaseSession(), s.Provider)
		sc := bufio.NewScanner(f)
		sc.Buffer(nil, 1<<24)
		for sc.Scan() {
			var q struct{ DB, Query string }
			if err := json.Unmarshal(sc.Bytes(), &q); err != nil {
				return fmt.Errorf("catalog: %w", err)
			}
			ctx := sql.NewContext(context.Background(), sql.WithSession(sess))
			if q.DB != "" {
				ctx.SetCurrentDatabase(q.DB)
			}
			_, it, _, err := s.Engine.Query(ctx, q.Query)
			if err == nil {
				_, err = sql.RowIterToRows(ctx, it)
			}
			if err != nil {
				return fmt.Errorf("catalog %q: %w", q.Query, err)
			}
		}
		f.Close()
		if err := sc.Err(); err != nil {
			return err
		}
	}
	if err := s.Provider.core.Recover(s.store.Last(), s.store.Load); err != nil {
		return err
	}
	if err := s.Provider.resumeAutoIncrement(); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	s.catalog = f
	s.Provider.OnDDL = func(db, query string) error {
		b, _ := json.Marshal(struct{ DB, Query string }{db, query})
		if _, err := f.Write(append(b, '\n')); err != nil {
			return err
		}
		return f.Sync()
	}
	return nil
}

// resumeAutoIncrement sets each auto-increment counter past the largest
// value its column holds.
func (p *Provider) resumeAutoIncrement() error {
	tx := p.core.Begin()
	defer tx.Abort()
	for _, d := range p.AllDatabases(sql.NewEmptyContext()) {
		for _, t := range d.(*Database).tables {
			if t.autoInc < 0 {
				continue
			}
			recs, err := tx.ScanRows(t.id, -1, "", "", t.all())
			if err != nil {
				return err
			}
			for _, r := range recs {
				switch v := r.Row[t.autoInc].(type) {
				case int64:
					if v > 0 {
						p.core.BumpID(t.id, uint64(v))
					}
				case uint64:
					p.core.BumpID(t.id, v)
				}
			}
		}
	}
	return nil
}

// Close stops the server and the store.
func (s *Server) Close() error {
	err := s.Server.Close()
	s.Provider.core.Close()
	if s.store != nil {
		err = errors.Join(err, s.store.Close())
	}
	if s.catalog != nil {
		err = errors.Join(err, s.catalog.Close())
	}
	return err
}

// cachedHandler runs each statement the plan cache covers as a bound plan
// through go-mysql-server's handler, which still owns transactions, the
// process list, results and errors; other statements take its ComQuery.
type cachedHandler struct {
	*server.Handler
	cache    *PlanCache
	sessions sync.Map // connection ID -> *Session
}

func (h *cachedHandler) ComQuery(ctx context.Context, c *mysql.Conn, query string, callback mysql.ResultSpoolFn) error {
	if v, ok := h.sessions.Load(c.ConnectionID); ok && h.cache != nil {
		if n := h.plan(ctx, v.(*Session), query); n != nil {
			return h.ComExecuteBound(ctx, c, query, n, callback)
		}
	}
	return h.Handler.ComQuery(ctx, c, query, callback)
}

// plan returns the cached plan for query, or nil. It also does the
// per-statement bookkeeping the engine does for a query it plans.
func (h *cachedHandler) plan(ctx context.Context, s *Session, query string) sql.Node {
	sctx := sql.NewContext(ctx, sql.WithSession(s))
	if n, _, err := h.cache.PlanTokens(sctx, query); err == nil && n != nil {
		starting(sctx, n)
		return n
	}
	stmt, _, rest, err := h.cache.Parser.ParseWithOptions(sctx, query, ';', false, sql.LoadSqlMode(sctx).ParserOptions())
	if err != nil || rest != "" {
		return nil
	}
	n, _, err := h.cache.Plan(sctx, query, stmt)
	if err != nil || n == nil {
		return nil
	}
	starting(sctx, n)
	return n
}

// ComStmtExecute runs a prepared statement as a bound plan when the cache
// covers it.
func (h *cachedHandler) ComStmtExecute(ctx context.Context, c *mysql.Conn, prepare *mysql.PrepareData, callback func(*sqltypes.Result) error) error {
	if v, ok := h.sessions.Load(c.ConnectionID); ok && h.cache != nil {
		if n := h.planBound(ctx, v.(*Session), prepare); n != nil {
			return h.ComExecuteBound(ctx, c, prepare.PrepareStmt, n, func(r *sqltypes.Result, _ bool) error { return callback(r) })
		}
	}
	return h.Handler.ComStmtExecute(ctx, c, prepare, callback)
}

func (h *cachedHandler) planBound(ctx context.Context, s *Session, prepare *mysql.PrepareData) sql.Node {
	bindings := make(map[string]ast.Expr, len(prepare.BindVars))
	for name, bv := range prepare.BindVars {
		v, err := sqltypes.BindVariableToValue(bv)
		if err != nil {
			return nil
		}
		if bindings[name], err = ast.ExprFromValue(v); err != nil {
			return nil
		}
	}
	sctx := sql.NewContext(ctx, sql.WithSession(s))
	n, _, err := h.cache.PlanBound(sctx, prepare.PrepareStmt, bindings)
	if err != nil || n == nil {
		return nil
	}
	starting(sctx, n)
	return n
}

func (h *cachedHandler) ConnectionClosed(c *mysql.Conn) {
	h.sessions.Delete(c.ConnectionID)
	h.Handler.ConnectionClosed(c)
}
