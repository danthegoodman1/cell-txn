// Checks lionturtle against celltnx's use of it: one writer thread commits
// TPC-C-shaped batches (hot warehouse and district rows, customer and stock
// updates, inserted orders, and a commit-timestamp key), syncs once per group,
// and recovery scans everything back.
//
//   usecase edges DIR                 API edge cases the engine relies on
//   usecase load DIR SECONDS GROUP    write for SECONDS; GROUP commits per sync, 0 = never sync
//   usecase verify DIR [ACKED]        reopen, check the state against the model
//   usecase crash DIR ROUNDS GROUP    kill -9 the writer repeatedly, verify each recovery
//   usecase fault DIR GROUP N SHIM    fail the Nth sync through SHIM, then verify recovery
//
// LT_CACHE_MB sets cache_bytes. With LT_ACK_FD set, load writes each acknowledged
// timestamp (8 bytes) to that descriptor.
#include <lionturtle.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double us_since(Clock::time_point t) { return std::chrono::duration<double, std::micro>(Clock::now() - t).count(); }

[[noreturn]] static void die(const char *what, lt_status st) {
  fprintf(stderr, "%s: status %d: %s\n", what, int(st), lt_last_error());
  exit(3);
}

#define CHECK(call, what)                     \
  do {                                        \
    lt_status st_ = (call);                   \
    if (st_ != LT_OK) die(what, st_);         \
  } while (0)

// --- the model ---

static uint64_t mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
  x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
  return x ^ (x >> 31);
}
static uint64_t h(uint64_t ts, uint64_t n) { return mix(ts * 64 + n); }

enum : uint8_t { T_WH = 1, T_DIST, T_CUST, T_STOCK, T_HIST, T_ORDERS, T_NEWORD, T_OLINE, T_META = 0xFF };
constexpr uint64_t D = 10, C = 30000, S = 100000, LINES = 10;
static const size_t VSIZE[] = {0, 48, 64, 80, 48, 40, 48, 24, 64};

static void put64(std::string &s, uint64_t v) {
  for (int i = 7; i >= 0; i--) s.push_back(char(v >> (8 * i)));
}
static uint64_t get64(const char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | uint8_t(p[i]);
  return v;
}
static std::string key(uint8_t t, uint64_t a) {
  std::string k(1, char(t));
  put64(k, a);
  return k;
}
static std::string key2(uint8_t t, uint64_t a, uint64_t b) {
  std::string k = key(t, a);
  put64(k, b);
  return k;
}
static std::string row(uint8_t t, uint64_t ts, uint64_t id) {
  std::string v;
  put64(v, ts);
  put64(v, id);
  v.push_back(char(t));
  while (v.size() < VSIZE[t]) v.push_back(char('a' + (ts + v.size()) % 26));
  return v;
}
static const std::string META_KEY(1, char(T_META));

struct Op {
  std::string k, v;
};

// Commit ts is a Payment when even and a NewOrder when odd.
static void commit_ops(uint64_t ts, std::vector<Op> &ops) {
  ops.clear();
  uint64_t d = h(ts, 0) % D;
  if (ts % 2 == 0) {
    ops.push_back({key(T_WH, 0), row(T_WH, ts, 0)});
    ops.push_back({key(T_DIST, d), row(T_DIST, ts, d)});
    uint64_t c = h(ts, 1) % C;
    ops.push_back({key(T_CUST, c), row(T_CUST, ts, c)});
    ops.push_back({key(T_HIST, ts), row(T_HIST, ts, ts)});
  } else {
    ops.push_back({key(T_DIST, d), row(T_DIST, ts, d)});
    ops.push_back({key(T_ORDERS, ts), row(T_ORDERS, ts, ts)});
    ops.push_back({key(T_NEWORD, ts), row(T_NEWORD, ts, ts)});
    uint64_t seen[LINES];
    for (uint64_t n = 1; n <= LINES; n++) {
      ops.push_back({key2(T_OLINE, ts, n), row(T_OLINE, ts, n)});
      uint64_t s = h(ts, 10 + n) % S;
      seen[n - 1] = s;
      if (std::find(seen, seen + n - 1, s) == seen + n - 1) ops.push_back({key(T_STOCK, s), row(T_STOCK, ts, s)});
    }
  }
  std::string m;
  put64(m, ts);
  ops.push_back({META_KEY, m});
}

static lt_db *open_db(const char *dir, double *ms = nullptr) {
  lt_options o = lt_options_default();
  if (const char *c = getenv("LT_CACHE_MB")) o.cache_bytes = strtoull(c, nullptr, 10) << 20;
  if (const char *c = getenv("LT_CHI")) o.checkpoint_distance = strtoull(c, nullptr, 10);
  lt_db *db;
  auto t = Clock::now();
  CHECK(lt_open(dir, &o, &db), "open");
  if (ms) *ms = us_since(t) / 1000;
  return db;
}

static uint64_t read_meta(lt_db *db) {
  lt_value *v;
  CHECK(lt_get(db, META_KEY.data(), 1, &v), "get meta");
  if (!v) return 0;
  uint64_t ts = get64(lt_value_bytes(v).data);
  lt_value_free(v);
  return ts;
}

static std::string proc_field(const char *file, const char *name) {
  std::ifstream f(file);
  std::string line;
  while (std::getline(f, line))
    if (line.rfind(name, 0) == 0) return line.substr(strlen(name));
  return "?";
}

static double pct(std::vector<double> &v, double p) {
  if (v.empty()) return 0;
  size_t i = std::min(v.size() - 1, size_t(p * v.size()));
  std::nth_element(v.begin(), v.begin() + i, v.end());
  return v[i];
}

// --- load ---

static int load(const char *dir, double seconds, uint64_t group) {
  int ack_fd = getenv("LT_ACK_FD") ? atoi(getenv("LT_ACK_FD")) : -1;
  lt_db *db = open_db(dir);
  uint64_t first = read_meta(db) + 1, ts = first;
  lt_batch *batch = lt_batch_new();
  std::vector<Op> ops;
  std::vector<double> wlat, slat, cycle;
  uint64_t rows = 0, bytes = 0, slow = 0;
  auto start = Clock::now();
  auto deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
  uint64_t per_round = group ? group : 256;
  while (Clock::now() < deadline) {
    auto c0 = Clock::now();
    for (uint64_t g = 0; g < per_round; g++, ts++) {
      commit_ops(ts, ops);
      lt_batch_clear(batch);
      for (auto &op : ops) {
        CHECK(lt_batch_put(batch, op.k.data(), op.k.size(), op.v.data(), op.v.size()), "batch put");
        bytes += op.k.size() + op.v.size();
      }
      rows += ops.size();
      auto t = Clock::now();
      lt_status st = lt_write(db, batch, false);
      if (st != LT_OK) die("write", st);
      wlat.push_back(us_since(t));
    }
    if (group) {
      auto t = Clock::now();
      lt_status st = lt_sync(db);
      if (st != LT_OK) {
        fprintf(stderr, "sync failed after commit %llu: status %d: %s\n", (unsigned long long)(ts - 1), int(st),
                lt_last_error());
        lt_status w = lt_write(db, batch, false);
        lt_status s = lt_sync(db);
        fprintf(stderr, "then: write status %d (%s), sync status %d\n", int(w), w ? lt_last_error() : "ok", int(s));
        exit(4);
      }
      slat.push_back(us_since(t));
      uint64_t acked = ts - 1;
      if (ack_fd >= 0 && write(ack_fd, &acked, 8) != 8) return 1;
    }
    double c = us_since(c0);
    cycle.push_back(c);
    if (c > 50000) slow++;
  }
  double secs = std::chrono::duration<double>(Clock::now() - start).count();
  uint64_t n = ts - first;
  fprintf(stderr, "load: %llu commits in %.1fs = %.0f commits/s, %.0f rows/s, %.0f MB/s of keys+values\n",
          (unsigned long long)n, secs, n / secs, rows / secs, bytes / secs / 1e6);
  fprintf(stderr, "  lt_write us: p50 %.1f p99 %.1f p99.9 %.1f max %.0f\n", pct(wlat, .5), pct(wlat, .99),
          pct(wlat, .999), *std::max_element(wlat.begin(), wlat.end()));
  if (group)
    fprintf(stderr, "  lt_sync us: p50 %.0f p99 %.0f max %.0f\n", pct(slat, .5), pct(slat, .99),
            *std::max_element(slat.begin(), slat.end()));
  fprintf(stderr, "  rounds of %llu commits%s over 50 ms: %llu of %zu; slowest %.0f ms\n",
          (unsigned long long)per_round, group ? " + sync" : "", (unsigned long long)slow, cycle.size(),
          *std::max_element(cycle.begin(), cycle.end()) / 1000);
  fprintf(stderr, "  peak RSS%s, threads%s, disk writes %.0f MB (%.2f x user bytes)\n",
          proc_field("/proc/self/status", "VmHWM:").c_str(), proc_field("/proc/self/status", "Threads:").c_str(),
          atof(proc_field("/proc/self/io", "write_bytes: ").c_str()) / 1e6,
          atof(proc_field("/proc/self/io", "write_bytes: ").c_str()) / double(bytes));
  auto t = Clock::now();
  lt_batch_free(batch);
  lt_close(db);
  fprintf(stderr, "  close %.0f ms\n", us_since(t) / 1000);
  return 0;
}

// --- verify ---

struct Expect {
  uint8_t t;
  uint64_t a, b;
  uint64_t ts;
};

// Checks every table against the model at the recovered timestamp.
static int verify(const char *dir, uint64_t acked, bool quiet = false) {
  double open_ms;
  lt_db *db = open_db(dir, &open_ms);
  uint64_t r = read_meta(db);
  if (r < acked) {
    fprintf(stderr, "verify: recovered through %llu, but %llu was acknowledged\n", (unsigned long long)r,
            (unsigned long long)acked);
    return 1;
  }
  uint64_t wh = 0;
  std::vector<uint64_t> dist(D), cust(C), stock(S);
  for (uint64_t ts = 1; ts <= r; ts++) {
    uint64_t d = h(ts, 0) % D;
    dist[d] = ts;
    if (ts % 2 == 0) {
      wh = ts;
      cust[h(ts, 1) % C] = ts;
    } else {
      for (uint64_t n = 1; n <= LINES; n++) stock[h(ts, 10 + n) % S] = ts;
    }
  }
  // The expected entries in key order, table by table.
  std::vector<Expect> exp;
  auto gen = [&](uint8_t t) {
    exp.clear();
    switch (t) {
      case T_WH:
        if (wh) exp.push_back({t, 0, 0, wh});
        break;
      case T_DIST:
        for (uint64_t i = 0; i < D; i++)
          if (dist[i]) exp.push_back({t, i, 0, dist[i]});
        break;
      case T_CUST:
        for (uint64_t i = 0; i < C; i++)
          if (cust[i]) exp.push_back({t, i, 0, cust[i]});
        break;
      case T_STOCK:
        for (uint64_t i = 0; i < S; i++)
          if (stock[i]) exp.push_back({t, i, 0, stock[i]});
        break;
      case T_HIST:
        for (uint64_t ts = 2; ts <= r; ts += 2) exp.push_back({t, ts, 0, ts});
        break;
      case T_ORDERS:
      case T_NEWORD:
        for (uint64_t ts = 1; ts <= r; ts += 2) exp.push_back({t, ts, 0, ts});
        break;
      case T_OLINE:
        for (uint64_t ts = 1; ts <= r; ts += 2)
          for (uint64_t n = 1; n <= LINES; n++) exp.push_back({t, ts, n, ts});
        break;
    }
  };
  uint64_t total = 0;
  auto t0 = Clock::now();
  for (uint8_t t = T_WH; t <= T_OLINE; t++) {
    gen(t);
    lt_iter *it;
    char p = char(t);
    CHECK(lt_iter_prefix(db, &p, 1, false, &it), "iter");
    size_t i = 0;
    for (;; i++) {
      bool has;
      lt_slice k, v;
      CHECK(lt_iter_next(it, &has, &k, &v), "next");
      if (!has) break;
      if (i >= exp.size()) {
        fprintf(stderr, "table %d: unexpected extra entry at ts %llu\n", t, (unsigned long long)get64(k.data + 1));
        return 1;
      }
      const Expect &e = exp[i];
      std::string ek = t == T_OLINE ? key2(t, e.a, e.b) : key(t, e.a);
      std::string ev = row(t, e.ts, t == T_OLINE ? e.b : e.a);
      if (std::string(k.data, k.len) != ek || std::string(v.data, v.len) != ev) {
        fprintf(stderr, "table %d entry %zu: expected id %llu at ts %llu, found key %zu bytes, value ts %llu\n", t, i,
                (unsigned long long)e.a, (unsigned long long)e.ts, k.len,
                v.len >= 8 ? (unsigned long long)get64(v.data) : 0ULL);
        return 1;
      }
    }
    lt_iter_free(it);
    if (i != exp.size()) {
      fprintf(stderr, "table %d: %zu entries, expected %zu\n", t, i, exp.size());
      return 1;
    }
    total += i;
  }
  double scan_s = us_since(t0) / 1e6;
  // Nothing outside the known tables, and a full scan agrees.
  lt_iter *it;
  CHECK(lt_iter_new(db, nullptr, 0, nullptr, 0, false, &it), "iter all");
  uint64_t all = 0;
  for (;;) {
    bool has;
    CHECK(lt_iter_next(it, &has, nullptr, nullptr), "next");
    if (!has) break;
    all++;
  }
  lt_iter_free(it);
  lt_close(db);
  if (r) total++;  // the commit-timestamp key
  if (all != total) {
    fprintf(stderr, "full scan found %llu entries, expected %llu\n", (unsigned long long)all,
            (unsigned long long)total);
    return 1;
  }
  if (!quiet)
    printf("verify: through %llu (acknowledged %llu); %llu rows match; open %.0f ms; scan %.2f s = %.1fM rows/s\n",
           (unsigned long long)r, (unsigned long long)acked, (unsigned long long)total, open_ms, scan_s,
           total / scan_s / 1e6);
  return 0;
}

// --- crash and fault ---

static pid_t spawn_load(const char *self, const char *dir, uint64_t group, int ack_w, const char *preload) {
  pid_t pid = fork();
  if (pid == 0) {
    dup2(ack_w, 3);
    setenv("LT_ACK_FD", "3", 1);
    if (preload) setenv("LD_PRELOAD", preload, 1);
    std::string g = std::to_string(group);
    execl(self, self, "load", dir, "600", g.c_str(), (char *)nullptr);
    _exit(127);
  }
  return pid;
}

static uint64_t drain(int fd, uint64_t acked) {
  uint64_t v;
  while (read(fd, &v, 8) == 8) acked = v;
  return acked;
}

static int crash(const char *self, const char *dir, int rounds, uint64_t group) {
  uint64_t acked = 0;
  std::mt19937_64 rng(7);
  for (int round = 0; round < rounds; round++) {
    if (verify(dir, acked)) return 1;
    int p[2];
    if (pipe(p)) return 1;
    pid_t pid = spawn_load(self, dir, group, p[1], nullptr);
    close(p[1]);
    usleep(300000 + rng() % 2700000);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    acked = drain(p[0], acked);
    close(p[0]);
  }
  return verify(dir, acked);
}

static int fault(const char *self, const char *dir, uint64_t group, const char *n, const char *shim) {
  int p[2];
  if (pipe(p)) return 1;
  setenv("FAIL_SYNC_AT", n, 1);
  pid_t pid = spawn_load(self, dir, group, p[1], shim);
  close(p[1]);
  uint64_t acked = drain(p[0], 0);  // until the writer exits and closes its end
  close(p[0]);
  int status;
  waitpid(pid, &status, 0);
  unsetenv("FAIL_SYNC_AT");
  printf("fault: writer exited with %d after acknowledging %llu\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
         (unsigned long long)acked);
  return verify(dir, acked);
}

// --- edge cases ---

static int failures = 0;
static void expect(bool ok, const char *what) {
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static std::vector<std::string> scan_prefix(lt_db *db, const std::string &p) {
  std::vector<std::string> out;
  lt_iter *it;
  CHECK(lt_iter_prefix(db, p.data(), p.size(), false, &it), "iter");
  for (;;) {
    bool has;
    lt_slice k;
    CHECK(lt_iter_next(it, &has, &k, nullptr), "next");
    if (!has) break;
    out.emplace_back(k.data, k.len);
  }
  lt_iter_free(it);
  return out;
}

static bool get(lt_db *db, const std::string &k, std::string *v) {
  lt_value *val;
  CHECK(lt_get(db, k.data(), k.size(), &val), "get");
  if (!val) return false;
  lt_slice s = lt_value_bytes(val);
  if (v) v->assign(s.data, s.len);
  lt_value_free(val);
  return true;
}

static int edges(const char *dir) {
  lt_db *db = open_db(dir);
  using S = std::string;
  // Binary keys: a prefix ending in 0xFF must stop before the next prefix,
  // and an all-0xFF prefix has no upper bound to compute.
  std::vector<S> keys = {S("\x01\xfe\xff", 3), S("\x01\xff\xff", 3) + S("\x00", 1), S("\x01\xff\xff\xff", 4),
                         S("\x02\x00\x00", 3), S("\xff\xff", 2), S("\xff\xff\x00", 3), S("\xff\xff\xff\xff", 4)};
  for (auto &k : keys) CHECK(lt_put(db, k.data(), k.size(), "v", 1, false), "put");
  auto a = scan_prefix(db, S("\x01\xff\xff", 3));
  expect(a.size() == 2 && a[0] == keys[1] && a[1] == keys[2], "prefix ending in 0xFF stops before the next prefix");
  auto b = scan_prefix(db, S("\xff\xff", 2));
  expect(b.size() == 3, "all-0xFF prefix returns every key under it");
  // NUL bytes and empty values.
  S nk("a\0b\0", 4), nv("\0\0x", 3), out;
  CHECK(lt_put(db, nk.data(), nk.size(), nv.data(), nv.size(), false), "put");
  CHECK(lt_put(db, "empty", 5, nullptr, 0, false), "put empty");
  expect(get(db, nk, &out) && out == nv, "keys and values with NUL bytes round-trip");
  expect(get(db, "empty", &out) && out.empty(), "an empty value reads back as present and empty");
  // Size limits, and an oversized write leaves the store writable.
  S kmax(65535, 'k'), kover(65536, 'k'), v16(16 << 20, 'v'), v16x((16 << 20) + 1, 'v');
  expect(lt_put(db, kmax.data(), kmax.size(), "v", 1, false) == LT_OK, "65,535-byte key accepted");
  expect(lt_put(db, kover.data(), kover.size(), "v", 1, false) == LT_INVALID_ARGUMENT, "65,536-byte key rejected");
  expect(lt_put(db, "big", 3, v16.data(), v16.size(), false) == LT_OK, "16 MiB value accepted");
  expect(lt_put(db, "big2", 4, v16x.data(), v16x.size(), false) == LT_INVALID_ARGUMENT, "16 MiB + 1 value rejected");
  // lt_batch_put accepts an oversized op; lt_write rejects the whole batch.
  lt_batch *bad = lt_batch_new();
  CHECK(lt_batch_put(bad, "whole", 5, "1", 1), "batch put");
  CHECK(lt_batch_put(bad, kover.data(), kover.size(), "v", 1), "batch put");
  expect(lt_write(db, bad, true) == LT_INVALID_ARGUMENT, "a batch holding an oversized op is rejected");
  lt_batch_free(bad);
  expect(!get(db, "whole", nullptr), "no op of a rejected batch applies");
  expect(lt_put(db, "after", 5, "1", 1, true) == LT_OK, "store still writable after rejected writes");
  lt_status st;
  // The same key twice in one batch.
  lt_batch *dup = lt_batch_new();
  CHECK(lt_batch_put(dup, "dup", 3, "first", 5), "batch put");
  CHECK(lt_batch_put(dup, "dup", 3, "second", 6), "batch put");
  CHECK(lt_write(db, dup, false), "write dup");
  lt_batch_free(dup);
  expect(get(db, "dup", &out) && out == "second", "a key written twice in one batch keeps the later value");
  // Batches far larger than a commit: a bulk insert, then a table drop.
  for (uint64_t n : {1000000ull, 3000000ull}) {
    lt_batch *big = lt_batch_new();
    std::string prefix = "B" + std::to_string(n) + "/";
    for (uint64_t i = 0; i < n; i++) {
      std::string k = prefix;
      put64(k, i);
      std::string v(56, char('a' + i % 26));
      CHECK(lt_batch_put(big, k.data(), k.size(), v.data(), v.size()), "batch put");
    }
    auto t = Clock::now();
    st = lt_write(db, big, true);
    double put_ms = us_since(t) / 1000;
    expect(st == LT_OK, (std::to_string(n) + "-row batch of puts, synced").c_str());
    lt_batch_clear(big);
    for (uint64_t i = 0; i < n; i++) {
      std::string k = prefix;
      put64(k, i);
      CHECK(lt_batch_delete(big, k.data(), k.size()), "batch delete");
    }
    t = Clock::now();
    st = lt_write(db, big, true);
    double del_ms = us_since(t) / 1000;
    expect(st == LT_OK, (std::to_string(n) + "-row batch of deletes, synced").c_str());
    printf("         %llu rows (%.0f MB): put %.0f ms, delete %.0f ms\n", (unsigned long long)n, n * 66 / 1e6, put_ms,
           del_ms);
    lt_batch_free(big);
  }
  // lt_close makes unsynced writes durable; deletes survive checkpoint and reopen.
  CHECK(lt_put(db, "unsynced", 8, "1", 1, false), "put");
  CHECK(lt_checkpoint(db), "checkpoint");
  lt_close(db);
  db = open_db(dir);
  expect(get(db, "unsynced", nullptr), "a write completed before lt_close survives reopen");
  expect(get(db, "empty", &out) && out.empty(), "an empty value survives checkpoint and reopen");
  expect(scan_prefix(db, "B1000000/").empty() && scan_prefix(db, "B3000000/").empty(),
         "deleted rows stay deleted after checkpoint and reopen");
  expect(get(db, "big", &out) && out.size() == (16u << 20), "16 MiB value survives checkpoint and reopen");
  lt_close(db);
  printf("edges: %d failures\n", failures);
  return failures != 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3) return 2;
  std::string mode = argv[1];
  if (mode == "edges") return edges(argv[2]);
  if (mode == "load") return load(argv[2], atof(argv[3]), strtoull(argv[4], nullptr, 10));
  if (mode == "verify") return verify(argv[2], argc > 3 ? strtoull(argv[3], nullptr, 10) : 0);
  if (mode == "crash") return crash(argv[0], argv[2], atoi(argv[3]), strtoull(argv[4], nullptr, 10));
  if (mode == "fault") return fault(argv[0], argv[2], strtoull(argv[3], nullptr, 10), argv[4], argv[5]);
  return 2;
}
