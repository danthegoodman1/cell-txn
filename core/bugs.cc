#include "bugs.h"

#include <utility>

namespace celltnx {

#ifdef CELLTNX_SIMBUGS
bool bugSkipBuckets, bugSetNoExists, bugGCEager, bugCheckSkipDelta, bugNoAbsentRead, bugDeltaReadUntracked,
    bugUniqueNoRead, bugIndexNoRange, bugAckBeforeSync, bugReadOnlyNoWait, bugAckJoined, bugSeekReload,
    bugPublishEarly, bugLinkBeforeInit, bugRefreshNoValidate;

static const std::pair<const char *, bool *> kFlags[] = {
    {"skip-buckets", &bugSkipBuckets},
    {"set-no-exists", &bugSetNoExists},
    {"gc-eager", &bugGCEager},
    {"check-skip-delta", &bugCheckSkipDelta},
    {"no-absent-read", &bugNoAbsentRead},
    {"delta-read-untracked", &bugDeltaReadUntracked},
    {"unique-no-read", &bugUniqueNoRead},
    {"index-no-range", &bugIndexNoRange},
    {"ack-before-sync", &bugAckBeforeSync},
    {"read-only-no-wait", &bugReadOnlyNoWait},
    {"ack-joined", &bugAckJoined},
    {"seek-reload", &bugSeekReload},
    {"publish-early", &bugPublishEarly},
    {"link-before-init", &bugLinkBeforeInit},
    {"refresh-no-validate", &bugRefreshNoValidate},
};

const std::vector<std::string> &Bugs() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (auto &f : kFlags) v.emplace_back(f.first);
    return v;
  }();
  return names;
}

bool SetBug(std::string_view name) {
  for (auto &f : kFlags)
    if (name == f.first) {
      *f.second = true;
      return true;
    }
  return false;
}

void ResetBugs() {
  for (auto &f : kFlags) *f.second = false;
}
#else
const std::vector<std::string> &Bugs() {
  static const std::vector<std::string> none;
  return none;
}

bool SetBug(std::string_view) { return false; }

void ResetBugs() {}
#endif

}  // namespace celltnx
