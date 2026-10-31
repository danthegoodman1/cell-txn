// Transaction mixes as straight-line code against a Client. The simulator
// runs each client as a coroutine; the bench and the stress tests run each
// on its own thread. It is a port of the Go package workload: every random
// draw, yield and core call happens in the same order, so seeds replay the
// Go simulator's runs exactly.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "db.h"
#include "rand.h"
#include "record.h"

namespace celltnx::workload {

// Env supplies scheduling and time. Ticks are simulated steps in the
// simulator and microseconds elsewhere.
class Env {
 public:
  virtual ~Env() = default;
  virtual void Yield() = 0;
  virtual void Sleep(int64_t ticks) = 0;
  virtual int64_t Now() = 0;
  virtual bool Stopped() = 0;
};

struct Client;

// Workload is a transaction mix. One workload serves every client at once,
// so Load and Step must not change it.
class Workload {
 public:
  virtual ~Workload() = default;
  virtual std::string Name() = 0;
  virtual std::vector<Schema> Schemas() = 0;
  // Load inserts the initial data.
  virtual Status Load(Client &c) = 0;
  // Step runs one logical transaction, retrying conflicts. It fails only
  // for an unexpected failure or a violated invariant.
  virtual Status Step(Client &c) = 0;
  // Detail is what Go's fmt.Sprintf("%+v", w) prints for the workload.
  virtual std::string Detail() = 0;
};

// Stats counts outcomes.
struct Stats {
  int64_t Commits = 0;
  int64_t Rejected = 0;    // CHECK or overflow at commit
  int64_t UserAborts = 0;  // the transaction chose to abort
  int64_t Conflicts = 0;   // retried after a conflict
  int64_t RowConf = 0;
  int64_t BucketConf = 0;

  // Add accumulates s2 into this.
  void Add(const Stats &s2);
};

// Client is one session issuing transactions. It owns its random stream;
// DB, Rec and Env belong to the caller and must outlive it.
struct Client {
  int ID = 0;
  celltnx::DB *DB = nullptr;
  check::Recorder *Rec = nullptr;
  celltnx::Rand Rand{0, 0};
  workload::Env *Env = nullptr;
  int Quota = 0;  // logical transactions to run; 0 runs until Env->Stopped()
  int Done = 0;   // logical transactions finished
  workload::Stats Stats;
  // OnDone, when set, observes each logical transaction.
  std::function<void(int64_t latency, int retries, const Status &err)> OnDone;
  // seq is Uniq's counter.
  int64_t seq = 0;

  // Uniq returns a value no other client or call produces.
  int64_t Uniq();
  // Do runs fn in a transaction, committing if fn succeeds and retrying
  // conflicts with randomized exponential backoff.
  Status Do(const std::function<Status(check::Tx &)> &fn);
};

// Run executes the workload until the quota is met or the env stops. A
// failure keeps the step's code and fields; a kInternal one gains a
// "client N: " prefix.
Status Run(Workload &w, Client &c);

// ErrAbort is returned by a transaction body that chooses to abort: a
// kInternal status that IsAbort recognizes.
Status ErrAbort();
bool IsAbort(const Status &st);
// Expect maps the normal outcomes of Do (success, a constraint rejection,
// a chosen abort) to success.
Status Expect(Status st);
// Absent maps the normal outcomes of a write on a possibly missing row
// (not found, duplicate key) to success.
Status Absent(Status st);
// Internal returns a kInternal status carrying msg.
Status Internal(std::string msg);

// AsInt returns v's int64, or 0 for any other type.
inline int64_t AsInt(const Value &v) { return v.type == Type::kInt ? v.i : 0; }

// GoInts formats a sequence as Go's %v does: [a b c].
template <class C>
std::string GoInts(const C &xs) {
  std::string s = "[";
  bool first = true;
  for (auto x : xs) {
    if (!first) s += ' ';
    first = false;
    s += std::to_string(x);
  }
  return s + "]";
}

}  // namespace celltnx::workload
