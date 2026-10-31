// The serializability checker. A Recorder logs what every transaction
// observed and wrote; Replay reruns the committed transactions one at a
// time in their claimed serialization order against an independent model
// and fails on the first divergence. It is a port of the Go package check:
// every hash, yield and core call happens in the same order, so the
// simulator's traces match the Go one's.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "db.h"

namespace celltnx::check {

// Kind classifies a logged transaction.
enum class Kind : uint8_t {
  kWriter,    // committed at Ts = commitTs
  kReader,    // wrote nothing and committed at Ts = readTs
  kRejected,  // failed a CHECK or overflowed while merging after commit Ts
};

const char *KindName(Kind k);

// OpKind is a logged operation.
enum class OpKind : uint8_t { kGet, kSet, kAdd, kInsert, kDelete, kScan, kSavepoint, kRollback, kIndexScan };

const char *OpKindName(OpKind k);

// Result is a write's outcome.
enum class Result : uint8_t { kOK, kNotFound, kDuplicate };

// ScanRow is a row a scan visited. Vals holds the predicate columns, plus
// the projection columns when the row matched.
struct ScanRow {
  std::string PK;
  bool Match = false;
  OwnedRow Vals;
};

// Op is one operation and what it returned.
struct Op {
  OpKind Kind = OpKind::kGet;
  int Table = 0;
  int Index = 0;  // index scans
  std::string PK;
  ColMask Cols = 0;  // get and set columns; scan predicate columns
  ColMask Proj = 0;  // scan projection columns
  int Col = 0;       // add column
  int64_t Delta = 0;
  OwnedRow Vals;  // get result; set and insert arguments
  bool Found = false;  // get result
  Result Res = Result::kOK;  // write result
  std::string Lo, Hi;  // scan range [Lo, Hi); Hi "" is unbounded
  std::vector<ScanRow> Rows;
  int SP = 0;
};

// Log is one finished transaction. Acked reports whether its commit was
// acknowledged to the client, which promises durability.
struct Log {
  check::Kind Kind = check::Kind::kWriter;
  uint64_t Ts = 0;
  uint64_t ReadTs = 0;
  int Client = 0;
  std::vector<Op> Ops;
  bool Acked = false;
};

class Tx;

// Recorder collects logs from concurrent clients and hashes every
// operation's outcome into a trace for determinism checks.
class Recorder {
 public:
  Recorder();
  Recorder(const Recorder &) = delete;
  Recorder &operator=(const Recorder &) = delete;

  // Logs returns a copy of the logs recorded so far.
  std::vector<Log> Logs();
  // Trace returns the hash of every recorded outcome.
  uint64_t Trace();
  // SetLogs replaces the recorded logs, so a recovered process can
  // continue a history.
  void SetLogs(std::vector<Log> logs);
  // Truncate drops the logs a crash erased: everything serialized after
  // last, the recovered commit timestamp. With durable set, it fails if any
  // dropped transaction was acknowledged, leaving the logs unchanged.
  Status Truncate(uint64_t last, bool durable);

  // Begin starts a recorded transaction. yield, which may be empty, runs
  // before Begin and before every operation, so the simulator can
  // interleave clients there.
  std::unique_ptr<Tx> Begin(DB &db, int client, std::function<void()> yield);

  // off disables logging and tracing, for benchmarks. Set it before use.
  bool off = false;
  // on_add and on_ack, when set, observe each log as it is added and
  // acknowledged, under the recorder's lock.
  std::function<void(int i, const Log &l)> on_add;
  std::function<void(int i)> on_ack;
  // refresh, when set, runs before every operation and before Commit; when
  // it returns true the transaction refreshes its snapshot there. A failed
  // refresh leaves the transaction as it was, and Commit then reports the
  // conflict.
  std::function<bool()> refresh;

 private:
  friend class Tx;
  void Hash(std::initializer_list<uint64_t> words);
  int Add(Log l);
  void Ack(int i);

  std::mutex mu_;
  std::vector<Log> logs_;
  uint64_t trace_;
};

// Tx wraps a transaction, logging each operation. Every operation but
// Savepoint, RollbackTo and Abort calls yield first. Rows and keys it
// returns stay valid until the transaction ends, as the core's do.
class Tx {
 public:
  Tx(const Tx &) = delete;
  Tx &operator=(const Tx &) = delete;

  // txn returns the underlying transaction.
  Txn &txn() { return *t_; }

  // Get is a tracked read; see Txn::Get.
  Status Get(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found);
  // GetUntracked is a snapshot read the checker ignores, since its values
  // carry no serializability guarantee.
  Status GetUntracked(int tbl, std::string_view pk, ColMask cols, std::vector<Value> *row, bool *found);
  // Scan is a tracked range read; see Txn::Scan. match may be empty.
  Status Scan(int tbl, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj, const Match &match,
              std::vector<Rec> *out);
  // IndexScan is a tracked secondary-index range read; see Txn::IndexScan.
  Status IndexScan(int tbl, int idx, std::string_view lo, std::string_view hi, ColMask pred, ColMask proj,
                   const Match &match, std::vector<Rec> *out);
  // Set assigns columns; see Txn::Set.
  Status Set(int tbl, std::string_view pk, ColMask cols, Row vals);
  // Add adds a delta; see Txn::Add.
  Status Add(int tbl, std::string_view pk, int col, int64_t delta);
  // Insert creates a row; see Txn::Insert.
  Status Insert(int tbl, std::string_view pk, Row row);
  // Delete removes a row; see Txn::Delete.
  Status Delete(int tbl, std::string_view pk);
  // Savepoint marks the write state; see Txn::Savepoint.
  int Savepoint();
  // RollbackTo discards writes after a savepoint; see Txn::RollbackTo.
  void RollbackTo(int sp);
  // Commit commits and logs the transaction if it committed or was
  // rejected by a constraint. The log is written when the commit installs,
  // before the durability wait, so a crash cannot hide a commit from the
  // checker.
  Status Commit();
  // Abort abandons the transaction.
  void Abort();

 private:
  friend class Recorder;
  Tx(std::unique_ptr<Txn> t, Recorder *rec, std::function<void()> yield, int client)
      : t_(std::move(t)), rec_(rec), yield_(std::move(yield)), client_(client) {}

  void Yield() {
    if (yield_) yield_();
  }
  // Before runs ahead of each operation: the yield, then any refresh.
  void Before();
  void Record(Op op);
  Status ScanImpl(OpKind kind, int tbl, int idx, std::string_view lo, std::string_view hi, ColMask pred,
                  ColMask proj, const Match &match, std::vector<Rec> *out);

  std::unique_ptr<Txn> t_;
  Status doomed_;  // a failed refresh, which Commit reports
  Recorder *rec_;
  std::function<void()> yield_;
  int client_;
  std::vector<Op> ops_;
};

}  // namespace celltnx::check
