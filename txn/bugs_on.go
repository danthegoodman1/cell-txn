//go:build simbugs

package txn

import "fmt"

// Deliberate bugs for simulator self-tests. Each must be caught by the
// checker or a workload invariant.
var (
	bugSkipBuckets        bool // validation ignores scanned buckets
	bugSetNoExists        bool // Set records no read of Exists
	bugGCEager            bool // GC ignores active snapshots
	bugCheckSkipDelta     bool // merge skips CHECKs on delta-only rows
	bugNoAbsentRead       bool // a Get of a missing row records nothing
	bugDeltaReadUntracked bool // reading a delta'd column records no read
	bugUniqueNoRead       bool // a unique-key claim records no read of the entry
	bugIndexNoRange       bool // an index scan records no buckets
	bugAckBeforeSync      bool // commits are acknowledged before they sync
	bugReadOnlyNoWait     bool // read-only commits skip the durability wait
)

var bugFlags = map[string]*bool{
	"skip-buckets":         &bugSkipBuckets,
	"set-no-exists":        &bugSetNoExists,
	"gc-eager":             &bugGCEager,
	"check-skip-delta":     &bugCheckSkipDelta,
	"no-absent-read":       &bugNoAbsentRead,
	"delta-read-untracked": &bugDeltaReadUntracked,
	"unique-no-read":       &bugUniqueNoRead,
	"index-no-range":       &bugIndexNoRange,
	"ack-before-sync":      &bugAckBeforeSync,
	"read-only-no-wait":    &bugReadOnlyNoWait,
}

// Bugs lists the deliberate bugs SetBug accepts.
var Bugs = []string{"skip-buckets", "set-no-exists", "gc-eager", "check-skip-delta", "no-absent-read", "delta-read-untracked", "unique-no-read", "index-no-range", "ack-before-sync", "read-only-no-wait"}

// SetBug enables a deliberate bug for the whole process.
func SetBug(name string) error {
	f, ok := bugFlags[name]
	if !ok {
		return fmt.Errorf("txn: unknown bug %q", name)
	}
	*f = true
	return nil
}

// ResetBugs disables every deliberate bug.
func ResetBugs() {
	for _, f := range bugFlags {
		*f = false
	}
}
