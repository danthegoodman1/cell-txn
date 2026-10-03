package sqlgms

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/server"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/vitess/go/mysql"

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
}

// Server is a running MySQL-protocol server.
type Server struct {
	*server.Server
	Engine   *sqle.Engine
	Provider *Provider
	store    *badger.Store
	catalog  *os.File
}

// Serve opens the store, recovers it when durable, and starts listening.
func Serve(cfg ServerConfig) (*Server, error) {
	opts := txn.Options{Mode: cfg.Mode, BucketBits: cfg.BucketBits, NoSync: cfg.NoSync}
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
	sb := func(ctx context.Context, conn *mysql.Conn, addr string) (sql.Session, error) {
		base, err := sql.BaseSessionFromConnection(ctx, conn, addr)
		if err != nil {
			return nil, err
		}
		return NewSession(base, s.Provider), nil
	}
	srv, err := server.NewServer(server.Config{Protocol: "tcp", Address: cfg.Addr}, s.Engine, sql.NewContext, sb, nil)
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
