// Stands in for the celltnx engine's store adapter: built as a shared object
// that statically links liblionturtle_c.a, as a MySQL plugin would.
#include <lionturtle.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#define API extern "C" __attribute__((visibility("default")))

static lt_db *db;
static lt_batch *batch;

static std::string be64(uint64_t v) {
  std::string s(8, '\0');
  for (int i = 7; i >= 0; i--, v >>= 8) s[i] = char(v & 0xff);
  return s;
}

static uint64_t from_be64(const char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | uint8_t(p[i]);
  return v;
}

static int fail(const char *what, lt_status st) {
  fprintf(stderr, "%s: status %d: %s\n", what, int(st), lt_last_error());
  return -1;
}

API int st_open(const char *dir) {
  lt_options o = lt_options_default();
  o.cache_bytes = 64 << 20;
  if (getenv("LT_CHI")) o.checkpoint_distance = strtoull(getenv("LT_CHI"), nullptr, 10);
  lt_status st = lt_open(dir, &o, &db);
  if (st != LT_OK) return fail("open", st);
  batch = lt_batch_new();
  return 0;
}

// One commit: two row images in different tables plus the commit timestamp,
// written atomically, as the engine's writer would.
API int st_commit(uint64_t ts) {
  lt_batch_clear(batch);
  std::string v(120, char('a' + ts % 26));
  memcpy(&v[0], be64(ts).data(), 8);
  std::string ka = "a/" + be64(ts), kb = "b/" + be64(ts), meta = "meta/ts", tsv = be64(ts);
  lt_status st;
  if ((st = lt_batch_put(batch, ka.data(), ka.size(), v.data(), v.size())) != LT_OK) return fail("put", st);
  if ((st = lt_batch_put(batch, kb.data(), kb.size(), v.data(), v.size())) != LT_OK) return fail("put", st);
  if ((st = lt_batch_put(batch, meta.data(), meta.size(), tsv.data(), tsv.size())) != LT_OK) return fail("put", st);
  if ((st = lt_write(db, batch, false)) != LT_OK) return fail("write", st);
  return 0;
}

API int st_sync() {
  lt_status st = lt_sync(db);
  return st == LT_OK ? 0 : fail("sync", st);
}

// Recovery check: meta/ts names the last commit, and each table holds
// exactly commits 1..ts, with intact values.
static int check_table(const char *prefix, uint64_t last) {
  lt_iter *it;
  lt_status st = lt_iter_prefix(db, prefix, 2, false, &it);
  if (st != LT_OK) return fail("iter", st);
  uint64_t want = 1;
  for (;;) {
    bool has;
    lt_slice k, v;
    if ((st = lt_iter_next(it, &has, &k, &v)) != LT_OK) return fail("next", st);
    if (!has) break;
    uint64_t got = from_be64(k.data + 2);
    if (k.len != 10 || got != want || v.len != 120 || from_be64(v.data) != got || v.data[8] != char('a' + got % 26)) {
      fprintf(stderr, "%s: expected commit %llu, found %llu (key %zu bytes, value %zu bytes)\n", prefix,
              (unsigned long long)want, (unsigned long long)got, k.len, v.len);
      lt_iter_free(it);
      return -1;
    }
    want++;
  }
  lt_iter_free(it);
  if (want - 1 != last) {
    fprintf(stderr, "%s: holds commits 1..%llu, meta says %llu\n", prefix, (unsigned long long)(want - 1),
            (unsigned long long)last);
    return -1;
  }
  return 0;
}

API int st_recover(uint64_t *last) {
  lt_value *v;
  lt_status st = lt_get(db, "meta/ts", 7, &v);
  if (st != LT_OK) return fail("get", st);
  *last = 0;
  if (v) {
    lt_slice s = lt_value_bytes(v);
    *last = from_be64(s.data);
    lt_value_free(v);
  }
  if (check_table("a/", *last) || check_table("b/", *last)) return -1;
  return 0;
}

API void st_close() {
  lt_batch_free(batch);
  lt_close(db);
}
