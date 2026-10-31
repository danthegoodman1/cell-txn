// The workloads match Go's draw for draw, and whole runs match Go's
// recorder traces. The vectors come from the generators in testdata:
//   go run ./cpp/tests/testdata/mathvec > cpp/tests/testdata/math_vectors.txt
//   GODEBUG=cpu.fma=off go run ./cpp/tests/testdata/mathvec > cpp/tests/testdata/math_vectors_nofma.txt
//   go run ./cpp/tests/testdata/wlvec > cpp/tests/testdata/workload_vectors.txt
//   go run ./cpp/tests/testdata/tracevec > cpp/tests/testdata/trace_vectors.txt
// math_vectors.txt must come from a CPU with AVX and FMA, where Go's Exp
// takes its FMA path.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gomath.h"
#include "random.h"
#include "replay.h"
#include "tpcc.h"
#include "users.h"

namespace celltnx::workload {
namespace {

uint64_t Hex(const std::string &s) { return std::stoull(s, nullptr, 16); }
double F(const std::string &s) { return std::bit_cast<double>(Hex(s)); }
uint64_t B(double f) { return std::bit_cast<uint64_t>(f); }

void CheckMath(const std::string &file, bool fma) {
  std::ifstream in(file);
  ASSERT_TRUE(in.good()) << file;
  // Pow dispatches on this CPU, so only the matching file applies to it.
  bool pow = gomath::UseFMA() == fma;
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    std::istringstream is(line);
    std::string op, a, b, c;
    is >> op >> a >> b >> c;
    if (op == "exp") {
      EXPECT_EQ(B(gomath::Exp(F(a), fma)), Hex(b)) << line;
    } else if (op == "log") {
      EXPECT_EQ(B(gomath::Log(F(a))), Hex(b)) << line;
    } else if (op == "pow" && pow) {
      EXPECT_EQ(B(GoPow(F(a), F(b))), Hex(c)) << line;
    }
    n++;
  }
  EXPECT_EQ(n, 4575);
}

TEST(GoCompat, MathFMA) { CheckMath(CELLTNX_TESTDATA "/math_vectors.txt", true); }
TEST(GoCompat, MathNoFMA) { CheckMath(CELLTNX_TESTDATA "/math_vectors_nofma.txt", false); }

TEST(GoCompat, FloatToUint64) {
  EXPECT_EQ(GoFloatToUint64(3.9), 3u);
  EXPECT_EQ(GoFloatToUint64(9223372036854775808.0), uint64_t(1) << 63);
  EXPECT_EQ(GoFloatToUint64(1e19), 10000000000000000000ull);
  EXPECT_EQ(GoFloatToUint64(std::nan("")), uint64_t(1) << 63);
  EXPECT_EQ(GoFloatToUint64(-1.0), ~uint64_t(0));
}

// Each line is a workload drawn from Rand(seed, 7), its Detail, and the
// generator's next value; a Users line is followed by Zipf ranks drawn from
// the same generator. The trailing Zipf lines share Rand(1, 1).
TEST(GoCompat, WorkloadDraws) {
  std::ifstream in(CELLTNX_TESTDATA "/workload_vectors.txt");
  ASSERT_TRUE(in.good());
  std::unique_ptr<Rand> r;
  std::unique_ptr<Workload> w;
  Rand tail(1, 1);
  std::string line;
  int workloads = 0, zipfs = 0;
  while (std::getline(in, line)) {
    std::istringstream is(line);
    std::string kind;
    is >> kind;
    if (kind == "zipf") {
      uint64_t n;
      std::string theta;
      is >> n >> theta;
      Rand *zr = &tail;
      if (r != nullptr) {
        auto *u = dynamic_cast<Users *>(w.get());
        ASSERT_NE(u, nullptr) << line;
        EXPECT_EQ(u->Rows, n) << line;
        EXPECT_EQ(B(u->Theta), Hex(theta)) << line;
        zr = r.get();
      }
      Zipf z(n, F(theta));
      uint64_t want;
      int i = 0;
      for (; is >> want; i++) ASSERT_EQ(z.Next(*zr), want) << line << " draw " << i;
      EXPECT_GT(i, 0);
      r.reset();
      zipfs++;
      continue;
    }
    uint64_t seed, next;
    is >> seed >> next;
    std::string detail;
    std::getline(is >> std::ws, detail);
    r = std::make_unique<Rand>(seed, 7);
    if (kind == "random") w = NewRandom(*r);
    if (kind == "users") w = NewUsersFrom(*r);
    if (kind == "tpcc") w = NewTPCCFrom(*r);
    ASSERT_NE(w, nullptr) << line;
    EXPECT_EQ(w->Name(), kind);
    EXPECT_EQ(w->Detail(), detail) << line;
    EXPECT_EQ(r->Uint64(), next) << line;
    if (kind != "users") r.reset();
    workloads++;
  }
  EXPECT_EQ(workloads, 3000);
  EXPECT_EQ(zipfs, 1020);
}

// Baton hands control from client to client, as tracevec's goroutines do:
// one client runs at a time, and a seeded stream picks who runs next at
// every yield and sleep.
class Baton {
 public:
  Baton(uint64_t seed, int n) : r_(seed, 3) {
    for (int i = 0; i < n; i++) live_.push_back(i);
  }
  void Start() {
    std::lock_guard l(mu_);
    turn_ = Pick();
    cv_.notify_all();
  }
  void Wait(int i) {
    std::unique_lock l(mu_);
    cv_.wait(l, [&] { return turn_ == i; });
  }
  void Yield(int i, int64_t ticks) {
    std::unique_lock l(mu_);
    now_ += ticks + 1;
    int j = Pick();
    if (j == i) return;
    turn_ = j;
    cv_.notify_all();
    cv_.wait(l, [&] { return turn_ == i; });
  }
  void Exit(int i) {
    std::lock_guard l(mu_);
    live_.erase(std::find(live_.begin(), live_.end(), i));
    turn_ = live_.empty() ? -1 : Pick();
    cv_.notify_all();
  }
  int64_t Now() {
    std::lock_guard l(mu_);
    return now_;
  }

 private:
  int Pick() { return live_[r_.IntN(int64_t(live_.size()))]; }

  std::mutex mu_;
  std::condition_variable cv_;
  Rand r_;
  std::vector<int> live_;
  int turn_ = -1;
  int64_t now_ = 0;
};

class BatonEnv : public Env {
 public:
  BatonEnv(Baton *b, int i) : b_(b), i_(i) {}
  void Yield() override { b_->Yield(i_, 0); }
  void Sleep(int64_t ticks) override { b_->Yield(i_, ticks); }
  int64_t Now() override { return b_->Now(); }
  bool Stopped() override { return false; }

 private:
  Baton *b_;
  int i_;
};

// IdleEnv runs the loader alone.
class IdleEnv : public Env {
 public:
  void Yield() override {}
  void Sleep(int64_t) override {}
  int64_t Now() override { return 0; }
  bool Stopped() override { return false; }
};

std::string TraceRun(uint64_t seed, Mode mode, Workload &w) {
  DB db(Options{.mode = mode, .bucket_bits = unsigned(seed % 4)}, w.Schemas());
  check::Recorder rec;
  IdleEnv idle;
  Client loader{.DB = &db, .Rec = &rec, .Rand = Rand(seed, 2), .Env = &idle};
  if (Status st = w.Load(loader); !st.ok()) return "load: " + st.Message();
  int n = 1 + int(seed % 6);
  Baton baton(seed, n);
  std::vector<std::unique_ptr<BatonEnv>> envs;
  std::vector<Client> clients;
  for (int i = 0; i < n; i++) {
    envs.push_back(std::make_unique<BatonEnv>(&baton, i));
    clients.push_back(Client{
        .ID = i + 1, .DB = &db, .Rec = &rec, .Rand = Rand(seed, uint64_t(i + 4)), .Env = envs.back().get(), .Quota = 25});
  }
  std::vector<Status> errs(n);
  std::vector<std::thread> threads;
  for (int i = 0; i < n; i++) {
    threads.emplace_back([&, i] {
      baton.Wait(i);
      errs[i] = Run(w, clients[i]);
      baton.Exit(i);
    });
  }
  baton.Start();
  for (std::thread &t : threads) t.join();
  Stats st;
  int failed = 0;
  for (int i = 0; i < n; i++) {
    st.Add(clients[i].Stats);
    if (!errs[i].ok()) {
      ADD_FAILURE() << "run: " << errs[i].Message();
      failed++;
    }
  }
  std::vector<check::Log> logs = rec.Logs();
  Status replay = check::Replay(w.Schemas(), logs, db.LastCommitTs(), [&](int t) { return db.Dump(t); });
  if (!replay.ok()) ADD_FAILURE() << "replay: " << replay.Message();
  char trace[24];
  snprintf(trace, sizeof trace, "0x%llx", (unsigned long long)rec.Trace());
  std::ostringstream out;
  out << w.Name() << ' ' << seed << ' ' << ModeName(mode) << " clients=" << n << " trace=" << trace
      << " logs=" << logs.size() << " last=" << db.LastCommitTs() << " now=" << baton.Now()
      << " commits=" << st.Commits << " rejected=" << st.Rejected << " aborts=" << st.UserAborts
      << " conflicts=" << st.Conflicts << " rowconf=" << st.RowConf << " bucketconf=" << st.BucketConf
      << " failed=" << failed << " replay=" << (replay.ok() ? "ok" : "fail");
  return out.str();
}

TEST(GoCompat, Traces) {
  std::ifstream in(CELLTNX_TESTDATA "/trace_vectors.txt");
  ASSERT_TRUE(in.good());
  std::vector<std::string> want;
  for (std::string line; std::getline(in, line);) want.push_back(line);
  ASSERT_EQ(want.size(), 540u);
  size_t i = 0;
  for (uint64_t seed = 0; seed < 60; seed++) {
    for (Mode mode : {Mode::kRow, Mode::kCell, Mode::kCellDelta}) {
      Rand r(seed, 11);
      std::unique_ptr<Workload> ws[] = {NewRandom(r), NewUsersFrom(r), NewTPCCFrom(r)};
      for (auto &w : ws) EXPECT_EQ(TraceRun(seed, mode, *w), want[i++]);
    }
  }
}

}  // namespace
}  // namespace celltnx::workload
