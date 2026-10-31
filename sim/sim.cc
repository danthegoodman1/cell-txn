#include "sim.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "coroutine.h"
#include "format.h"
#include "rand.h"
#include "record.h"
#include "random.h"
#include "replay.h"
#include "tpcc.h"
#include "users.h"

namespace celltnx::sim {

std::string Params::String() const {
  std::string s = std::string("mode=") + ModeName(mode) + " workload=" + workload + " clients=" + std::to_string(clients) +
                  " quota=" + std::to_string(quota) + " bucketBits=" + std::to_string(bucket_bits) +
                  " skew=" + (skew ? "true" : "false") + " interleave=" + FormatFloat(interleave);
  if (durable)
    s += std::string(" durable nosync=") + (no_sync ? "true" : "false") + " crashP=" + FormatFloat(crash_p) +
         " syncFailP=" + FormatFloat(sync_fail_p) + " latency=" + std::to_string(latency) +
         " commitDelay=" + std::to_string(commit_delay);
  if (refresh != 0) s += " refresh=" + FormatFloat(refresh);
  return s + " " + detail;
}

namespace {

constexpr const char *kSyncFailed = "sim: sync failed";

bool SyncFailed(const Status &st) { return st.code() == Code::kIo && st.error().detail == kSyncFailed; }

struct Co {
  std::unique_ptr<Coroutine> fiber;
  int64_t wake = 0;
  int weight = 1;  // scheduling weight; skewed seeds starve some clients
  const std::function<bool()> *blocked = nullptr;  // set while waiting; runnable once it holds
  bool daemon = false;  // runs only while other coroutines do
  bool done = false;
};

struct Sched {
  int64_t now = 0;
  Co *cur = nullptr;

  // Yield suspends the running coroutine. Outside a coroutine it does
  // nothing.
  void Yield() {
    if (cur != nullptr) cur->fiber->Yield();
  }

  // Wait suspends the running coroutine until cond holds.
  void Wait(const std::function<bool()> &cond) {
    Co *c = cur;
    while (!cond()) {
      if (c == nullptr) throw InvariantError("sim: wait outside a coroutine");
      c->blocked = &cond;
      Yield();
    }
    if (c != nullptr) c->blocked = nullptr;
  }
};

// SimEnv is a client's view of the scheduler.
struct SimEnv : workload::Env {
  explicit SimEnv(Sched *s) : s(s) {}
  void Yield() override { s->Yield(); }
  int64_t Now() override { return s->now; }
  bool Stopped() override { return false; }
  void Sleep(int64_t ticks) override {
    if (Co *c = s->cur) {
      c->wake = s->now + ticks;
      s->Yield();
    }
  }
  Sched *s;
};

// Disk is a simulated Store. Written commits become durable at Sync; a
// crash keeps the synced commits plus a random prefix of the rest, since
// the real store writes commits atomically and in order.
struct Disk : Store {
  struct DWrite {
    int table;
    std::string key;
    std::optional<OwnedRow> row;  // empty for a delete
  };
  struct DCommit {
    uint64_t ts;
    std::vector<DWrite> writes;
  };

  Disk(Sched *s, Rand r, int64_t latency) : env(s), r(r), latency(latency) {}

  Status Write(std::span<const Commit> batch) override {
    for (const Commit &c : batch) {
      DCommit d{c.ts, {}};
      for (const WriteRef &w : c.writes) {
        DWrite dw{w.table, std::string(w.key), std::nullopt};
        if (!w.deleted()) dw.row.emplace(w.row());
        d.writes.push_back(std::move(dw));
      }
      written.push_back(std::move(d));
    }
    return {};
  }

  Status Sync() override {
    if (latency > 0) env.Sleep(1 + r.Int64N(latency));
    if (r.Float64() < fail_p) return Error{.code = Code::kIo, .detail = kSyncFailed};
    for (auto &c : written) durable.push_back(std::move(c));
    written.clear();
    return {};
  }

  // Crash drops a random suffix of the unsynced commits and returns the
  // newest surviving commit timestamp.
  uint64_t Crash() {
    size_t keep = size_t(r.IntN(int64_t(written.size()) + 1));
    for (size_t i = 0; i < keep; i++) durable.push_back(std::move(written[i]));
    written.clear();
    return durable.empty() ? 0 : durable.back().ts;
  }

  // Load replays the durable commits and passes each live row to put, in
  // table and key order.
  Status Load(const DB::Put &put) {
    std::map<int, std::map<std::string, const OwnedRow *>> rows;
    for (const DCommit &c : durable)
      for (const DWrite &w : c.writes) {
        if (!w.row)
          rows[w.table].erase(w.key);
        else
          rows[w.table][w.key] = &*w.row;
      }
    for (auto &[t, keys] : rows)
      for (auto &[k, row] : keys) put(t, k, row->row());
    return {};
  }

  SimEnv env;
  Rand r;
  std::vector<DCommit> durable, written;
  int64_t latency;  // max simulated sync time
  double fail_p = 0;  // chance a sync fails
};

// DrawRefresh returns the seed's refresh probability, from its own stream.
double DrawRefresh(uint64_t seed, const Config &cfg) {
  if (cfg.refresh >= 0) return cfg.refresh;
  Rand g(seed, 0x510e527fade682d1ull);
  static const double kChoices[] = {0, 0.02, 0.1, 0.5};
  return kChoices[g.IntN(4)];
}

// DrawInterleave returns the seed's switch probability at interleave
// points, from its own stream so every other parameter keeps its value.
double DrawInterleave(uint64_t seed, const Config &cfg) {
  if (cfg.interleave >= 0) return cfg.interleave;
  Rand g(seed, 0xa54ff53a5f1d36f1ull);
  static const double kChoices[] = {0, 0.01, 0.1, 0.5};
  return kChoices[g.IntN(4)];
}

std::pair<Params, std::unique_ptr<workload::Workload>> Draw(uint64_t seed, Rand &r, const Config &cfg) {
  Params p;
  p.mode = Mode(r.IntN(3));
  p.clients = int(1 + r.IntN(16));
  p.quota = int(3 + r.IntN(40));
  p.bucket_bits = unsigned(r.IntN(5));
  p.skew = r.IntN(3) == 0;
  p.durable = r.IntN(2) == 0;
  if (cfg.mode >= 0) p.mode = Mode(cfg.mode);
  if (cfg.durable >= 0) p.durable = cfg.durable == 1;
  if (p.durable) {
    p.no_sync = r.IntN(4) == 0;
    static const double kCrash[] = {0, 0.0005, 0.002, 0.01}, kFail[] = {0, 0, 0.001, 0.01};
    static const int64_t kLatency[] = {0, 5, 50}, kDelay[] = {0, 0, 0, 2, 10, 50};
    p.crash_p = kCrash[r.IntN(4)];
    p.sync_fail_p = kFail[r.IntN(4)];
    p.latency = kLatency[r.IntN(3)];
    // A separate stream leaves every other parameter as before. Delays stay
    // below the 100 steps between crashes at the highest crash rate, so
    // batches still sync between crashes.
    Rand g(seed, 0xbb67ae8584caa73bull);
    p.commit_delay = kDelay[g.IntN(6)];
    if (cfg.commit_delay >= 0) p.commit_delay = cfg.commit_delay;
  }
  p.workload = cfg.workload;
  if (p.workload.empty()) {
    static const char *kWorkloads[] = {"random", "random", "users", "tpcc"};
    p.workload = kWorkloads[r.IntN(4)];
  }
  std::unique_ptr<workload::Workload> w;
  if (p.workload == "random")
    w = workload::NewRandom(r);
  else if (p.workload == "users")
    w = workload::NewUsersFrom(r);
  else if (p.workload == "tpcc")
    w = workload::NewTPCCFrom(r);
  else
    throw InvariantError("sim: unknown workload " + p.workload);
  p.detail = w->Detail();
  return {std::move(p), std::move(w)};
}

// Simulation holds one seed's run. Members are destroyed in reverse order,
// so the coroutines, which hold transactions, go before the DB.
struct Simulation {
  Simulation(uint64_t seed, const Config &cfg) : seed(seed), cfg(cfg), r(seed, 0x9e3779b97f4a7c15ull) {}

  // Child returns a generator seeded with the scheduler stream's next two
  // values.
  Rand Child() {
    uint64_t a = r.Uint64();
    uint64_t b = r.Uint64();
    return Rand(a, b);
  }

  std::unique_ptr<DB> Open() {
    Options o;
    o.mode = p.mode;
    o.bucket_bits = p.bucket_bits;
    o.yield = [this] { s.Yield(); };
    o.interleave = interleave;
    o.wait = [this](const std::function<bool()> &cond) { s.Wait(cond); };
    o.no_invariants = cfg.no_invariants;
    o.keep_trimmed = true;
    if (p.durable) {
      o.store = disk.get();
      o.no_sync = p.no_sync;
      if (p.commit_delay > 0) o.commit_delay = [this] { env.Sleep(p.commit_delay); };
    }
    return std::make_unique<DB>(std::move(o), schemas);
  }

  Co *Spawn(std::function<Status()> body) {
    auto c = std::make_unique<Co>();
    c->fiber = std::make_unique<Coroutine>(std::move(body));
    cos.push_back(std::move(c));
    return cos.back().get();
  }

  void StopAll() {
    for (auto &c : cos) c->fiber->Stop();
    cos.clear();
  }

  void StartWriter() {
    if (!p.durable) return;
    DB *d = db.get();
    Spawn([d] { return d->RunWriter(); })->daemon = true;
  }

  void StartClients() {
    for (size_t i = 0; i < clients.size(); i++) {
      workload::Client *c = clients[i].get();
      if (c->Done < c->Quota) {
        c->DB = db.get();
        Spawn([this, c] { return workload::Run(*w, *c); })->weight = weights[i];
      }
    }
  }

  // Crash stops every coroutine, drops the disk's unsynced suffix, recovers
  // a new DB from what survived, checks it against the history, and
  // restarts the writer and the unfinished clients.
  Status Crash() {
    res.crashes++;
    StopAll();
    db->Close();
    uint64_t last = disk->Crash();
    if (Status st = rec.Truncate(last, !p.no_sync); !st.ok()) return st;
    db.reset();
    db = Open();
    if (Status st = db->Recover(last, [this](const DB::Put &put) { return disk->Load(put); }); !st.ok()) return st;
    DB *d = db.get();
    if (Status st = check::Replay(schemas, rec.Logs(), last, [d](int tbl) { return d->Dump(tbl); }); !st.ok())
      return Error{.code = Code::kInternal,
                   .detail = "recovered state after crash " + std::to_string(res.crashes) + ": " + st.Message()};
    StartWriter();
    StartClients();
    return {};
  }

  Status Loop(bool crashes) {
    std::vector<Co *> runnable;
    for (;;) {
      runnable.clear();
      int64_t wake = -1;
      bool live = false;
      int64_t total = 0;
      for (auto &cp : cos) {
        Co *c = cp.get();
        if (c->done) continue;
        live = live || !c->daemon;
        if (c->wake > s.now) {
          if (wake < 0 || c->wake < wake) wake = c->wake;
        } else if (c->blocked == nullptr || (*c->blocked)()) {
          runnable.push_back(c);
          total += c->weight;
        }
      }
      if (!live) return {};
      if (runnable.empty()) {
        if (wake < 0) return Error{.code = Code::kInternal, .detail = "deadlock: every coroutine is blocked"};
        s.now = wake;
        continue;
      }
      Co *c = nullptr;
      int64_t n = r.IntN(total);
      for (Co *x : runnable) {
        c = x;
        if (n < x->weight) break;
        n -= x->weight;
      }
      s.cur = c;
      bool more = c->fiber->Next();
      s.cur = nullptr;
      if (!more) {
        c->done = true;
        const Status &err = c->fiber->err();
        if (SyncFailed(err) && crashes) {
          if (Status st = Crash(); !st.ok()) return st;
        } else if (!err.ok()) {
          return Error(err.error());
        }
      }
      s.now++;
      res.steps++;
      if (res.steps > max_steps)
        return Error{.code = Code::kInternal,
                     .detail = "liveness: clients unfinished after " + std::to_string(max_steps) + " steps"};
      if (!cfg.no_invariants && res.steps % 1000 == 0 && !db->Committing())
        if (Status st = db->CheckInvariants(); !st.ok()) return st;
      if (crashes && r.Float64() < p.crash_p)
        if (Status st = Crash(); !st.ok()) return st;
    }
  }

  Status Body() {
    auto [params, wl] = Draw(seed, r, cfg);
    p = std::move(params);
    w = std::move(wl);
    p.interleave = DrawInterleave(seed, cfg);
    res.params = p;
    max_steps = cfg.max_steps != 0 ? cfg.max_steps : 5'000'000;
    if (p.interleave != 0) {
      auto ir = std::make_shared<Rand>(seed, 0x3c6ef372fe94f82bull);
      double prob = p.interleave;
      // Its decisions come from their own stream, so they never shift the
      // scheduler's draws.
      interleave = [this, ir, prob] {
        if (s.cur != nullptr && ir->Float64() < prob) s.Yield();
      };
    }
    p.refresh = DrawRefresh(seed, cfg);
    res.params.refresh = p.refresh;
    if (p.refresh != 0) {
      auto rr = std::make_shared<Rand>(seed, 0x9b05688c2b3e6c1full);
      double prob = p.refresh;
      rec.refresh = [rr, prob] { return rr->Float64() < prob; };
    }
    disk = std::make_unique<Disk>(&s, Child(), p.latency);
    schemas = w->Schemas();
    db = Open();

    for (int i = 0; i < p.clients; i++) {
      auto c = std::make_unique<workload::Client>();
      c->ID = i + 1;
      c->Rec = &rec;
      c->Rand = Child();
      c->Env = &env;
      c->Quota = p.quota;
      clients.push_back(std::move(c));
      weights.push_back(p.skew ? 1 << r.IntN(6) : 1);
    }

    StartWriter();
    loader.DB = db.get();
    loader.Rec = &rec;
    loader.Rand = Child();
    loader.Env = &env;
    Spawn([this] { return w->Load(loader); });
    if (Status st = Loop(false); !st.ok()) return st;
    if (p.durable) {
      // Even without sync acknowledgments, the initial data must survive.
      Spawn([this] {
        s.Wait([this] { return db->DurableTs() >= db->LastCommitTs(); });
        return Status();
      });
      if (Status st = Loop(false); !st.ok()) return st;
    }
    disk->fail_p = p.sync_fail_p;
    StartClients();
    if (Status st = Loop(true); !st.ok()) return st;

    for (auto &c : clients) res.stats.Add(c->Stats);
    if (!cfg.no_invariants)
      if (Status st = db->CheckInvariants(); !st.ok()) return st;
    std::vector<check::Log> logs = rec.Logs();
    res.logs = int(logs.size());
    res.trace = rec.Trace() ^ uint64_t(res.steps);
    DB *d = db.get();
    return check::Replay(schemas, logs, db->LastCommitTs(), [d](int tbl) { return d->Dump(tbl); });
  }

  uint64_t seed;
  Config cfg;
  Rand r;
  Params p;
  Result res;
  int64_t max_steps = 0;
  Sched s;
  SimEnv env{&s};
  std::function<void()> interleave;
  std::unique_ptr<workload::Workload> w;
  std::vector<Schema> schemas;
  check::Recorder rec;
  std::unique_ptr<Disk> disk;
  std::unique_ptr<DB> db;
  std::vector<std::unique_ptr<workload::Client>> clients;
  std::vector<int> weights;
  workload::Client loader;
  std::vector<std::unique_ptr<Co>> cos;
};

}  // namespace

Result Run(uint64_t seed, const Config &cfg) {
  Simulation sim(seed, cfg);
  sim.res.seed = seed;
  try {
    Status st = sim.Body();
    if (!st.ok()) sim.res.err = st.Message();
  } catch (const std::exception &e) {
    sim.res.err = std::string("panic: ") + e.what();
  }
  sim.StopAll();
  return sim.res;
}

}  // namespace celltnx::sim
