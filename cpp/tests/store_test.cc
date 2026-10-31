// The lionturtle store: its row encoding, durable table IDs, and recovery
// after kill -9, checked against the serial replay of the history.
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "replay.h"
#include "store.h"
#include "tpcc.h"
#include "workload.h"

namespace celltnx {
namespace {

using lionturtle::Store;

#define MUST(expr)                          \
  do {                                      \
    ::celltnx::Status st_ = (expr);         \
    ASSERT_TRUE(st_.ok()) << st_.Message(); \
  } while (0)

std::string TempDir(const char *name) {
  std::string d = std::filesystem::temp_directory_path() / (std::string("celltnx-") + name + "-XXXXXX");
  if (mkdtemp(d.data()) == nullptr) abort();
  return d;
}

TEST(LionturtleStore, RowEncoding) {
  std::string big(300, 'z');
  std::vector<Value> row = {Value(),          Value::Int(-5),  Value::Uint(~uint64_t(0)), Value::Float(-0.25),
                            Value::Str(""),   Value::Str(big), Value::Str(std::string_view("a\0b", 3))};
  std::string b;
  lionturtle::EncodeRow(row, &b);
  std::vector<Value> got;
  ASSERT_TRUE(lionturtle::DecodeRow(b, &got));
  ASSERT_EQ(got.size(), row.size());
  for (size_t i = 0; i < row.size(); i++) EXPECT_TRUE(got[i] == row[i]) << i;
  for (size_t n = 0; n < b.size(); n++) EXPECT_FALSE(lionturtle::DecodeRow(std::string_view(b).substr(0, n), &got)) << n;
  EXPECT_FALSE(lionturtle::DecodeRow(b + "x", &got));
}

// Rows written through a DB survive reopening under their durable table
// IDs, and a dropped or unbound table keeps nothing.
TEST(LionturtleStore, DurableTables) {
  std::string dir = TempDir("tables");
  Schema s{.name = "t", .cols = {"v"}};
  std::vector<Value> row = {Value::Str("one")};
  uint64_t a = 7, b = 300, c = 9;
  {
    std::unique_ptr<Store> st;
    MUST(Store::Open(dir, &st));
    DB db(Options{.store = st.get()}, {s, s, s});
    st->Bind(0, a);
    st->Bind(1, b);
    std::thread w([&] { (void)db.RunWriter(); });
    auto tx = db.Begin();
    MUST(tx->Insert(0, "k1", row));
    MUST(tx->Insert(1, "k2", row));
    MUST(tx->Insert(2, "k3", row));  // unbound: memory only
    MUST(tx->Commit());
    db.Close();
    w.join();
    EXPECT_EQ(st->Last(), db.LastCommitTs());
  }
  std::unique_ptr<Store> st;
  MUST(Store::Open(dir, &st));
  EXPECT_EQ(st->Last(), 1u);
  std::vector<std::string> keys;
  auto collect = [&](std::string_view key, Row r) {
    keys.emplace_back(key);
    EXPECT_EQ(r[0].str(), "one");
  };
  MUST(st->Load(a, collect));
  MUST(st->Load(b, collect));
  MUST(st->Load(c, collect));
  EXPECT_EQ(keys, (std::vector<std::string>{"k1", "k2"}));
  MUST(st->Drop(a));
  keys.clear();
  MUST(st->Load(a, collect));
  MUST(st->Load(b, collect));
  EXPECT_EQ(keys, (std::vector<std::string>{"k2"}));
  st.reset();
  std::filesystem::remove_all(dir);
}

// --- kill -9 ---

workload::TPCC CrashWorkload() { return workload::TPCC(2, 4, 30, 200, 10); }

struct ThreadEnv : workload::Env {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  void Yield() override {}
  void Sleep(int64_t ticks) override { std::this_thread::sleep_for(std::chrono::microseconds(ticks)); }
  int64_t Now() override {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  }
  bool Stopped() override { return false; }
};

// The child streams its history as records: an installed transaction's log
// or an acknowledgment, each field a little-endian u64 or a length-prefixed
// string, with rows in the store's encoding.
class Writer {
 public:
  explicit Writer(int fd) : fd_(fd) {}
  void U(uint64_t x) { buf_.append(reinterpret_cast<const char *>(&x), 8); }
  void S(std::string_view s) {
    U(s.size());
    buf_.append(s);
  }
  void R(Row r) {
    std::string b;
    lionturtle::EncodeRow(r, &b);
    S(b);
  }
  void Flush() {
    for (size_t off = 0; off < buf_.size();) {
      ssize_t n = write(fd_, buf_.data() + off, buf_.size() - off);
      if (n <= 0) _exit(3);
      off += size_t(n);
    }
    buf_.clear();
  }

 private:
  int fd_;
  std::string buf_;
};

class Reader {
 public:
  explicit Reader(FILE *f) : f_(f) {}
  bool U(uint64_t *x) { return fread(x, 8, 1, f_) == 1; }
  bool S(std::string *s) {
    uint64_t n;
    if (!U(&n)) return false;
    s->resize(n);
    return n == 0 || fread(s->data(), n, 1, f_) == 1;
  }
  bool R(OwnedRow *r) {
    std::string b;
    std::vector<Value> vals;
    if (!S(&b) || !lionturtle::DecodeRow(b, &vals)) return false;
    r->Assign(vals);
    return true;
  }

 private:
  FILE *f_;
};

void WriteLog(Writer &w, int i, const check::Log &l) {
  w.U(0);
  w.U(uint64_t(i));
  w.U(uint64_t(l.Kind));
  w.U(l.Ts);
  w.U(l.ReadTs);
  w.U(uint64_t(l.Client));
  w.U(l.Acked);
  w.U(l.Ops.size());
  for (const check::Op &o : l.Ops) {
    for (uint64_t x : {uint64_t(o.Kind), uint64_t(o.Table), uint64_t(o.Index), o.Cols, o.Proj, uint64_t(o.Col),
                       uint64_t(o.Delta), uint64_t(o.Found), uint64_t(o.Res), uint64_t(int64_t(o.SP))})
      w.U(x);
    w.S(o.PK);
    w.R(o.Vals.row());
    w.S(o.Lo);
    w.S(o.Hi);
    w.U(o.Rows.size());
    for (const check::ScanRow &r : o.Rows) {
      w.S(r.PK);
      w.U(r.Match);
      w.R(r.Vals.row());
    }
  }
  w.Flush();
}

// ReadRecord reads one record into history; base is where this round's
// indexes start.
bool ReadRecord(Reader &r, size_t base, std::vector<check::Log> *history) {
  uint64_t tag, i;
  if (!r.U(&tag) || !r.U(&i)) return false;
  if (tag == 1) {
    (*history)[base + i].Acked = true;
    return true;
  }
  check::Log l;
  uint64_t kind, client, acked, nops;
  if (!r.U(&kind) || !r.U(&l.Ts) || !r.U(&l.ReadTs) || !r.U(&client) || !r.U(&acked) || !r.U(&nops)) return false;
  l.Kind = check::Kind(kind);
  l.Client = int(client);
  l.Acked = acked;
  for (uint64_t j = 0; j < nops; j++) {
    check::Op o;
    uint64_t f[10];
    for (uint64_t &x : f)
      if (!r.U(&x)) return false;
    o.Kind = check::OpKind(f[0]);
    o.Table = int(f[1]);
    o.Index = int(f[2]);
    o.Cols = f[3];
    o.Proj = f[4];
    o.Col = int(f[5]);
    o.Delta = int64_t(f[6]);
    o.Found = f[7];
    o.Res = check::Result(f[8]);
    o.SP = int(int64_t(f[9]));
    uint64_t nrows;
    if (!r.S(&o.PK) || !r.R(&o.Vals) || !r.S(&o.Lo) || !r.S(&o.Hi) || !r.U(&nrows)) return false;
    for (uint64_t k = 0; k < nrows; k++) {
      check::ScanRow sr;
      uint64_t match;
      if (!r.S(&sr.PK) || !r.U(&match) || !r.R(&sr.Vals)) return false;
      sr.Match = match;
      o.Rows.push_back(std::move(sr));
    }
    l.Ops.push_back(std::move(o));
  }
  if (history->size() != base + i) return false;
  history->push_back(std::move(l));
  return true;
}

// Bind gives table i of the workload the durable ID i+1.
void Bind(Store &st, size_t tables) {
  for (size_t i = 0; i < tables; i++) st.Bind(int(i), i + 1);
}

Status Recover(DB &db, Store &st, size_t tables) {
  return db.Recover(st.Last(), [&](const DB::Put &put) {
    for (size_t i = 0; i < tables; i++)
      if (Status s = st.Load(i + 1, [&](std::string_view k, Row r) { put(int(i), k, r); }); !s.ok()) return s;
    return Status();
  });
}

// RunChild recovers the store, loads TPC-C into an empty one, and runs
// eight clients until killed, streaming its history to fd.
[[noreturn]] void RunChild(const std::string &dir, int fd, int64_t delay_us) {
  auto fail = [](const Status &s) {
    fprintf(stderr, "child: %s\n", s.Message().c_str());
    _exit(2);
  };
  std::unique_ptr<Store> st;
  if (Status s = Store::Open(dir, &st); !s.ok()) fail(s);
  workload::TPCC w = CrashWorkload();
  std::vector<Schema> schemas = w.Schemas();
  Options o{.mode = Mode::kCellDelta, .bucket_bits = 2, .store = st.get()};
  if (delay_us > 0) o.commit_delay = [delay_us] { std::this_thread::sleep_for(std::chrono::microseconds(delay_us)); };
  DB db(std::move(o), schemas);
  bool fresh = st->Last() == 0;
  if (Status s = Recover(db, *st, schemas.size()); !s.ok()) fail(s);
  Bind(*st, schemas.size());
  std::thread([&] {
    if (Status s = db.RunWriter(); !s.ok()) fail(s);
  }).detach();
  Writer out(fd);
  check::Recorder rec;
  rec.on_add = [&](int i, const check::Log &l) { WriteLog(out, i, l); };
  rec.on_ack = [&](int i) {
    out.U(1);
    out.U(uint64_t(i));
    out.Flush();
  };
  ThreadEnv env;
  if (fresh) {
    workload::Client loader{.DB = &db, .Rec = &rec, .Rand = Rand(1, 1), .Env = &env};
    if (Status s = w.Load(loader); !s.ok()) fail(s);
  }
  uint64_t seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  std::vector<std::thread> clients;
  for (int i = 0; i < 8; i++)
    clients.emplace_back([&, i] {
      workload::Client c{.ID = i + 1, .DB = &db, .Rec = &rec, .Rand = Rand(seed, uint64_t(i)), .Env = &env};
      if (Status s = workload::Run(w, c); !s.ok()) fail(s);
    });
  for (auto &t : clients) t.join();
  _exit(1);
}

// Crash runs TPC-C in a child process, kills it with SIGKILL at a random
// moment, recovers the store, and checks that no acknowledged commit was
// lost and that the recovered state equals the serial replay of every
// surviving commit. It repeats on one store, odd rounds holding each sync
// open so batches gather commits.
TEST(LionturtleStore, Crash) {
  std::string dir = TempDir("crash");
  std::vector<Schema> schemas = CrashWorkload().Schemas();
  std::vector<check::Log> history;
  std::mt19937_64 r(std::random_device{}());
  for (int round = 0; round < 8; round++) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      close(fds[0]);
      RunChild(dir, fds[1], round % 2 ? 500 : 0);
    }
    close(fds[1]);
    size_t base = history.size();
    FILE *in = fdopen(fds[0], "r");
    bool ok = true;
    std::thread reader([&] {
      Reader rd(in);
      while (ReadRecord(rd, base, &history)) {
      }
      ok = feof(in);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300 + r() % 1200));
    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
    reader.join();
    fclose(in);
    ASSERT_TRUE(WIFSIGNALED(status)) << "round " << round << ": child exited with status " << WEXITSTATUS(status);
    ASSERT_TRUE(ok) << "round " << round << ": malformed history";

    std::unique_ptr<Store> st;
    MUST(Store::Open(dir, &st));
    uint64_t last = st->Last();
    check::Recorder rec;
    rec.SetLogs(std::move(history));
    MUST(rec.Truncate(last, true));
    history = rec.Logs();
    size_t acked = 0;
    for (const check::Log &l : history) acked += l.Acked;
    DB db(Options{}, schemas);
    MUST(Recover(db, *st, schemas.size()));
    MUST(db.CheckInvariants());
    MUST(check::Replay(schemas, history, last, [&](int t) { return db.Dump(t); }));
    printf("round %d: recovered through commit %lu; %zu commits logged, %zu acknowledged\n", round,
           (unsigned long)last, history.size(), acked);
  }
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace celltnx
