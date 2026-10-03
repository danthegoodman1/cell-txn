//go:build !simbugs

package txn

import "errors"

// Deliberate bugs for simulator self-tests exist only under the simbugs
// build tag. Here they are constants, so the compiler removes them.
const (
	bugSkipBuckets        = false
	bugSetNoExists        = false
	bugGCEager            = false
	bugCheckSkipDelta     = false
	bugNoAbsentRead       = false
	bugDeltaReadUntracked = false
	bugUniqueNoRead       = false
	bugIndexNoRange       = false
	bugAckBeforeSync      = false
	bugReadOnlyNoWait     = false
	bugAckJoined          = false
)

// Bugs lists the deliberate bugs SetBug accepts.
var Bugs []string

// SetBug enables a deliberate bug. It requires the simbugs build tag.
func SetBug(name string) error {
	return errors.New("txn: deliberate bugs require -tags simbugs")
}

// ResetBugs disables every deliberate bug.
func ResetBugs() {}
