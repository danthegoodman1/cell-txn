package sqlgms

import (
	"context"
	"os"
	"testing"

	"github.com/dolthub/go-mysql-server/enginetest"
	"github.com/dolthub/go-mysql-server/enginetest/scriptgen/setup"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/information_schema"

	"cell-tnx/txn"
)

// harness runs go-mysql-server's engine tests against cell-tnx. Each
// engine gets a fresh core DB in cell+delta mode, the most permissive
// for concurrency.
type harness struct {
	setup   [][]setup.SetupScript
	pro     *Provider
	session *Session
}

var (
	_ enginetest.Harness             = (*harness)(nil)
	_ enginetest.TransactionHarness  = (*harness)(nil)
	_ enginetest.IndexHarness        = (*harness)(nil)
	_ enginetest.ForeignKeyHarness   = (*harness)(nil)
	_ enginetest.KeylessTableHarness = (*harness)(nil)
)

func (h *harness) Setup(scripts ...[]setup.SetupScript) { h.setup = scripts }

func (h *harness) NewContext() *sql.Context {
	if h.session == nil {
		h.session = NewSession(enginetest.NewBaseSession(), h.pro)
	}
	return sql.NewContext(context.Background(), sql.WithSession(h.session))
}

func (h *harness) NewSession() *sql.Context {
	h.session = nil
	return h.NewContext()
}

func (h *harness) NewEngine(t *testing.T) (enginetest.QueryEngine, error) {
	h.pro = NewProvider(txn.Open(txn.Options{Mode: txn.CellDelta, BucketBits: 4}))
	h.session = nil
	e := NewEngine(h.pro)
	e.Analyzer.Catalog.InfoSchema = information_schema.NewInformationSchemaDatabase()
	e.Analyzer.Runner = e
	var flat []setup.SetupScript
	for _, s := range h.setup {
		flat = append(flat, s...)
	}
	if len(flat) == 0 {
		flat = setup.MydbData
	}
	e, err := enginetest.RunSetupScripts(h.NewContext(), e, flat, true)
	if err != nil {
		return nil, err
	}
	// Queries go through the plan cache, which re-analyzes every hit and
	// fails the query if the cached plan differs.
	pc := NewPlanCache(e, h.pro)
	pc.Verify = true
	return pc, nil
}

func (h *harness) SupportsNativeIndexCreation() bool { return true }
func (h *harness) SupportsForeignKeys() bool         { return false }
func (h *harness) SupportsKeylessTables() bool       { return true }

// TestEngine runs go-mysql-server's DML, DDL, CHECK and transaction
// suites. It is opt-in (CELLTNX_ENGINETEST=1) because some cases exercise
// features outside this prototype; bench/enginetest.sh records which.
func TestEngine(t *testing.T) {
	if os.Getenv("CELLTNX_ENGINETEST") == "" {
		t.Skip("set CELLTNX_ENGINETEST=1")
	}
	for name, suite := range map[string]func(*testing.T, enginetest.Harness){
		"InsertInto":         enginetest.TestInsertInto,
		"InsertIntoErrors":   enginetest.TestInsertIntoErrors,
		"ReplaceInto":        enginetest.TestReplaceInto,
		"Update":             enginetest.TestUpdate,
		"UpdateErrors":       enginetest.TestUpdateErrors,
		"Delete":             enginetest.TestDelete,
		"DeleteErrors":       enginetest.TestDeleteErrors,
		"Truncate":           enginetest.TestTruncate,
		"CreateTable":        enginetest.TestCreateTable,
		"ChecksOnInsert":     enginetest.TestChecksOnInsert,
		"ChecksOnUpdate":     enginetest.TestChecksOnUpdate,
		"TransactionScripts": enginetest.TestTransactionScripts,
		"Scripts":            enginetest.TestScripts,
		"Queries":            enginetest.TestQueries,
	} {
		t.Run(name, func(t *testing.T) { suite(t, &harness{}) })
	}
}
