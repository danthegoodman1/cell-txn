//go:build simbugs

package sim

import (
	"testing"

	"cell-tnx/txn"
)

// TestSelfTest enables each deliberate bug with internal invariants off and
// requires the checker or a workload invariant to catch it.
func TestSelfTest(t *testing.T) {
	defer txn.ResetBugs()
	for _, bug := range txn.Bugs {
		txn.ResetBugs()
		if err := txn.SetBug(bug); err != nil {
			t.Fatal(err)
		}
		found := false
		for seed := range uint64(30000) {
			if r := Run(seed, Config{Mode: -1, Durable: -1, CommitDelay: -1, NoInvariants: true}); r.Err != nil {
				t.Logf("%s: found at seed %d: %.120s", bug, seed, r.Err)
				found = true
				break
			}
		}
		if !found {
			t.Errorf("%s: not found in 30000 seeds", bug)
		}
	}
}
