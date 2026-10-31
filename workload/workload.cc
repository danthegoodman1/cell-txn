#include "workload.h"

#include <algorithm>
#include <utility>

namespace celltnx::workload {

namespace {
constexpr const char *kAbortDetail = "workload: transaction aborted by client";
}

void Stats::Add(const Stats &s2) {
  Commits += s2.Commits;
  Rejected += s2.Rejected;
  UserAborts += s2.UserAborts;
  Conflicts += s2.Conflicts;
  RowConf += s2.RowConf;
  BucketConf += s2.BucketConf;
}

Status Internal(std::string msg) { return Error{.code = Code::kInternal, .detail = std::move(msg)}; }

Status ErrAbort() { return Internal(kAbortDetail); }

bool IsAbort(const Status &st) { return st.code() == Code::kInternal && st.error().detail == kAbortDetail; }

Status Expect(Status st) {
  if (st.ok() || st.code() == Code::kConstraint || IsAbort(st)) return {};
  return st;
}

Status Absent(Status st) {
  if (st.code() == Code::kNotFound || st.code() == Code::kDuplicateKey) return {};
  return st;
}

int64_t Client::Uniq() {
  seq++;
  return int64_t(uint64_t(int64_t(ID) + 1) << 32) | seq;
}

Status Client::Do(const std::function<Status(check::Tx &)> &fn) {
  int64_t start = Env->Now();
  int64_t backoff = 4;
  for (int retries = 0;; retries++) {
    std::unique_ptr<check::Tx> tx = Rec->Begin(*DB, ID, [env = Env] { env->Yield(); });
    Status err = fn(*tx);
    if (err.ok())
      err = tx->Commit();
    else
      tx->Abort();
    if (err.code() == Code::kConflict) {
      Stats.Conflicts++;
      if (err.error().bucket)
        Stats.BucketConf++;
      else
        Stats.RowConf++;
      Env->Sleep(1 + Rand.Int64N(backoff));
      backoff = std::min<int64_t>(backoff * 2, 1024);
      continue;
    }
    if (err.ok())
      Stats.Commits++;
    else if (err.code() == Code::kConstraint)
      Stats.Rejected++;
    else if (IsAbort(err))
      Stats.UserAborts++;
    if (OnDone) OnDone(Env->Now() - start, retries, err);
    return err;
  }
}

Status Run(Workload &w, Client &c) {
  while (c.Quota == 0 || c.Done < c.Quota) {
    if (c.Env->Stopped()) return {};
    if (Status st = w.Step(c); !st.ok()) {
      if (st.code() != Code::kInternal) return st;
      return Internal("client " + std::to_string(c.ID) + ": " + st.error().detail);
    }
    c.Done++;
  }
  return {};
}

}  // namespace celltnx::workload
