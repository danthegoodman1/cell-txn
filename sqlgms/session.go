package sqlgms

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/analyzer"

	"cell-tnx/txn"
)

// Transaction wraps a core transaction.
type Transaction struct {
	t        *txn.Txn
	readOnly bool
}

func (t *Transaction) String() string   { return fmt.Sprintf("txn@%d", t.t.ReadTs()) }
func (t *Transaction) IsReadOnly() bool { return t.readOnly }

// Txn returns the core transaction.
func (t *Transaction) Txn() *txn.Txn { return t.t }

// Session runs one client's statements in core transactions.
type Session struct {
	*sql.BaseSession
	p          *Provider
	savepoints []savepoint // in creation order
	// LastCommit describes the most recent COMMIT, successful or rejected
	// by a CHECK; the simulator's oracle reads it.
	LastCommit *CommitInfo
}

// CommitInfo is a transaction's serialization point: its commitTs if it
// wrote, its readTs if not, or the last commit its rejected merge saw.
type CommitInfo struct {
	Ts       uint64
	Wrote    bool
	Rejected bool
	Check    string // the CHECK a rejected merge failed
}

var (
	_ sql.TransactionSession    = (*Session)(nil)
	_ sql.LifecycleAwareSession = (*Session)(nil)
)

// NewSession returns a session over the provider's core DB.
func NewSession(base *sql.BaseSession, p *Provider) *Session {
	return &Session{BaseSession: base, p: p}
}

func (s *Session) StartTransaction(_ *sql.Context, ch sql.TransactionCharacteristic) (sql.Transaction, error) {
	s.savepoints = nil
	return &Transaction{t: s.p.core.Begin(), readOnly: ch == sql.ReadOnly}, nil
}

// CommitTransaction commits; on failure it also detaches the transaction,
// which the engine leaves attached.
func (s *Session) CommitTransaction(ctx *sql.Context, tx sql.Transaction) error {
	t := tx.(*Transaction).t
	err := t.Commit()
	var ke *txn.ConstraintError
	switch {
	case err == nil:
		s.LastCommit = &CommitInfo{Ts: t.SerialTs(), Wrote: t.Wrote()}
	case errors.As(err, &ke):
		s.LastCommit = &CommitInfo{Ts: ke.AfterTs, Rejected: true, Check: ke.Check}
	}
	if err != nil {
		ctx.SetTransaction(nil)
		ctx.SetIgnoreAutoCommit(false)
	}
	return MapError(err)
}

// MapError converts core errors into MySQL ones: conflicts become
// deadlocks (1213), which clients retry, and rejected merges become CHECK
// violations.
func MapError(err error) error {
	var ce *txn.ConflictError
	var ke *txn.ConstraintError
	switch {
	case err == nil:
		return nil
	case errors.As(err, &ce):
		return sql.ErrLockDeadlock.New(ce.Error())
	case errors.As(err, &ke):
		return sql.ErrCheckConstraintViolated.New(ke.Check)
	}
	return err
}

func (s *Session) Rollback(_ *sql.Context, tx sql.Transaction) error {
	tx.(*Transaction).t.Abort()
	return nil
}

type savepoint struct {
	name string
	sp   int
}

func (s *Session) findSavepoint(name string) int {
	for i, p := range s.savepoints {
		if strings.EqualFold(p.name, name) {
			return i
		}
	}
	return -1
}

// CreateSavepoint sets a savepoint, replacing any of the same name.
func (s *Session) CreateSavepoint(_ *sql.Context, tx sql.Transaction, name string) error {
	if i := s.findSavepoint(name); i >= 0 {
		s.savepoints = append(s.savepoints[:i], s.savepoints[i+1:]...)
	}
	s.savepoints = append(s.savepoints, savepoint{name, tx.(*Transaction).t.Savepoint()})
	return nil
}

// RollbackToSavepoint rolls back to a savepoint and, as in MySQL, deletes
// the savepoints set after it.
func (s *Session) RollbackToSavepoint(_ *sql.Context, tx sql.Transaction, name string) error {
	i := s.findSavepoint(name)
	if i < 0 {
		return sql.ErrSavepointDoesNotExist.New(name)
	}
	tx.(*Transaction).t.RollbackTo(s.savepoints[i].sp)
	s.savepoints = s.savepoints[:i+1]
	return nil
}

// ReleaseSavepoint deletes a savepoint and those set after it.
func (s *Session) ReleaseSavepoint(_ *sql.Context, _ sql.Transaction, name string) error {
	i := s.findSavepoint(name)
	if i < 0 {
		return sql.ErrSavepointDoesNotExist.New(name)
	}
	s.savepoints = s.savepoints[:i]
	return nil
}

func (s *Session) CommandBegin() error { return nil }

// CommandEnd aborts an autocommit transaction a failed statement left
// attached.
func (s *Session) CommandEnd() {
	tx, ok := s.GetTransaction().(*Transaction)
	if !ok || tx == nil || s.GetIgnoreAutoCommit() {
		return
	}
	ctx := sql.NewContext(context.Background(), sql.WithSession(s))
	if v, err := s.GetSessionVariable(ctx, "autocommit"); err == nil {
		if on, err := sql.ConvertToBool(ctx, v); err == nil && !on {
			return
		}
	}
	tx.t.Abort()
	s.SetTransaction(nil)
}

// SessionEnd aborts any open transaction.
func (s *Session) SessionEnd() {
	if tx, ok := s.GetTransaction().(*Transaction); ok && tx != nil {
		tx.t.Abort()
		s.SetTransaction(nil)
	}
}

// engineMu serializes engine construction: go-mysql-server initializes
// package-global status variables in sqle.New without synchronization.
var engineMu sync.Mutex

// NewEngine returns an engine over p that annotates every plan with its
// column access.
func NewEngine(p *Provider) *sqle.Engine {
	engineMu.Lock()
	defer engineMu.Unlock()
	e := sqle.New(analyzer.NewDefault(p), &sqle.Config{})
	e.Analyzer.ExecBuilder = NewBuilder()
	e.Analyzer.Catalog.StatsProvider = noStats{}
	return e
}

// NewDefaultEngine returns go-mysql-server's default engine over pro,
// constructed under the same lock as NewEngine.
func NewDefaultEngine(pro sql.DatabaseProvider) *sqle.Engine {
	engineMu.Lock()
	defer engineMu.Unlock()
	return sqle.NewDefault(pro)
}
