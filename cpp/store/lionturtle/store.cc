#include "store.h"

#include <bit>
#include <cstring>

#include "lionturtle.h"

namespace celltnx::lionturtle {

namespace {

constexpr char kMeta = 0x00, kRows = 0x01;
constexpr std::string_view kLastKey("\x00last", 5);
static_assert(kLastKey[0] == kMeta);

// A batch past this many bytes of keys and values is split across writes.
// Each write is atomic and they land in order, so a crash still keeps a
// prefix of the commits.
constexpr size_t kMaxBatchBytes = 64 << 20;

Status Fail(const char *op) {
  return Error{.code = Code::kIo, .detail = std::string("lionturtle: ") + op + ": " + lt_last_error()};
}

void AppendBE64(std::string *b, uint64_t x) {
  for (int s = 56; s >= 0; s -= 8) b->push_back(char(x >> s));
}

uint64_t ParseBE64(const char *p) {
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) x = x << 8 | uint8_t(p[i]);
  return x;
}

std::string TablePrefix(uint64_t id) {
  std::string k(1, kRows);
  AppendBE64(&k, id);
  return k;
}

void AppendUvarint(std::string *b, uint64_t x) {
  while (x >= 0x80) {
    b->push_back(char(x | 0x80));
    x >>= 7;
  }
  b->push_back(char(x));
}

bool ParseUvarint(std::string_view *b, uint64_t *x) {
  *x = 0;
  for (int shift = 0; shift < 64 && !b->empty(); shift += 7) {
    uint8_t c = uint8_t(b->front());
    b->remove_prefix(1);
    *x |= uint64_t(c & 0x7f) << shift;
    if (c < 0x80) return true;
  }
  return false;
}

Status GetMeta(lt_db *db, std::string_view key, uint64_t *out) {
  lt_value *v = nullptr;
  if (lt_get(db, key.data(), key.size(), &v) != LT_OK) return Fail("get");
  if (v == nullptr) {
    *out = 0;
    return {};
  }
  lt_slice b = lt_value_bytes(v);
  bool ok = b.len == 8;
  if (ok) *out = ParseBE64(b.data);
  lt_value_free(v);
  if (!ok) return Error{.code = Code::kIo, .detail = "lionturtle: corrupt metadata"};
  return {};
}

}  // namespace

// Rows use Go's badger encoding: a uvarint column count, then per value a
// tag, with numbers as 8 big-endian bytes and strings as a uvarint length
// and the bytes.
enum : uint8_t { kTagNull, kTagInt, kTagUint, kTagFloat, kTagString };

void EncodeRow(Row row, std::string *out) {
  AppendUvarint(out, row.size());
  for (const Value &v : row) {
    switch (v.type) {
      case Type::kNull:
        out->push_back(char(kTagNull));
        break;
      case Type::kInt:
        out->push_back(char(kTagInt));
        AppendBE64(out, uint64_t(v.i));
        break;
      case Type::kUint:
        out->push_back(char(kTagUint));
        AppendBE64(out, v.u);
        break;
      case Type::kFloat:
        out->push_back(char(kTagFloat));
        AppendBE64(out, std::bit_cast<uint64_t>(v.f));
        break;
      case Type::kString:
        out->push_back(char(kTagString));
        AppendUvarint(out, v.len);
        out->append(v.str());
        break;
    }
  }
}

bool DecodeRow(std::string_view b, std::vector<Value> *row) {
  row->clear();
  uint64_t n;
  if (!ParseUvarint(&b, &n) || n > b.size()) return false;
  for (uint64_t i = 0; i < n; i++) {
    if (b.empty()) return false;
    uint8_t tag = uint8_t(b.front());
    b.remove_prefix(1);
    switch (tag) {
      case kTagNull:
        row->push_back(Value());
        break;
      case kTagInt:
      case kTagUint:
      case kTagFloat: {
        if (b.size() < 8) return false;
        uint64_t u = ParseBE64(b.data());
        b.remove_prefix(8);
        row->push_back(tag == kTagInt    ? Value::Int(int64_t(u))
                       : tag == kTagUint ? Value::Uint(u)
                                         : Value::Float(std::bit_cast<double>(u)));
        break;
      }
      case kTagString: {
        uint64_t len;
        if (!ParseUvarint(&b, &len) || len > b.size()) return false;
        row->push_back(Value::Str(b.substr(0, len)));
        b.remove_prefix(len);
        break;
      }
      default:
        return false;
    }
  }
  return b.empty();
}

Store::Store(lt_db *db) : db_(db), batch_(lt_batch_new()) {}

Store::~Store() {
  lt_batch_free(batch_);
  lt_close(db_);
}

Status Store::Open(const std::string &dir, std::unique_ptr<Store> *out) {
  lt_options o = lt_options_default();
  // Smaller memtables than the defaults: the engine keeps every row in
  // memory already, and these settings cut peak memory threefold at a
  // small cost in write amplification (PLAN-MYSQL.md 0D).
  o.checkpoint_distance = 8;
  o.cache_bytes = 64 << 20;
  // Commits sync the log; zero-filled segments make each sync an overwrite,
  // which ext4 makes durable in about half the time of an append.
  o.preallocate_log = true;
  lt_db *db = nullptr;
  if (lt_open(dir.c_str(), &o, &db) != LT_OK) return Fail("open");
  std::unique_ptr<Store> s(new Store(db));
  if (Status st = GetMeta(db, kLastKey, &s->last_); !st.ok()) return st;
  *out = std::move(s);
  return {};
}

Status Store::Write(std::span<const Commit> batch) {
  std::lock_guard g(mu_);
  size_t bytes = 0;
  auto flush = [&] {
    if (lt_write(db_, batch_, false) != LT_OK) return Fail("write");
    lt_batch_clear(batch_);
    bytes = 0;
    return Status();
  };
  lt_batch_clear(batch_);
  for (const Commit &c : batch) {
    for (const WriteRef &w : c.writes) {
      uint64_t id = size_t(w.table) < ids_.size() ? ids_[w.table] : 0;
      if (id == 0) continue;
      key_ = TablePrefix(id);
      key_.append(w.key);
      lt_status st;
      if (w.deleted()) {
        st = lt_batch_delete(batch_, key_.data(), key_.size());
      } else {
        val_.clear();
        EncodeRow(w.row(), &val_);
        st = lt_batch_put(batch_, key_.data(), key_.size(), val_.data(), val_.size());
        bytes += val_.size();
      }
      if (st != LT_OK) return Fail("batch");
      bytes += key_.size();
    }
    // Split only between commits, each write carrying its newest commit.
    if (bytes >= kMaxBatchBytes || &c == &batch.back()) {
      val_.clear();
      AppendBE64(&val_, c.ts);
      if (lt_batch_put(batch_, kLastKey.data(), kLastKey.size(), val_.data(), val_.size()) != LT_OK)
        return Fail("batch");
      if (Status st = flush(); !st.ok()) return st;
      last_ = c.ts;
    }
  }
  return {};
}

Status Store::Sync() {
  if (lt_sync(db_) != LT_OK) return Fail("sync");
  return {};
}

void Store::Bind(int tbl, uint64_t id) {
  std::lock_guard g(mu_);
  if (size_t(tbl) >= ids_.size()) ids_.resize(tbl + 1, 0);
  ids_[tbl] = id;
}

void Store::Unbind(int tbl) {
  std::lock_guard g(mu_);
  if (size_t(tbl) < ids_.size()) ids_[tbl] = 0;
}

Status Store::Load(uint64_t id, const DB::RowPut &put) {
  std::string prefix = TablePrefix(id);
  lt_iter *it = nullptr;
  if (lt_iter_prefix(db_, prefix.data(), prefix.size(), false, &it) != LT_OK) return Fail("iterate");
  std::vector<Value> row;
  Status st;
  for (;;) {
    bool has;
    lt_slice k, v;
    if (lt_iter_next(it, &has, &k, &v) != LT_OK) {
      st = Fail("iterate");
      break;
    }
    if (!has) break;
    std::string_view key(k.data, k.len), val(v.data, v.len);
    if (!DecodeRow(val, &row)) {
      st = Error{.code = Code::kIo, .detail = "lionturtle: corrupt row in table " + std::to_string(id)};
      break;
    }
    put(key.substr(prefix.size()), Row(row.data(), row.size()));
  }
  lt_iter_free(it);
  return st;
}

Status Store::Drop(uint64_t id) {
  std::lock_guard g(mu_);
  std::string prefix = TablePrefix(id);
  lt_iter *it = nullptr;
  if (lt_iter_prefix(db_, prefix.data(), prefix.size(), false, &it) != LT_OK) return Fail("iterate");
  lt_batch *b = lt_batch_new();
  Status st;
  size_t n = 0;
  for (;;) {
    bool has;
    lt_slice k;
    if (lt_iter_next(it, &has, &k, nullptr) != LT_OK) {
      st = Fail("iterate");
      break;
    }
    if (!has) break;
    if (lt_batch_delete(b, k.data, k.len) != LT_OK) {
      st = Fail("batch");
      break;
    }
    // Deleting is idempotent, so the batches need not be one write.
    if (++n % 65536 == 0) {
      if (lt_write(db_, b, false) != LT_OK) {
        st = Fail("write");
        break;
      }
      lt_batch_clear(b);
    }
  }
  lt_iter_free(it);
  if (st.ok() && lt_write(db_, b, true) != LT_OK) st = Fail("write");
  lt_batch_free(b);
  return st;
}

}  // namespace celltnx::lionturtle
