package sim

import (
	"go/ast"
	"go/parser"
	"go/token"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// TestDeterminismRules rejects goroutines, wall-clock time and the global
// math/rand source in the packages the simulator runs.
func TestDeterminismRules(t *testing.T) {
	banned := map[string]bool{"time": true, "math/rand": true, "os": true}
	for _, dir := range []string{"../txn", "../check", "../workload"} {
		files, _ := filepath.Glob(filepath.Join(dir, "*.go"))
		for _, path := range files {
			if strings.HasSuffix(path, "_test.go") {
				continue
			}
			f, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
			if err != nil {
				t.Fatal(err)
			}
			for _, imp := range f.Imports {
				if p, _ := strconv.Unquote(imp.Path.Value); banned[p] {
					t.Errorf("%s imports %s", path, p)
				}
			}
			ast.Inspect(f, func(n ast.Node) bool {
				if _, ok := n.(*ast.GoStmt); ok {
					t.Errorf("%s starts a goroutine", path)
				}
				return true
			})
		}
	}
}
