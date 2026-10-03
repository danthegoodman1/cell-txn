package sim

import (
	"fmt"
	"testing"
)

// TestSeeds runs a fixed seed range and reruns a sample to check that each
// seed reproduces exactly.
func TestSeeds(t *testing.T) {
	n := uint64(1000)
	if testing.Short() {
		n = 100
	}
	for seed := range n {
		r := Run(seed, Config{Mode: -1, Durable: -1})
		if r.Err != nil {
			t.Fatalf("seed %d (%s): %v", seed, r.Params, r.Err)
		}
		if seed%10 == 0 {
			r2 := Run(seed, Config{Mode: -1, Durable: -1})
			if r2.Trace != r.Trace || r2.Steps != r.Steps || fmt.Sprint(r2.Err) != fmt.Sprint(r.Err) {
				t.Fatalf("seed %d is nondeterministic: trace %#x/%#x steps %d/%d", seed, r.Trace, r2.Trace, r.Steps, r2.Steps)
			}
		}
	}
}

// TestSQLSeeds runs SQL seeds through the end-to-end oracle.
func TestSQLSeeds(t *testing.T) {
	n := uint64(300)
	if testing.Short() {
		n = 30
	}
	for seed := range n {
		if r := RunSQL(seed, Config{Mode: -1}); r.Err != nil {
			t.Fatalf("seed %d (%s): %v", seed, r.Params, r.Err)
		}
	}
}
