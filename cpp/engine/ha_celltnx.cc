#include "ha_celltnx.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "keys.h"
#include "sql/dd/types/table.h"
#include "sql/mysqld.h"
#include "store.h"
#include "my_bitmap.h"
#include "my_byteorder.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/key.h"
#include "sql/sql_check_constraint.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_insert.h"
#include "sql/sql_update.h"
#include "sql/table.h"

namespace celltnx::engine {

// --- columns and keys ---

// Rows hold semantic values, so the core can derive index keys and check
// constraints without MySQL's Field objects: integers and floats as
// numbers, character columns as their bytes, everything else as its packed
// image.
enum class ColKind : uint8_t { kInt, kUint, kFloat, kString, kBytes };

struct ColSpec {
  ColKind kind = ColKind::kBytes;
  const CHARSET_INFO *cs = nullptr;  // kString
  uint32_t chars = 0;                // kString: length in characters
  bool pad = false;                  // kString: a PAD SPACE collation
  size_t weights = 0;                // kString: weight buffer size
};

struct KeySpec {
  std::string name;
  std::vector<int> cols;
  bool unique = false;
  bool primary = false;
  int core_index = -1;  // position among the core's secondary indexes
};

struct TableInfo {
  int id = -1;
  uint64_t durable = 0;       // the store's ID for the table, its data-dictionary ID; 0 for memory only
  std::vector<ColSpec> cols;
  std::vector<KeySpec> keys;  // by MySQL key number
  int pk = -1;                // MySQL key number of the primary key; -1 for a hidden key
  ColMask all = 0;
  ColMask indexed = 0;        // columns in any key; they never take deltas
  uint ref_len = 0;
  // CHECK constraints are translated on first use, when a TABLE with
  // resolved expressions is at hand. final holds the columns of the ones
  // that could not be translated: the server's statement-time check is all
  // they get, so they never take deltas.
  mutable std::mutex check_mu;
  mutable bool checks_done = false;
  mutable ColMask final = 0;
};

static ColSpec Classify(Field *f) {
  ColSpec c;
  switch (f->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      c.kind = f->is_flag_set(UNSIGNED_FLAG) ? ColKind::kUint : ColKind::kInt;
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      c.kind = ColKind::kFloat;
      break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
      c.kind = ColKind::kString;
      c.cs = f->charset();
      c.chars = f->char_length();
      c.pad = c.cs->pad_attribute != NO_PAD;
      c.weights = c.cs->coll->strnxfrmlen(c.cs, f->field_length) + 16;
      break;
    default:
      c.kind = ColKind::kBytes;
      break;
  }
  return c;
}

// KeyableBytes reports whether a packed column image sorts as its value
// does, so it can be a key part as plain bytes.
static bool KeyableBytes(Field *f) {
  switch (f->real_type()) {
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_TIME2:
      return true;
    default:
      return false;
  }
}

// EncodeValue appends an order-preserving, prefix-free encoding of v.
static void EncodeValue(const ColSpec &c, const Value &v, std::string *out) {
  if (v.null()) {
    keys::AppendNull(*out);
    return;
  }
  switch (c.kind) {
    case ColKind::kInt:
      keys::AppendInt(*out, v.i);
      return;
    case ColKind::kUint:
      keys::AppendUint(*out, v.u);
      return;
    case ColKind::kFloat:
      keys::AppendFloat(*out, v.f);
      return;
    case ColKind::kString: {
      thread_local std::string w;
      w.resize(c.weights);
      size_t n = c.cs->coll->strnxfrm(c.cs, reinterpret_cast<uint8_t *>(w.data()), w.size(), c.chars,
                                      reinterpret_cast<const uint8_t *>(v.s), v.len,
                                      c.pad ? MY_STRXFRM_PAD_TO_MAXLEN : 0);
      keys::AppendString(*out, std::string_view(w.data(), n));
      return;
    }
    case ColKind::kBytes:
      keys::AppendString(*out, v.str());
      return;
  }
}

static size_t MaxEncoded(const ColSpec &c, Field *f) {
  switch (c.kind) {
    case ColKind::kString:
      return 3 + 2 * c.weights;
    case ColKind::kBytes:
      return 3 + 2 * size_t(f->pack_length());
    default:
      return 9;
  }
}

// LeadingFixed is the width of a key's leading fixed-width parts, the
// bucket prefix: a composite key of integers splits on its last one.
static int LeadingFixed(const TableInfo &t, const KeySpec &k) {
  int n = 0;
  for (int c : k.cols) {
    ColKind kind = t.cols[c].kind;
    if (kind != ColKind::kInt && kind != ColKind::kUint && kind != ColKind::kFloat) break;
    n += 9;
  }
  return n;
}

static std::shared_ptr<TableInfo> BuildInfo(TABLE *t, std::string *err) {
  auto info = std::make_shared<TableInfo>();
  uint n = t->s->fields;
  if (n > uint(kMaxCols)) {
    *err = "celltnx tables have at most 63 columns";
    return nullptr;
  }
  for (uint i = 0; i < n; i++) info->cols.push_back(Classify(t->field[i]));
  info->all = Col(int(n)) - 1;
  info->pk = t->s->primary_key == MAX_KEY ? -1 : int(t->s->primary_key);
  int core = 0;
  size_t pk_len = 8;
  for (uint k = 0; k < t->s->keys; k++) {
    KEY &key = t->key_info[k];
    KeySpec ks;
    ks.name = key.name;
    ks.primary = int(k) == info->pk;
    ks.unique = (key.flags & HA_NOSAME) != 0;
    size_t len = 0;
    for (uint j = 0; j < key.user_defined_key_parts; j++) {
      KEY_PART_INFO &kp = key.key_part[j];
      if (kp.key_part_flag & HA_PART_KEY_SEG) {
        *err = "celltnx does not support prefix indexes";
        return nullptr;
      }
      int col = int(kp.field->field_index());
      const ColSpec &c = info->cols[col];
      if (c.kind == ColKind::kBytes && !KeyableBytes(kp.field)) {
        *err = std::string("celltnx cannot index column ") + kp.field->field_name;
        return nullptr;
      }
      ks.cols.push_back(col);
      info->indexed |= Col(col);
      len += MaxEncoded(c, kp.field);
    }
    if (ks.primary)
      pk_len = len;
    else
      ks.core_index = core++;
    info->keys.push_back(std::move(ks));
  }
  info->ref_len = uint(2 + pk_len);
  return info;
}

static Schema BuildSchema(TABLE *t, std::shared_ptr<const TableInfo> info, const char *name) {
  Schema s;
  s.name = name;
  for (uint i = 0; i < t->s->fields; i++) s.cols.emplace_back(t->field[i]->field_name);
  if (info->pk >= 0) s.bucket_prefix = LeadingFixed(*info, info->keys[info->pk]);
  for (const KeySpec &k : info->keys) {
    if (k.primary) continue;
    Index ix;
    ix.name = k.name;
    for (int c : k.cols) ix.cols |= Col(c);
    ix.unique = k.unique;
    ix.bucket_prefix = LeadingFixed(*info, k);
    const KeySpec *ks = &k;
    // A unique key with a NULL part claims nothing, as in MySQL.
    ix.key = [info, ks](Row r, std::string *out) {
      bool unique = true;
      for (int c : ks->cols) {
        if (r[c].null()) unique = false;
        EncodeValue(info->cols[c], r[c], out);
      }
      return unique;
    };
    s.indexes.push_back(std::move(ix));
  }
  return s;
}

// --- CHECK constraints ---

// Pred is a CHECK expression the core evaluates on merged rows: numeric
// comparisons between a column and a constant, AND, OR, NOT and NULL
// tests. Evaluation is three-valued, and only FALSE violates, as in SQL.
struct Pred {
  enum Op : uint8_t { kCmp, kAnd, kOr, kNot, kIsNull, kIsNotNull } op = kCmp;
  Item_func::Functype cmp = Item_func::EQ_FUNC;
  int col = -1;
  long double num = 0;
  std::vector<Pred> kids;
};

enum Truth : int8_t { kFalse = 0, kTrue = 1, kUnknown = -1 };

static Truth Eval(const Pred &p, Row r) {
  switch (p.op) {
    case Pred::kCmp: {
      const Value &v = r[p.col];
      if (v.null()) return kUnknown;
      long double x = v.type == Type::kInt ? (long double)v.i : v.type == Type::kUint ? (long double)v.u : v.f;
      bool ok = false;
      switch (p.cmp) {
        case Item_func::EQ_FUNC: ok = x == p.num; break;
        case Item_func::NE_FUNC: ok = x != p.num; break;
        case Item_func::LT_FUNC: ok = x < p.num; break;
        case Item_func::LE_FUNC: ok = x <= p.num; break;
        case Item_func::GT_FUNC: ok = x > p.num; break;
        case Item_func::GE_FUNC: ok = x >= p.num; break;
        default: break;
      }
      return ok ? kTrue : kFalse;
    }
    case Pred::kAnd: {
      Truth t = kTrue;
      for (const Pred &k : p.kids) {
        Truth e = Eval(k, r);
        if (e == kFalse) return kFalse;
        if (e == kUnknown) t = kUnknown;
      }
      return t;
    }
    case Pred::kOr: {
      Truth t = kFalse;
      for (const Pred &k : p.kids) {
        Truth e = Eval(k, r);
        if (e == kTrue) return kTrue;
        if (e == kUnknown) t = kUnknown;
      }
      return t;
    }
    case Pred::kNot: {
      Truth e = Eval(p.kids[0], r);
      return e == kUnknown ? kUnknown : e == kTrue ? kFalse : kTrue;
    }
    case Pred::kIsNull:
      return r[p.col].null() ? kTrue : kFalse;
    case Pred::kIsNotNull:
      return r[p.col].null() ? kFalse : kTrue;
  }
  return kUnknown;
}

static bool Numeric(const ColSpec &c) {
  return c.kind == ColKind::kInt || c.kind == ColKind::kUint || c.kind == ColKind::kFloat;
}

// ColumnOf returns the column an item reads, or -1 unless it is a numeric
// column of t.
static int ColumnOf(Item *it, TABLE *t, const TableInfo &info) {
  it = it->real_item();
  if (it->type() != Item::FIELD_ITEM) return -1;
  Field *f = down_cast<Item_field *>(it)->field;
  if (f == nullptr || f->table != t) return -1;
  int col = int(f->field_index());
  return Numeric(info.cols[col]) ? col : -1;
}

static bool NumericConst(Item *it) {
  if (!it->const_item()) return false;
  Item_result r = it->result_type();
  return r == INT_RESULT || r == REAL_RESULT || r == DECIMAL_RESULT;
}

static long double ConstValue(Item *it) {
  if (it->result_type() == INT_RESULT)
    return it->unsigned_flag ? (long double)(ulonglong)it->val_int() : (long double)it->val_int();
  return it->val_real();
}

static Item_func::Functype Flip(Item_func::Functype f) {
  switch (f) {
    case Item_func::LT_FUNC: return Item_func::GT_FUNC;
    case Item_func::LE_FUNC: return Item_func::GE_FUNC;
    case Item_func::GT_FUNC: return Item_func::LT_FUNC;
    case Item_func::GE_FUNC: return Item_func::LE_FUNC;
    default: return f;
  }
}

static bool Translate(Item *it, TABLE *t, const TableInfo &info, Pred *out, ColMask *cols) {
  it = it->real_item();
  if (it->type() != Item::FUNC_ITEM && it->type() != Item::COND_ITEM) return false;
  auto *f = down_cast<Item_func *>(it);
  switch (f->functype()) {
    case Item_func::EQ_FUNC:
    case Item_func::NE_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::GE_FUNC: {
      if (f->argument_count() != 2) return false;
      Item *a = f->arguments()[0], *b = f->arguments()[1];
      int col = ColumnOf(a, t, info);
      Item_func::Functype op = f->functype();
      if (col < 0) {
        std::swap(a, b);
        col = ColumnOf(a, t, info);
        op = Flip(op);
      }
      if (col < 0 || !NumericConst(b)) return false;
      out->op = Pred::kCmp;
      out->cmp = op;
      out->col = col;
      out->num = ConstValue(b);
      *cols |= Col(col);
      return true;
    }
    case Item_func::COND_AND_FUNC:
    case Item_func::COND_OR_FUNC: {
      out->op = f->functype() == Item_func::COND_AND_FUNC ? Pred::kAnd : Pred::kOr;
      List_iterator<Item> li(*down_cast<Item_cond *>(f)->argument_list());
      while (Item *arg = li++) {
        out->kids.emplace_back();
        if (!Translate(arg, t, info, &out->kids.back(), cols)) return false;
      }
      return true;
    }
    case Item_func::NOT_FUNC:
      out->op = Pred::kNot;
      out->kids.emplace_back();
      return f->argument_count() == 1 && Translate(f->arguments()[0], t, info, &out->kids.back(), cols);
    case Item_func::ISNULL_FUNC:
    case Item_func::ISNOTNULL_FUNC: {
      int col = ColumnOf(f->arguments()[0], t, info);
      if (col < 0) return false;
      out->op = f->functype() == Item_func::ISNULL_FUNC ? Pred::kIsNull : Pred::kIsNotNull;
      out->col = col;
      *cols |= Col(col);
      return true;
    }
    default:
      return false;
  }
}

// ReferencedCols returns the columns of t an item tree reads.
static ColMask ReferencedCols(Item *it, TABLE *t) {
  ColMask m = 0;
  WalkItem(it, enum_walk::SUBQUERY_PREFIX, [&](Item *x) {
    if (x->type() == Item::FIELD_ITEM) {
      Field *f = down_cast<Item_field *>(x)->field;
      if (f != nullptr && f->table == t) m |= Col(int(f->field_index()));
    }
    return false;
  });
  return m;
}

// --- the engine ---

namespace {

handlerton *hton = nullptr;
ulong mode_var = 2;
uint bucket_bits_var = 0;
uint lock_wait_ms_var = 1000;
bool durable_var = true;
bool sync_var = true;
uint commit_delay_us_var = 0;
uint group_wait_us_var = 500;
const char *mode_names[] = {"row", "cell", "cell+delta", NullS};
TYPELIB mode_typelib = {array_elements(mode_names) - 1, "", mode_names, nullptr};

// RowLocks queues transactions that read a row with SELECT ... FOR UPDATE,
// so a hot read-modify-write waits for the previous one to commit instead
// of failing validation and rerunning. The locks are advisory: commit-time
// validation alone decides serializability, so a wait that times out costs
// at most a conflict. Only a transaction holding no lock waits, so no wait
// can close a cycle.
class RowLocks {
 public:
  enum class Got { kHeld, kFree, kWaited, kBusy, kUnused };

  // Acquire takes key for owner, which uses cols of the row, or reports
  // kHeld if owner has it. When another owner holds the key it waits, if
  // wait is set, until the key is free, the timeout passes, or thd is
  // killed. With if_used, it takes nothing and reports kUnused unless the
  // key's holder or waiters use one of cols.
  Got Acquire(THD *thd, const std::string &key, const void *owner, ColMask cols, bool wait,
              std::chrono::milliseconds timeout, bool if_used = false) {
    Shard &sh = shards_[std::hash<std::string>{}(key) % kShards];
    std::unique_lock g(sh.mu);
    if (if_used) {
      auto it = sh.map.find(key);
      if (it == sh.map.end() || (it->second.cols & cols) == 0) return Got::kUnused;
    }
    Entry &e = sh.map[key];
    e.cols |= cols;
    if (e.owner == owner) return Got::kHeld;
    if (e.owner == nullptr) {
      e.owner = owner;
      return Got::kFree;
    }
    if (!wait || timeout.count() == 0) return Got::kBusy;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    e.waiters++;
    while (e.owner != nullptr && std::chrono::steady_clock::now() < deadline && !thd_killed(thd))
      sh.cv.wait_until(g, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
    e.waiters--;
    if (e.owner != nullptr) return Got::kBusy;
    e.owner = owner;
    return Got::kWaited;
  }

  void Release(const std::string &key) {
    Shard &sh = shards_[std::hash<std::string>{}(key) % kShards];
    std::lock_guard g(sh.mu);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return;
    if (it->second.waiters == 0) {
      sh.map.erase(it);
      return;
    }
    it->second.owner = nullptr;
    sh.cv.notify_all();
  }

 private:
  static constexpr size_t kShards = 64;
  struct Entry {
    const void *owner = nullptr;
    // The columns the holders and waiters since the entry's creation read
    // or assign; it resets when the key falls idle.
    ColMask cols = 0;
    int waiters = 0;
  };
  struct Shard {
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, Entry> map;  // node-based: entries stay put while waiters hold them
  };
  Shard shards_[kShards];
};

// Counters back the global status variables.
struct Counters {
  std::atomic<uint64_t> commits{0}, conflicts{0}, bucket_conflicts{0}, check_failures{0};
  std::atomic<uint64_t> lock_waits{0}, lock_timeouts{0}, refreshes{0}, refresh_conflicts{0};
  std::atomic<uint64_t> commit_lock_wait_ns{0}, commit_lock_hold_ns{0};
};
Counters counters;

struct Engine {
  std::unique_ptr<lionturtle::Store> store;  // with celltnx_durable
  std::unique_ptr<DB> db;
  std::thread writer;
  std::mutex mu;  // guards tables
  std::map<std::string, std::shared_ptr<TableInfo>> tables;
  RowLocks locks;
};
Engine *engine = nullptr;

}  // namespace

// Trx is a connection's state: its open transaction, and the statement
// savepoint that a failed statement rolls back to.
struct Trx {
  std::unique_ptr<Txn> txn;
  std::unique_ptr<Txn> spare;  // the last finished transaction, for Begin to reuse
  int in_use = 0;  // handlers locked by the running statement
  int stmt_sp = 0;
  // Rows the running statement read while assigning deltas, with the delta
  // columns. If it fails, the error may rest on their untracked base values,
  // so those become tracked reads.
  struct Promotion {
    int tbl;
    std::string pk;
    ColMask cols;
  };
  std::vector<Promotion> promote;
  std::vector<std::string> locks;  // RowLocks keys held until the transaction ends
  // The last transaction's serialization point and snapshot, for status
  // variables that other threads may read.
  std::atomic<uint64_t> last_commit_ts{0}, last_read_ts{0};

  void ReleaseLocks() {
    for (const std::string &k : locks) engine->locks.Release(k);
    locks.clear();
  }
};

static Trx *GetTrx(THD *thd) {
  auto *t = static_cast<Trx *>(thd_get_ha_data(thd, hton));
  if (t == nullptr) {
    t = new Trx;
    thd_set_ha_data(thd, hton, t);
  }
  return t;
}

static bool InTransaction(THD *thd) { return thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN); }

static void StartStatement(THD *thd, Trx *trx) {
  if (!trx->txn) trx->txn = engine->db->Begin(std::move(trx->spare));
  trx->promote.clear();
  trx->stmt_sp = trx->txn->Savepoint();
  trx->txn->BeginStatement();
  trans_register_ha(thd, false, hton, nullptr);
  if (InTransaction(thd)) trans_register_ha(thd, true, hton, nullptr);
}

// ReportCommitError raises the MySQL error for a failed commit. MySQL
// keeps the first error a statement raises, so the client sees this one
// rather than the generic commit failure the server adds after it.
static int ReportCommitError(const Status &st) {
  switch (st.code()) {
    case Code::kConflict:
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      return HA_ERR_LOCK_DEADLOCK;
    case Code::kConstraint: {
      const std::string &c = st.error().check;
      if (c == "txn: integer overflow" || c == "txn: delta on a non-integer value")
        my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "BIGINT", "a delta at commit");
      else if (c == "row size limit")
        my_error(ER_TOO_BIG_ROWSIZE, MYF(0), long(16 << 20));
      else
        my_error(ER_CHECK_CONSTRAINT_VIOLATED, MYF(0), c.c_str());
      return HA_ERR_GENERIC;
    }
    default:
      my_printf_error(ER_ERROR_DURING_COMMIT, "celltnx: %s", MYF(0), st.Message().c_str());
      return HA_ERR_GENERIC;
  }
}

static int Commit(handlerton *, THD *thd, bool all) {
  auto *trx = static_cast<Trx *>(thd_get_ha_data(thd, hton));
  if (trx == nullptr || !trx->txn) return 0;
  if (!all && InTransaction(thd)) return 0;  // a statement ends inside a transaction
  // Waiters may go once the commit is visible; they need not wait for it
  // to be durable, since their own commits queue behind it.
  Status st;
  try {
    st = trx->txn->Install();
    trx->ReleaseLocks();
    if (st.ok()) st = trx->txn->WaitDurable();
  } catch (const std::exception &e) {
    trx->ReleaseLocks();
    trx->txn.reset();
    my_printf_error(ER_ERROR_DURING_COMMIT, "celltnx: %s", MYF(0), e.what());
    return HA_ERR_GENERIC;
  }
  if (st.ok()) {
    counters.commits.fetch_add(1, std::memory_order_relaxed);
    trx->last_commit_ts.store(trx->txn->SerialTs(), std::memory_order_relaxed);
    trx->last_read_ts.store(trx->txn->ReadTs(), std::memory_order_relaxed);
  } else if (st.code() == Code::kConflict) {
    counters.conflicts.fetch_add(1, std::memory_order_relaxed);
    if (st.error().bucket) counters.bucket_conflicts.fetch_add(1, std::memory_order_relaxed);
  } else if (st.code() == Code::kConstraint) {
    counters.check_failures.fetch_add(1, std::memory_order_relaxed);
  }
  trx->spare = std::move(trx->txn);
  return st.ok() ? 0 : ReportCommitError(st);
}

static int Rollback(handlerton *, THD *thd, bool all) {
  auto *trx = static_cast<Trx *>(thd_get_ha_data(thd, hton));
  if (trx == nullptr || !trx->txn) return 0;
  if (all || !InTransaction(thd)) {
    trx->txn->Abort();
    trx->ReleaseLocks();
    trx->spare = std::move(trx->txn);
    return 0;
  }
  trx->txn->RollbackTo(trx->stmt_sp);
  std::vector<Value> row;
  bool found;
  for (const Trx::Promotion &p : trx->promote) (void)trx->txn->GetRow(p.tbl, p.pk, p.cols, &row, &found);
  trx->promote.clear();
  return 0;
}

static int CloseConnection(handlerton *, THD *thd) {
  auto *trx = static_cast<Trx *>(thd_get_ha_data(thd, hton));
  if (trx != nullptr) trx->ReleaseLocks();
  delete trx;
  thd_set_ha_data(thd, hton, nullptr);
  return 0;
}

static int SavepointSet(handlerton *, THD *thd, void *sv) {
  Trx *trx = GetTrx(thd);
  *static_cast<int *>(sv) = trx->txn ? trx->txn->Savepoint() : 0;
  return 0;
}

static int SavepointRollback(handlerton *, THD *thd, void *sv) {
  Trx *trx = GetTrx(thd);
  if (trx->txn) trx->txn->RollbackTo(*static_cast<int *>(sv));
  return 0;
}

static int SavepointRelease(handlerton *, THD *, void *) { return 0; }

static int StartConsistentSnapshot(handlerton *, THD *thd) {
  Trx *trx = GetTrx(thd);
  if (!trx->txn) trx->txn = engine->db->Begin(std::move(trx->spare));
  trans_register_ha(thd, true, hton, nullptr);
  return 0;
}

static handler *CreateHandler(handlerton *h, TABLE_SHARE *share, bool, MEM_ROOT *mem) {
  return new (mem) ha_celltnx(h, share);
}

static int Init(void *p) {
  hton = static_cast<handlerton *>(p);
  hton->state = SHOW_OPTION_YES;
  hton->db_type = DB_TYPE_UNKNOWN;
  hton->create = CreateHandler;
  hton->flags = HTON_CAN_RECREATE;
  hton->commit = Commit;
  hton->rollback = Rollback;
  hton->close_connection = CloseConnection;
  hton->savepoint_offset = sizeof(int);
  hton->savepoint_set = SavepointSet;
  hton->savepoint_rollback = SavepointRollback;
  hton->savepoint_release = SavepointRelease;
  hton->start_consistent_snapshot = StartConsistentSnapshot;
  engine = new Engine;
  Options o;
  o.mode = Mode(mode_var);
  o.bucket_bits = bucket_bits_var;
  o.max_key_bytes = 65535;
  o.max_row_bytes = 16 << 20;
  if (durable_var) {
    std::string dir = std::string(mysql_real_data_home) + "celltnx";
    if (Status st = lionturtle::Store::Open(dir, &engine->store); !st.ok()) {
      fprintf(stderr, "celltnx: cannot open the store in %s: %s\n", dir.c_str(), st.Message().c_str());
      delete engine;
      engine = nullptr;
      return 1;
    }
    o.store = engine->store.get();
    o.no_sync = !sync_var;
    o.commit_delay = [] {
      if (uint us = commit_delay_us_var) std::this_thread::sleep_for(std::chrono::microseconds(us));
    };
    // Clients acknowledged by one sync commit again moments later, just
    // after the next sync has begun without them, so they wait through two
    // syncs while a second group fills the gap. Waiting briefly for them
    // merges the groups, and each commit waits about one sync. The wait
    // pays only while it is shorter than the sync it saves, so it lasts no
    // longer than the previous flush took: the full celltnx_group_wait_us
    // on a drive that empties a volatile cache on each sync (milliseconds),
    // and next to nothing on one with power-loss protection (tens of
    // microseconds). Clients with long transactions never return in time;
    // after two waits in a row in which fewer than half of them came back,
    // the writer stops waiting for a while, then tries again.
    o.gather = [](size_t last, int64_t flush_ns, const std::function<size_t()> &queued) {
      static int misses = 0, skip = 0;  // the writer thread's alone
      int64_t ns = std::min(int64_t(group_wait_us_var) * 1000, flush_ns);
      if (ns <= 0 || last == 0) return;
      if (skip > 0) {
        skip--;
        return;
      }
      size_t start = queued(), target = start + last;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
      while (queued() < target && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      if (queued() - start >= std::max<size_t>(1, last / 2)) {
        misses = 0;
      } else if (++misses == 2) {
        misses = 0;
        skip = 64;
      }
    };
  }
  o.now = [] {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
  };
  o.on_commit = [](int64_t wait, int64_t hold) {
    counters.commit_lock_wait_ns.fetch_add(uint64_t(wait), std::memory_order_relaxed);
    counters.commit_lock_hold_ns.fetch_add(uint64_t(hold), std::memory_order_relaxed);
  };
  engine->db = std::make_unique<DB>(std::move(o), std::vector<Schema>{});
  if (engine->store) {
    // Tables load their rows when first opened; this restores the clock.
    (void)engine->db->Recover(engine->store->Last(), [](const DB::Put &) { return Status(); });
    engine->writer = std::thread([] {
      if (Status st = engine->db->RunWriter(); !st.ok())
        fprintf(stderr, "celltnx: the store failed, and commits now fail: %s\n", st.Message().c_str());
    });
  }
  return 0;
}

static int Deinit(void *) {
  if (engine == nullptr) return 0;
  if (engine->store) {
    engine->db->Close();
    engine->writer.join();
    (void)engine->db->Flush();  // commits queued as the writer stopped
  }
  engine->db.reset();
  delete engine;
  engine = nullptr;
  return 0;
}

// --- the handler ---

ha_celltnx::ha_celltnx(handlerton *h, TABLE_SHARE *share) : handler(h, share) {}
ha_celltnx::~ha_celltnx() = default;

handler::Table_flags ha_celltnx::table_flags() const {
  return HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE | HA_NULL_IN_KEY |
         HA_PRIMARY_KEY_REQUIRED_FOR_POSITION | HA_PRIMARY_KEY_IN_READ_INDEX | HA_NO_PREFIX_CHAR_KEYS;
}

ulong ha_celltnx::index_flags(uint, uint, bool) const {
  return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE;
}

Txn *ha_celltnx::txn() {
  Trx *trx = GetTrx(ha_thd());
  if (!trx->txn) trx->txn = engine->db->Begin(std::move(trx->spare));
  return trx->txn.get();
}

int ha_celltnx::Fail(const Status &st, const char *what) {
  switch (st.code()) {
    case Code::kDuplicateKey: {
      const std::string &ix = st.error().index;
      dup_key_ = info_->pk >= 0 ? uint(info_->pk) : MAX_KEY;
      if (ix != "PRIMARY")
        for (size_t k = 0; k < info_->keys.size(); k++)
          if (info_->keys[k].name == ix) dup_key_ = uint(k);
      errkey = dup_key_;
      return HA_ERR_FOUND_DUPP_KEY;
    }
    case Code::kNotFound:
      return HA_ERR_KEY_NOT_FOUND;
    case Code::kTooLarge:
      return HA_ERR_TOO_BIG_ROW;
    default:
      my_printf_error(ER_INTERNAL_ERROR, "celltnx %s: %s", MYF(0), what, st.Message().c_str());
      return HA_ERR_INTERNAL_ERROR;
  }
}

// ReadField returns the value of f in the record at buf. A string or packed
// image is copied into *scratch.
static Value ReadField(Field *f, const ColSpec &c, const uchar *buf, TABLE *table, std::string *scratch) {
  ptrdiff_t off = buf - table->record[0];
  f->move_field_offset(off);
  Value v;
  if (!f->is_null()) {
    switch (c.kind) {
      case ColKind::kInt:
        v = Value::Int(f->val_int());
        break;
      case ColKind::kUint:
        v = Value::Uint(uint64_t(f->val_int()));
        break;
      case ColKind::kFloat:
        v = Value::Float(f->val_real());
        break;
      case ColKind::kString: {
        String tmp;
        String *s = f->val_str(&tmp);
        scratch->assign(s->ptr(), s->length());
        v = Value::Str(*scratch);
        break;
      }
      case ColKind::kBytes: {
        size_t cap = std::max<size_t>(f->pack_length(), f->data_length()) + 16;
        scratch->resize(cap);
        auto *start = reinterpret_cast<uchar *>(scratch->data());
        uchar *end = f->pack(start, f->field_ptr(), cap);
        scratch->resize(size_t(end - start));
        v = Value::Str(*scratch);
        break;
      }
    }
  }
  f->move_field_offset(-off);
  return v;
}

static void WriteField(Field *f, const ColSpec &c, const Value &v, uchar *buf, TABLE *table) {
  ptrdiff_t off = buf - table->record[0];
  f->move_field_offset(off);
  if (v.null()) {
    f->set_null();
  } else {
    f->set_notnull();
    switch (c.kind) {
      case ColKind::kInt:
        f->store(longlong(v.i), false);
        break;
      case ColKind::kUint:
        f->store(longlong(v.u), true);
        break;
      case ColKind::kFloat:
        f->store(v.f);
        break;
      case ColKind::kString:
        f->store(v.s, v.len, c.cs);
        break;
      case ColKind::kBytes:
        f->unpack(f->field_ptr(), reinterpret_cast<const uchar *>(v.s), 0);
        break;
    }
  }
  f->move_field_offset(-off);
}

std::string ha_celltnx::EncodePK(const uchar *record) {
  std::string pk, scratch;
  for (int c : info_->keys[info_->pk].cols)
    EncodeValue(info_->cols[c], ReadField(table->field[c], info_->cols[c], record, table, &scratch), &pk);
  return pk;
}

std::string ha_celltnx::EncodeKey(uint keynr, const uchar *key, key_part_map map) {
  KEY &k = table->key_info[keynr];
  uint len = calculate_key_len(table, keynr, map);
  key_restore(key_buf_.data(), key, &k, len);
  std::string out, scratch;
  const KeySpec &ks = info_->keys[keynr];
  for (size_t j = 0; j < ks.cols.size() && (map & (key_part_map(1) << j)); j++) {
    int c = ks.cols[j];
    EncodeValue(info_->cols[c], ReadField(table->field[c], info_->cols[c], key_buf_.data(), table, &scratch), &out);
  }
  return out;
}

int ha_celltnx::Load(const Rec &rec, uchar *buf) {
  memcpy(buf, table->s->default_values, table->s->null_bytes);
  for (size_t i = 0; i < info_->cols.size(); i++) WriteField(table->field[i], info_->cols[i], rec.row[i], buf, table);
  cur_pk_ = rec.pk;
  if (delta_ != 0) GetTrx(ha_thd())->promote.push_back({info_->id, cur_pk_, delta_});
  return 0;
}

void ha_celltnx::Prepare() {
  THD *thd = ha_thd();
  if (stmt_id_ == thd->query_id) return;
  stmt_id_ = thd->query_id;
  InstallChecks();
  delta_ = 0;
  const Table_ref *tl = table->pos_in_table_list;
  for_update_ = thd_sql_command(thd) == SQLCOM_SELECT && table->reginfo.lock_type == TL_WRITE &&
                (tl == nullptr || tl->lock_descriptor().action == THR_WAIT);
  ColMask read = 0, write = 0;
  for (size_t i = 0; i < info_->cols.size(); i++) {
    if (bitmap_is_set(table->read_set, uint(i))) read |= Col(int(i));
    if (bitmap_is_set(table->write_set, uint(i))) write |= Col(int(i));
  }
  // An INSERT's write_set holds every inserted column, but an ON DUPLICATE
  // KEY UPDATE changes only the columns its clause assigns.
  assigned_ = write;
  if (auto *cmd = dynamic_cast<Sql_cmd_insert_base *>(thd->lex->m_sql_cmd);
      cmd != nullptr && cmd->duplicates == DUP_UPDATE) {
    assigned_ = 0;
    for (Item *fi : VisibleFields(cmd->update_field_list))
      if (Item *f = fi->real_item(); f->type() == Item::FIELD_ITEM && down_cast<Item_field *>(f)->field != nullptr &&
                                     down_cast<Item_field *>(f)->field->table == table)
        assigned_ |= Col(int(down_cast<Item_field *>(f)->field->field_index()));
      else
        assigned_ = write;  // a shape this cannot read: write what MySQL says
  }
  if (engine->db->mode() == Mode::kRow) {
    track_ = info_->all;
    return;
  }
  if (engine->db->mode() == Mode::kCellDelta) delta_ = ClassifyDeltas(thd);
  // MySQL skips an update whose new row equals the old one, comparing the
  // assigned columns' old values, so those count as read.
  track_ = (read | assigned_) & ~delta_;
}

bool ha_celltnx::IsDelta(int col, Item *val) const {
  ColKind kind = info_->cols[col].kind;
  if (kind != ColKind::kInt && kind != ColKind::kUint) return false;
  val = val->real_item();
  if (val->type() != Item::FUNC_ITEM) return false;
  auto *f = down_cast<Item_func *>(val);
  if (f->argument_count() != 2) return false;
  auto is_col = [&](Item *a) {
    a = a->real_item();
    return a->type() == Item::FIELD_ITEM && down_cast<Item_field *>(a)->field == table->field[col];
  };
  auto is_const = [](Item *a) {
    a = a->real_item();
    return a->result_type() == INT_RESULT && (a->const_item() || a->type() == Item::PARAM_ITEM);
  };
  Item *a = f->arguments()[0], *b = f->arguments()[1];
  switch (f->functype()) {
    case Item_func::PLUS_FUNC:
      return (is_col(a) && is_const(b)) || (is_const(a) && is_col(b));
    case Item_func::MINUS_FUNC:
      return is_col(a) && is_const(b);
    default:
      return false;
  }
}

// ClassifyDeltas returns the columns a single-table UPDATE assigns as
// c = c ± k, where c is an integer column in no key and no untranslated
// CHECK, read nowhere else in the statement. Anything else, and every
// statement shape it does not recognize, gets plain reads and writes.
ColMask ha_celltnx::ClassifyDeltas(THD *thd) {
  LEX *lex = thd->lex;
  if (lex->is_ignore() || table->triggers != nullptr || table->vfield != nullptr) return 0;
  // The assignments, and the other expressions that read this table's
  // stored row: an UPDATE's WHERE and ORDER BY. An INSERT's values describe
  // the new row, and a SELECT feeding it reads through another handler.
  Query_block *qb = lex->query_block;
  mem_root_deque<Item *> *fields = nullptr, *values = nullptr;
  std::vector<Item *> others;
  if (lex->sql_command == SQLCOM_UPDATE) {
    auto *cmd = dynamic_cast<Sql_cmd_update *>(lex->m_sql_cmd);
    if (cmd == nullptr || cmd->update_value_list == nullptr) return 0;
    fields = &qb->fields;
    values = cmd->update_value_list;
    if (Item *w = qb->where_cond()) others.push_back(w);
    for (ORDER *o = qb->order_list.first; o != nullptr; o = o->next) others.push_back(*o->item);
  } else if (lex->sql_command == SQLCOM_INSERT || lex->sql_command == SQLCOM_INSERT_SELECT) {
    auto *cmd = dynamic_cast<Sql_cmd_insert_base *>(lex->m_sql_cmd);
    if (cmd == nullptr || cmd->duplicates != DUP_UPDATE) return 0;
    fields = &cmd->update_field_list;
    values = &cmd->update_value_list;
  } else {
    return 0;
  }
  std::vector<std::pair<int, Item *>> sets;
  auto vit = values->begin();
  for (Item *fi : VisibleFields(*fields)) {
    if (vit == values->end()) return 0;
    Item *val = *vit++;
    Item *target = fi->real_item();
    if (target->type() != Item::FIELD_ITEM) return 0;
    Field *f = down_cast<Item_field *>(target)->field;
    if (f == nullptr || f->table != table) return 0;
    sets.emplace_back(int(f->field_index()), val);
  }
  ColMask candidates = 0, assigned = 0, twice = 0;
  for (auto &[col, val] : sets) {
    if (assigned & Col(col)) twice |= Col(col);
    assigned |= Col(col);
    if (IsDelta(col, val)) candidates |= Col(col);
  }
  candidates &= ~(info_->indexed | info_->final | twice);
  if (candidates == 0) return 0;
  ColMask elsewhere = 0;
  for (Item *e : others) elsewhere |= ReferencedCols(e, table);
  for (auto &[col, val] : sets) elsewhere |= ReferencedCols(val, table) & ~Col(col);
  return candidates & ~elsewhere;
}

// InstallChecks gives the core the table's CHECK constraints it can
// evaluate on merged rows, once per table.
void ha_celltnx::InstallChecks() {
  std::lock_guard g(info_->check_mu);
  if (info_->checks_done) return;
  info_->checks_done = true;
  std::vector<Check> checks;
  if (table->table_check_constraint_list != nullptr) {
    for (Sql_table_check_constraint &cc : *table->table_check_constraint_list) {
      if (!cc.is_enforced() || cc.value_generator() == nullptr) continue;
      Item *expr = cc.value_generator()->expr_item;
      auto pred = std::make_shared<Pred>();
      ColMask cols = 0;
      if (Translate(expr, table, *info_, pred.get(), &cols))
        checks.push_back(Check{std::string(cc.name().str, cc.name().length), cols,
                               [pred](Row r) { return Eval(*pred, r) != kFalse; }});
      else
        info_->final |= ReferencedCols(expr, table);
    }
  }
  engine->db->SetChecks(info_->id, std::move(checks));
}

int ha_celltnx::Scan(int idx, std::string_view lo, std::string_view hi, bool reverse) {
  rows_.clear();
  Prepare();
  Status st = txn()->ScanRows(info_->id, idx, lo, hi, track_, &rows_);
  if (!st.ok()) return Fail(st, "scan");
  pos_ = reverse ? long(rows_.size()) - 1 : 0;
  return 0;
}

int ha_celltnx::Emit(uchar *buf, int not_found) {
  if (pos_ < 0 || pos_ >= long(rows_.size())) return not_found;
  return Load(rows_[pos_], buf);
}

int ha_celltnx::Step(uchar *buf, int dir) {
  pos_ += dir;
  return Emit(buf, HA_ERR_END_OF_FILE);
}

// --- table lifecycle ---

// DurableID returns the store's ID for a table, or 0 for a table kept in
// memory only.
static uint64_t DurableID(const dd::Table *table_def) {
  if (!engine->store || table_def == nullptr || table_def->id() == dd::INVALID_OBJECT_ID) return 0;
  return table_def->id();
}

int ha_celltnx::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info, dd::Table *table_def) {
  std::string err;
  auto info = BuildInfo(form, &err);
  if (!info) {
    my_printf_error(ER_CHECK_NOT_IMPLEMENTED, "%s", MYF(0), err.c_str());
    return HA_ERR_UNSUPPORTED;
  }
  // MySQL stores a table in the data dictionary before asking an engine
  // without atomic DDL to create it, so its dictionary ID is final here and
  // stays put across renames. A temporary table lives in memory only.
  if (!(create_info->options & HA_LEX_CREATE_TMP_TABLE)) info->durable = DurableID(table_def);
  if (info->durable != 0) {
    // Clear rows a crash during an earlier DROP left under a reused ID.
    if (Status st = engine->store->Drop(info->durable); !st.ok()) {
      my_printf_error(ER_INTERNAL_ERROR, "celltnx: %s", MYF(0), st.Message().c_str());
      return HA_ERR_INTERNAL_ERROR;
    }
  }
  std::lock_guard g(engine->mu);
  info->id = engine->db->CreateTable(BuildSchema(form, info, name));
  if (info->durable != 0) engine->store->Bind(info->id, info->durable);
  engine->tables[name] = info;
  return 0;
}

int ha_celltnx::open(const char *name, int, uint, const dd::Table *table_def) {
  std::lock_guard g(engine->mu);
  auto it = engine->tables.find(name);
  if (it == engine->tables.end()) {
    // A table the data dictionary knows from before this process started:
    // its rows load from the store before any session can see it.
    std::string err;
    auto info = BuildInfo(table, &err);
    if (!info) return HA_ERR_UNSUPPORTED;
    info->id = engine->db->CreateTable(BuildSchema(table, info, name));
    info->durable = DurableID(table_def);
    if (info->durable != 0) {
      Field *ai = table->found_next_number_field;
      int ai_col = ai != nullptr ? int(ai->field_index()) : -1;
      uint64_t max_ai = 0;
      Status st = engine->db->LoadRows(info->id, [&](const DB::RowPut &put) {
        return engine->store->Load(info->durable, [&](std::string_view key, Row row) {
          put(key, row);
          if (ai_col >= 0 && size_t(ai_col) < row.size()) {
            const Value &v = row[ai_col];
            if (v.type == Type::kInt && v.i > 0) max_ai = std::max(max_ai, uint64_t(v.i));
            if (v.type == Type::kUint) max_ai = std::max(max_ai, v.u);
          }
        });
      });
      if (!st.ok()) {
        engine->db->DropTable(info->id);
        my_printf_error(ER_INTERNAL_ERROR, "celltnx: loading %s: %s", MYF(0), name, st.Message().c_str());
        return HA_ERR_INTERNAL_ERROR;
      }
      if (max_ai > 0) engine->db->BumpID(info->id, max_ai);
      engine->store->Bind(info->id, info->durable);
    }
    it = engine->tables.emplace(name, info).first;
  }
  info_ = it->second;
  ref_length = info_->ref_len;
  key_buf_.assign(table->s->rec_buff_length + 16, 0);
  return 0;
}

int ha_celltnx::close() { return 0; }

int ha_celltnx::delete_table(const char *name, const dd::Table *table_def) {
  std::lock_guard g(engine->mu);
  auto it = engine->tables.find(name);
  if (it != engine->tables.end()) {
    if (engine->store) engine->store->Unbind(it->second->id);
    engine->db->DropTable(it->second->id);
    engine->tables.erase(it);
  }
  if (uint64_t id = DurableID(table_def); id != 0) {
    if (Status st = engine->store->Drop(id); !st.ok()) {
      my_printf_error(ER_INTERNAL_ERROR, "celltnx: dropping %s: %s", MYF(0), name, st.Message().c_str());
      return HA_ERR_INTERNAL_ERROR;
    }
  }
  return 0;
}

int ha_celltnx::rename_table(const char *from, const char *to, const dd::Table *, dd::Table *) {
  std::lock_guard g(engine->mu);
  auto it = engine->tables.find(from);
  if (it == engine->tables.end()) return HA_ERR_NO_SUCH_TABLE;
  auto info = it->second;
  engine->tables.erase(it);
  engine->tables[to] = info;
  return 0;
}

// --- writes ---

static void ReadRow(TABLE *table, const TableInfo &info, const uchar *buf, std::vector<Value> *vals,
                    std::vector<std::string> *strs) {
  size_t n = info.cols.size();
  vals->resize(n);
  strs->resize(n);
  for (size_t i = 0; i < n; i++) (*vals)[i] = ReadField(table->field[i], info.cols[i], buf, table, &(*strs)[i]);
}

int ha_celltnx::write_row(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_write_count);
  if (table->next_number_field && buf == table->record[0])
    if (int err = update_auto_increment()) return err;
  Prepare();
  try {
    ReadRow(table, *info_, buf, &vals_, &strs_);
    std::string pk = info_->pk < 0 ? keys::U64(engine->db->NextHidden(info_->id)) : EncodePK(buf);
    if (Status st = txn()->Insert(info_->id, pk, vals_); !st.ok()) return Fail(st, "insert");
    if (table->next_number_field && buf == table->record[0]) {
      longlong v = table->next_number_field->val_int();
      if (v > 0) engine->db->BumpID(info_->id, uint64_t(v));
    }
  } catch (const std::exception &e) {
    my_printf_error(ER_INTERNAL_ERROR, "celltnx: %s", MYF(0), e.what());
    return HA_ERR_INTERNAL_ERROR;
  }
  return 0;
}

int ha_celltnx::update_row(const uchar *old_data, uchar *new_data) {
  ha_statistic_increment(&System_status_var::ha_update_count);
  Prepare();
  try {
    ReadRow(table, *info_, new_data, &vals_, &strs_);
    std::string pk_old = info_->pk < 0 ? cur_pk_ : EncodePK(old_data);
    std::string pk_new = info_->pk < 0 ? pk_old : EncodePK(new_data);
    Status st;
    if (pk_old != pk_new) {
      st = txn()->Delete(info_->id, pk_old);
      if (st.ok()) st = txn()->Insert(info_->id, pk_new, vals_);
    } else {
      ColMask cols = assigned_;
      ColMask deltas = cols & delta_;
      if (ColMask sets = cols & ~deltas) st = txn()->Set(info_->id, pk_old, sets, vals_);
      // A delta's amount is new minus old, which is the statement's
      // constant whatever the untracked old value was.
      std::string scratch;
      EachCol(deltas, [&](int c) {
        if (!st.ok()) return;
        Value o = ReadField(table->field[c], info_->cols[c], old_data, table, &scratch);
        const Value &n = vals_[c];
        if (o.null() || n.null()) {
          st = txn()->Set(info_->id, pk_old, Col(c), vals_);
          return;
        }
        int64_t d = info_->cols[c].kind == ColKind::kUint ? int64_t(n.u - o.u) : int64_t(uint64_t(n.i) - uint64_t(o.i));
        if (d != 0) st = txn()->Add(info_->id, pk_old, c, d);
      });
    }
    if (!st.ok()) return Fail(st, "update");
  } catch (const std::exception &e) {
    my_printf_error(ER_INTERNAL_ERROR, "celltnx: %s", MYF(0), e.what());
    return HA_ERR_INTERNAL_ERROR;
  }
  return 0;
}

int ha_celltnx::delete_row(const uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_delete_count);
  Prepare();
  std::string pk = info_->pk < 0 ? cur_pk_ : EncodePK(buf);
  if (Status st = txn()->Delete(info_->id, pk); !st.ok()) return Fail(st, "delete");
  return 0;
}

// --- reads ---

int ha_celltnx::rnd_init(bool scan) {
  rows_.clear();
  pos_ = -1;
  if (!scan) return 0;
  if (int err = Scan(-1, "", "", false)) return err;
  pos_ = -1;
  return 0;
}

int ha_celltnx::rnd_next(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return Step(buf, 1);
}

int ha_celltnx::rnd_end() {
  rows_.clear();
  return 0;
}

void ha_celltnx::position(const uchar *record) {
  std::string pk = info_->pk < 0 ? cur_pk_ : EncodePK(record);
  memset(ref, 0, ref_length);
  int2store(ref, uint16_t(pk.size()));
  memcpy(ref + 2, pk.data(), std::min<size_t>(pk.size(), ref_length - 2));
}

int ha_celltnx::rnd_pos(uchar *buf, uchar *pos) {
  ha_statistic_increment(&System_status_var::ha_read_rnd_count);
  std::string pk(reinterpret_cast<char *>(pos + 2), uint2korr(pos));
  Prepare();
  bool found;
  if (Status st = txn()->GetRow(info_->id, pk, track_, &vals_, &found); !st.ok()) return Fail(st, "read");
  if (!found) return HA_ERR_KEY_NOT_FOUND;
  rows_.assign(1, Rec{std::string_view(), vals_});
  pos_ = 0;
  pk_store_ = pk;
  rows_[0].pk = pk_store_;
  return Load(rows_[0], buf);
}

int ha_celltnx::index_init(uint idx, bool) {
  active_index = idx;
  rows_.clear();
  pos_ = -1;
  return 0;
}

int ha_celltnx::index_end() {
  active_index = MAX_KEY;
  rows_.clear();
  return 0;
}

// LockRow takes the row lock for a FOR UPDATE read of pk. If another
// transaction committed the row since this one's snapshot, typically the
// one it waited for, the snapshot moves forward so the read sees that
// commit. A refresh that fails means a conflict at commit, so the
// transaction rolls back now instead.
//
// With if_used, for an UPDATE that assigns deltas, it takes the lock only
// when a FOR UPDATE reader of a delta column holds it, or anyone awaits it:
// a delta committing inside that read-modify-write would fail its
// validation, so it queues behind instead. Elsewhere deltas stay lock-free
// and commute, as Payment's d_ytd does beside NewOrder's lock on the same
// district row.
int ha_celltnx::LockRow(std::string_view pk, bool if_used) {
  THD *thd = ha_thd();
  Trx *trx = GetTrx(thd);
  std::string key(sizeof(int), '\0');
  int id = info_->id;
  memcpy(key.data(), &id, sizeof id);
  key.append(pk);
  bool wait = trx->locks.empty() && lock_wait_ms_var > 0;
  ColMask cols = if_used ? delta_ : track_;
  auto got =
      engine->locks.Acquire(thd, key, trx, cols, wait, std::chrono::milliseconds(lock_wait_ms_var), if_used);
  if (got == RowLocks::Got::kWaited) counters.lock_waits.fetch_add(1, std::memory_order_relaxed);
  if (got == RowLocks::Got::kBusy && wait) counters.lock_timeouts.fetch_add(1, std::memory_order_relaxed);
  if (got == RowLocks::Got::kBusy || got == RowLocks::Got::kHeld || got == RowLocks::Got::kUnused) return 0;
  trx->locks.push_back(std::move(key));
  Txn *t = txn();
  if (got == RowLocks::Got::kFree && !t->Changed(id, pk)) return 0;
  counters.refreshes.fetch_add(1, std::memory_order_relaxed);
  if (Status st = t->Refresh(); !st.ok()) {
    counters.refresh_conflicts.fetch_add(1, std::memory_order_relaxed);
    thd_mark_transaction_to_rollback(thd, 1);
    return HA_ERR_LOCK_DEADLOCK;
  }
  return 0;
}

int ha_celltnx::index_read_map(uchar *buf, const uchar *key, key_part_map map, enum ha_rkey_function flag) {
  ha_statistic_increment(&System_status_var::ha_read_key_count);
  const KeySpec &ks = info_->keys[active_index];
  std::string k = EncodeKey(active_index, key, map);
  size_t parts = size_t(std::countr_one(map));
  if (ks.primary && parts >= ks.cols.size() && flag == HA_READ_KEY_EXACT) {
    Prepare();
    if (for_update_ || delta_ != 0)
      if (int err = LockRow(k, !for_update_)) return err;
    bool found;
    if (Status st = txn()->GetRow(info_->id, k, track_, &vals_, &found); !st.ok()) return Fail(st, "read");
    if (!found) {
      rows_.clear();
      pos_ = -1;
      return HA_ERR_KEY_NOT_FOUND;
    }
    rows_.assign(1, Rec{std::string_view(), vals_});
    pos_ = 0;
    pk_store_ = k;
    rows_[0].pk = pk_store_;
    return Load(rows_[0], buf);
  }
  int idx = ks.primary ? -1 : ks.core_index;
  std::string succ = keys::Succ(k);
  int err = 0;
  switch (flag) {
    case HA_READ_KEY_EXACT:
      err = Scan(idx, k, succ, false);
      break;
    case HA_READ_KEY_OR_NEXT:
      err = Scan(idx, k, "", false);
      break;
    case HA_READ_AFTER_KEY:
      if (succ.empty()) return HA_ERR_KEY_NOT_FOUND;
      err = Scan(idx, succ, "", false);
      break;
    case HA_READ_KEY_OR_PREV:
      err = Scan(idx, "", succ, true);
      break;
    case HA_READ_BEFORE_KEY:
      err = Scan(idx, "", k, true);
      break;
    case HA_READ_PREFIX_LAST:
      err = Scan(idx, k, succ, true);
      break;
    case HA_READ_PREFIX_LAST_OR_PREV:
      err = Scan(idx, "", succ, true);
      break;
    default:
      return HA_ERR_WRONG_COMMAND;
  }
  if (err) return err;
  return Emit(buf, HA_ERR_KEY_NOT_FOUND);
}

int ha_celltnx::index_read_last_map(uchar *buf, const uchar *key, key_part_map map) {
  return index_read_map(buf, key, map, HA_READ_PREFIX_LAST);
}

int ha_celltnx::index_next(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  return Step(buf, 1);
}

int ha_celltnx::index_prev(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_read_prev_count);
  return Step(buf, -1);
}

int ha_celltnx::index_first(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_read_first_count);
  const KeySpec &ks = info_->keys[active_index];
  if (int err = Scan(ks.primary ? -1 : ks.core_index, "", "", false)) return err;
  return Emit(buf, HA_ERR_END_OF_FILE);
}

int ha_celltnx::index_last(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_read_last_count);
  const KeySpec &ks = info_->keys[active_index];
  if (int err = Scan(ks.primary ? -1 : ks.core_index, "", "", true)) return err;
  return Emit(buf, HA_ERR_END_OF_FILE);
}

// index_next_same follows an exact-prefix read, whose scan already ends
// at the prefix.
int ha_celltnx::index_next_same(uchar *buf, const uchar *, uint) {
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  return Step(buf, 1);
}

int ha_celltnx::read_range_first(const key_range *start, const key_range *end, bool eq_range, bool) {
  const KeySpec &ks = info_->keys[active_index];
  // An equality range on the whole primary key is a point read, which
  // records no scanned range.
  if (eq_range && start != nullptr && ks.primary && start->flag == HA_READ_KEY_EXACT &&
      size_t(std::countr_one(start->keypart_map)) >= ks.cols.size()) {
    int err = index_read_map(table->record[0], start->key, start->keypart_map, HA_READ_KEY_EXACT);
    return err == HA_ERR_KEY_NOT_FOUND ? HA_ERR_END_OF_FILE : err;
  }
  std::string lo, hi;
  if (!RangeBounds(active_index, start, end, &lo, &hi)) return HA_ERR_END_OF_FILE;
  if (int err = Scan(ks.primary ? -1 : ks.core_index, lo, hi, false)) return err;
  return Emit(table->record[0], HA_ERR_END_OF_FILE);
}

// RangeBounds encodes a handler key range as [lo, hi), with an empty hi
// unbounded. It returns false when the range is empty.
bool ha_celltnx::RangeBounds(uint keynr, const key_range *start, const key_range *end, std::string *lo,
                             std::string *hi) {
  lo->clear();
  hi->clear();
  if (start != nullptr) {
    std::string k = EncodeKey(keynr, start->key, start->keypart_map);
    if (start->flag == HA_READ_AFTER_KEY) {
      *lo = keys::Succ(k);
      if (lo->empty()) return false;
    } else {
      *lo = std::move(k);
    }
  }
  if (end != nullptr) {
    std::string k = EncodeKey(keynr, end->key, end->keypart_map);
    *hi = end->flag == HA_READ_BEFORE_KEY ? k : keys::Succ(k);
    if (hi->empty() && end->flag == HA_READ_BEFORE_KEY) return false;
  }
  return true;
}

int ha_celltnx::read_range_next() { return Step(table->record[0], 1); }

int ha_celltnx::info(uint flag) {
  if (flag & HA_STATUS_VARIABLE) {
    stats.records = engine->db->ApproxRows(info_->id);
    stats.deleted = 0;
    stats.mean_rec_length = table->s->reclength;
    stats.data_file_length = stats.records * table->s->reclength;
  }
  if (flag & HA_STATUS_CONST) {
    float rows = float(std::max<uint64_t>(engine->db->ApproxRows(info_->id), 1));
    for (uint k = 0; k < table->s->keys; k++) {
      KEY &key = table->key_info[k];
      for (uint j = 0; j < key.actual_key_parts; j++) {
        bool exact = (key.flags & HA_NOSAME) && j + 1 >= key.user_defined_key_parts;
        key.set_records_per_key(j, exact ? 1.0f : std::max(1.0f, rows / float(uint64_t(1) << std::min(4 * (j + 1), 60u))));
      }
    }
  }
  if (flag & HA_STATUS_AUTO) stats.auto_increment_value = engine->db->PeekID(info_->id) + 1;
  // The server resets errkey before asking which key a duplicate hit.
  if (flag & HA_STATUS_ERRKEY) errkey = dup_key_;
  return 0;
}

// records_in_range counts the committed rows in the range, up to a cap that
// is far past any range where the count changes a plan.
ha_rows ha_celltnx::records_in_range(uint keynr, key_range *min_key, key_range *max_key) {
  std::string lo, hi;
  if (!RangeBounds(keynr, min_key, max_key, &lo, &hi)) return 0;
  const KeySpec &ks = info_->keys[keynr];
  // The optimizer takes 0 to mean an empty range; 1 keeps it honest.
  return std::max<ha_rows>(1, engine->db->CountRange(info_->id, ks.primary ? -1 : ks.core_index, lo, hi, 1000));
}

int ha_celltnx::external_lock(THD *thd, int lock_type) {
  Trx *trx = GetTrx(thd);
  if (lock_type != F_UNLCK) {
    if (trx->in_use++ == 0) StartStatement(thd, trx);
  } else if (trx->in_use > 0 && --trx->in_use == 0 && trx->txn) {
    trx->txn->EndStatement();
  }
  lock_type_ = lock_type;
  return 0;
}

int ha_celltnx::start_stmt(THD *thd, thr_lock_type) {
  StartStatement(thd, GetTrx(thd));
  return 0;
}

// --- status variables ---

template <std::atomic<uint64_t> Counters::*F>
static int ShowCounter(THD *, SHOW_VAR *var, char *buf) {
  var->type = SHOW_LONGLONG;
  var->value = buf;
  *reinterpret_cast<long long *>(buf) = static_cast<long long>((counters.*F).load(std::memory_order_relaxed));
  return 0;
}

template <std::atomic<uint64_t> Trx::*F>
static int ShowSession(THD *thd, SHOW_VAR *var, char *buf) {
  auto *trx = static_cast<Trx *>(thd_get_ha_data(thd, hton));
  var->type = SHOW_LONGLONG;
  var->value = buf;
  *reinterpret_cast<long long *>(buf) =
      trx == nullptr ? 0 : static_cast<long long>((trx->*F).load(std::memory_order_relaxed));
  return 0;
}

#define CELLTNX_COUNTER(name, field) \
  {"Celltnx_" name, reinterpret_cast<char *>(&ShowCounter<&Counters::field>), SHOW_FUNC, SHOW_SCOPE_GLOBAL}
SHOW_VAR status_variables[] = {
    {"Celltnx_last_commit_ts", reinterpret_cast<char *>(&ShowSession<&Trx::last_commit_ts>), SHOW_FUNC,
     SHOW_SCOPE_SESSION},
    {"Celltnx_last_read_ts", reinterpret_cast<char *>(&ShowSession<&Trx::last_read_ts>), SHOW_FUNC,
     SHOW_SCOPE_SESSION},
    CELLTNX_COUNTER("commits", commits),
    CELLTNX_COUNTER("conflicts", conflicts),
    CELLTNX_COUNTER("bucket_conflicts", bucket_conflicts),
    CELLTNX_COUNTER("check_failures", check_failures),
    CELLTNX_COUNTER("lock_waits", lock_waits),
    CELLTNX_COUNTER("lock_timeouts", lock_timeouts),
    CELLTNX_COUNTER("refreshes", refreshes),
    CELLTNX_COUNTER("refresh_conflicts", refresh_conflicts),
    CELLTNX_COUNTER("commit_lock_wait_ns", commit_lock_wait_ns),
    CELLTNX_COUNTER("commit_lock_hold_ns", commit_lock_hold_ns),
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF},
};
#undef CELLTNX_COUNTER

void ha_celltnx::get_auto_increment(ulonglong, ulonglong, ulonglong, ulonglong *first, ulonglong *reserved) {
  *first = engine->db->NextID(info_->id);
  *reserved = 1;
}

}  // namespace celltnx::engine

using celltnx::engine::bucket_bits_var;
using celltnx::engine::commit_delay_us_var;
using celltnx::engine::durable_var;
using celltnx::engine::group_wait_us_var;
using celltnx::engine::lock_wait_ms_var;
using celltnx::engine::sync_var;
using celltnx::engine::mode_typelib;
using celltnx::engine::mode_var;

static MYSQL_SYSVAR_ENUM(mode, mode_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Conflict tracking: row, cell or cell+delta", nullptr, nullptr, 2, &mode_typelib);
static MYSQL_SYSVAR_UINT(bucket_bits, bucket_bits_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Low key bits that each phantom-tracking bucket spans", nullptr, nullptr, 0, 0, 64, 0);

static MYSQL_SYSVAR_UINT(lock_wait_ms, lock_wait_ms_var, PLUGIN_VAR_RQCMDARG,
                         "Milliseconds a SELECT ... FOR UPDATE waits for another transaction's lock on a row before "
                         "reading it anyway; 0 never waits",
                         nullptr, nullptr, 1000, 0, 3600 * 1000, 0);

static MYSQL_SYSVAR_BOOL(durable, durable_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Keep tables in a lionturtle store under <datadir>/celltnx; with OFF they live in memory only",
                         nullptr, nullptr, true);
static MYSQL_SYSVAR_BOOL(sync, sync_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Acknowledge a commit once the store has synced it; with OFF, once it is visible, so a "
                         "crash may lose the latest acknowledged commits",
                         nullptr, nullptr, true);
static MYSQL_SYSVAR_UINT(commit_delay_us, commit_delay_us_var, PLUGIN_VAR_RQCMDARG,
                         "Microseconds the store's writer waits before each flush, so more commits share a sync",
                         nullptr, nullptr, 0, 0, 1000000, 0);

static MYSQL_SYSVAR_UINT(group_wait_us, group_wait_us_var, PLUGIN_VAR_RQCMDARG,
                         "Microseconds the store's writer may wait, after each sync, for the clients it acknowledged "
                         "to commit again, so they share the next sync; it waits no longer than that sync took, "
                         "and 0 never waits",
                         nullptr, nullptr, 500, 0, 100000, 0);

static SYS_VAR *celltnx_system_variables[] = {MYSQL_SYSVAR(mode),         MYSQL_SYSVAR(bucket_bits),
                                              MYSQL_SYSVAR(lock_wait_ms), MYSQL_SYSVAR(durable),
                                              MYSQL_SYSVAR(sync),         MYSQL_SYSVAR(commit_delay_us),
                                              MYSQL_SYSVAR(group_wait_us), nullptr};

static struct st_mysql_storage_engine celltnx_storage_engine = {MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(celltnx){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &celltnx_storage_engine,
    "CELLTNX",
    "cell-tnx",
    "Serializable optimistic transactions with per-cell conflict tracking",
    PLUGIN_LICENSE_GPL,
    celltnx::engine::Init,
    nullptr,
    celltnx::engine::Deinit,
    0x0001,
    celltnx::engine::status_variables,
    celltnx_system_variables,
    nullptr,
    0,
} mysql_declare_plugin_end;
