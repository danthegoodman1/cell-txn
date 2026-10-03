package sqlgms

import "github.com/dolthub/go-mysql-server/sql"

// noStats keeps no table statistics, so the planner uses its defaults.
// go-mysql-server's join planner divides by a statistic's distinct count,
// which ANALYZE TABLE can leave at zero.
type noStats struct{}

var _ sql.StatsProvider = noStats{}

func (noStats) GetTableStats(*sql.Context, string, sql.Table) ([]sql.Statistic, error) {
	return nil, nil
}
func (noStats) AnalyzeTable(*sql.Context, sql.Table, string) error { return nil }
func (noStats) SetStats(*sql.Context, sql.Statistic) error         { return nil }
func (noStats) GetStats(*sql.Context, sql.StatQualifier, []string) (sql.Statistic, bool) {
	return nil, false
}
func (noStats) DropStats(*sql.Context, sql.StatQualifier, []string) error  { return nil }
func (noStats) DropDbStats(*sql.Context, string, bool) error               { return nil }
func (noStats) RowCount(*sql.Context, string, sql.Table) (uint64, error)   { return 0, nil }
func (noStats) DataLength(*sql.Context, string, sql.Table) (uint64, error) { return 0, nil }
