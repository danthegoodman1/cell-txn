// Loads plugin.so with dlopen, as mysqld loads an engine, and runs:
//   host PLUGIN crash DIR ROUNDS   kill -9 a writer repeatedly; check recovery
//   host PLUGIN child DIR START    (internal) commit from START, report syncs
//   host PLUGIN bench DIR N GROUP  N commits, one sync per GROUP commits
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static int (*st_open)(const char *);
static int (*st_commit)(uint64_t);
static int (*st_sync)();
static int (*st_recover)(uint64_t *);
static void (*st_close)();

template <class F>
static void load(void *h, F &f, const char *name) {
  f = reinterpret_cast<F>(dlsym(h, name));
  if (!f) {
    fprintf(stderr, "missing %s\n", name);
    exit(2);
  }
}

static void must(int rc) {
  if (rc != 0) exit(1);
}

// Commits forever from start, syncing after a random group of 1–32 commits
// and writing each acknowledged timestamp to stdout.
static int child(const char *dir, uint64_t start) {
  must(st_open(dir));
  std::mt19937_64 r(start);
  for (uint64_t ts = start;;) {
    uint64_t n = 1 + r() % 32;
    for (uint64_t i = 0; i < n; i++) must(st_commit(ts++));
    must(st_sync());
    uint64_t acked = ts - 1;
    if (write(1, &acked, 8) != 8) return 1;
  }
}

static int crash(const char *self, const char *plugin, const char *dir, int rounds) {
  uint64_t acked = 0;
  std::mt19937_64 r(42);
  for (int round = 0; round < rounds; round++) {
    must(st_open(dir));
    uint64_t last;
    must(st_recover(&last));
    st_close();
    if (last < acked) {
      fprintf(stderr, "round %d: recovered through %llu, but %llu was acknowledged\n", round,
              (unsigned long long)last, (unsigned long long)acked);
      return 1;
    }
    printf("round %d: recovered through %llu (acknowledged %llu)\n", round, (unsigned long long)last,
           (unsigned long long)acked);
    int p[2];
    if (pipe(p)) return 1;
    pid_t pid = fork();
    if (pid == 0) {
      dup2(p[1], 1);
      close(p[0]);
      std::string start = std::to_string(last + 1);
      execl(self, self, plugin, "child", dir, start.c_str(), (char *)nullptr);
      _exit(127);
    }
    close(p[1]);
    usleep(20000 + r() % 300000);
    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
    uint64_t v;
    while (read(p[0], &v, 8) == 8) acked = v;
    close(p[0]);
  }
  must(st_open(dir));
  uint64_t last;
  must(st_recover(&last));
  st_close();
  printf("final: recovered through %llu (acknowledged %llu)\n", (unsigned long long)last,
         (unsigned long long)acked);
  return last >= acked ? 0 : 1;
}

static int bench(const char *dir, uint64_t n, uint64_t group) {
  must(st_open(dir));
  uint64_t last;
  must(st_recover(&last));
  std::vector<double> us;
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t ts = last + 1; ts <= last + n;) {
    auto s = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < group; i++) must(st_commit(ts++));
    must(st_sync());
    us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - s).count());
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  st_close();
  std::sort(us.begin(), us.end());
  printf("group %llu: %.0f commits/s, %.0f syncs/s, write+sync p50 %.0f us, p99 %.0f us\n",
         (unsigned long long)group, n / secs, us.size() / secs, us[us.size() / 2], us[us.size() * 99 / 100]);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 2;
  }
  load(h, st_open, "st_open");
  load(h, st_commit, "st_commit");
  load(h, st_sync, "st_sync");
  load(h, st_recover, "st_recover");
  load(h, st_close, "st_close");
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::string mode = argv[2];
  if (mode == "child") return child(argv[3], strtoull(argv[4], nullptr, 10));
  if (mode == "crash") return crash(argv[0], argv[1], argv[3], atoi(argv[4]));
  if (mode == "bench") return bench(argv[3], strtoull(argv[4], nullptr, 10), strtoull(argv[5], nullptr, 10));
  return 2;
}
