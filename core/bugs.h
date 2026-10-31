// Deliberate bugs for simulator self-tests. They exist only in builds with
// CELLTNX_SIMBUGS; elsewhere each is a constant false the compiler removes.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace celltnx {

#ifdef CELLTNX_SIMBUGS
#define CELLTNX_BUG(name) extern bool name;
#else
#define CELLTNX_BUG(name) inline constexpr bool name = false;
#endif

CELLTNX_BUG(bugSkipBuckets)         // validation ignores scanned buckets
CELLTNX_BUG(bugSetNoExists)         // Set records no read of Exists
CELLTNX_BUG(bugGCEager)             // GC ignores active snapshots
CELLTNX_BUG(bugCheckSkipDelta)      // merge skips CHECKs on delta-only rows
CELLTNX_BUG(bugNoAbsentRead)        // a Get of a missing row records nothing
CELLTNX_BUG(bugDeltaReadUntracked)  // reading a delta'd column records no read
CELLTNX_BUG(bugUniqueNoRead)        // a unique-key claim records no read of the entry
CELLTNX_BUG(bugIndexNoRange)        // an index scan records no buckets
CELLTNX_BUG(bugAckBeforeSync)       // commits are acknowledged before they sync
CELLTNX_BUG(bugReadOnlyNoWait)      // read-only commits skip the durability wait
CELLTNX_BUG(bugAckJoined)           // a flush acknowledges commits queued during its sync
CELLTNX_BUG(bugSeekReload)          // a reader's seek reloads its result after comparing
CELLTNX_BUG(bugPublishEarly)        // a commit publishes its timestamp before installing
CELLTNX_BUG(bugLinkBeforeInit)      // a skiplist node is linked before its next pointers are set
CELLTNX_BUG(bugRefreshNoValidate)   // a snapshot refresh skips validating earlier reads

#undef CELLTNX_BUG

// Bugs lists the names SetBug accepts: empty without CELLTNX_SIMBUGS.
const std::vector<std::string> &Bugs();

// SetBug enables a deliberate bug for the whole process. It fails without
// CELLTNX_SIMBUGS or for an unknown name.
bool SetBug(std::string_view name);

// ResetBugs disables every deliberate bug.
void ResetBugs();

}  // namespace celltnx
