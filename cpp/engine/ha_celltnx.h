// The celltnx storage engine: MySQL tables backed by the celltnx core, a
// serializable optimistic transaction layer with per-(row, column)
// conflict tracking. ENGINE.md describes it.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "my_base.h"
#include "sql/handler.h"
#include "db.h"

namespace celltnx::engine {

struct TableInfo;
struct Trx;

class ha_celltnx : public handler {
 public:
  ha_celltnx(handlerton *hton, TABLE_SHARE *share);
  ~ha_celltnx() override;

  const char *table_type() const override { return "CELLTNX"; }
  Table_flags table_flags() const override;
  ulong index_flags(uint idx, uint part, bool all_parts) const override;
  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys() const override { return MAX_KEY; }
  uint max_supported_key_parts() const override { return MAX_REF_PARTS; }
  uint max_supported_key_length() const override { return 3072; }
  uint max_supported_key_part_length(HA_CREATE_INFO *) const override { return 3072; }
  enum ha_key_alg get_default_index_algorithm() const override { return HA_KEY_ALG_BTREE; }
  bool is_index_algorithm_supported(enum ha_key_alg alg) const override { return alg == HA_KEY_ALG_BTREE; }
  // No table-level locks: concurrency control is optimistic.
  uint lock_count() const override { return 0; }

  int create(const char *name, TABLE *form, HA_CREATE_INFO *info, dd::Table *table_def) override;
  int open(const char *name, int mode, uint test_if_locked, const dd::Table *table_def) override;
  int close() override;
  int delete_table(const char *name, const dd::Table *table_def) override;
  int rename_table(const char *from, const char *to, const dd::Table *from_def, dd::Table *to_def) override;

  int write_row(uchar *buf) override;
  int update_row(const uchar *old_data, uchar *new_data) override;
  int delete_row(const uchar *buf) override;

  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  int rnd_end() override;
  void position(const uchar *record) override;

  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_last_map(uchar *buf, const uchar *key, key_part_map keypart_map) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;
  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range, bool sorted) override;
  int read_range_next() override;

  int info(uint flag) override;
  int extra(enum ha_extra_function) override { return 0; }
  ha_rows records_in_range(uint inx, key_range *min_key, key_range *max_key) override;
  int external_lock(THD *thd, int lock_type) override;
  int start_stmt(THD *thd, thr_lock_type lock_type) override;
  THR_LOCK_DATA **store_lock(THD *, THR_LOCK_DATA **to, enum thr_lock_type) override { return to; }
  void get_auto_increment(ulonglong offset, ulonglong increment, ulonglong nb_desired_values, ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;

 private:
  // Scan state: an ascending list of rows from one range, and a position
  // in it. Rows and keys stay valid until the transaction ends.
  int Scan(int idx, std::string_view lo, std::string_view hi, bool reverse);
  int Emit(uchar *buf, int not_found);
  int Step(uchar *buf, int dir);
  int Load(const Rec &rec, uchar *buf);
  Txn *txn();
  int Fail(const Status &st, const char *what);
  // Prepare computes the running statement's tracked and delta columns on
  // its first row operation, once read_set and write_set are final.
  void Prepare();
  ColMask ClassifyDeltas(THD *thd);
  bool IsDelta(int col, Item *val) const;
  void InstallChecks();
  std::string EncodePK(const uchar *record);
  std::string EncodeKey(uint keynr, const uchar *key, key_part_map map);
  int LockRow(std::string_view pk, bool if_used);
  bool RangeBounds(uint keynr, const key_range *start, const key_range *end, std::string *lo, std::string *hi);

  std::shared_ptr<const TableInfo> info_;
  std::vector<Rec> rows_;
  long pos_ = -1;
  std::string cur_pk_;    // the primary key of the row last returned
  std::string pk_store_;  // the key behind a point read's Rec
  std::vector<Value> vals_;
  std::vector<std::string> strs_;
  std::vector<uchar> key_buf_;
  int lock_type_ = F_UNLCK;
  uint dup_key_ = MAX_KEY;  // the key a duplicate-key error hit
  int64_t stmt_id_ = -1;    // the query Prepare last ran for
  ColMask track_ = 0;       // columns this statement reads
  ColMask delta_ = 0;       // columns this statement assigns as c = c ± k
  ColMask assigned_ = 0;    // columns this statement's updates assign
  bool for_update_ = false;  // a SELECT ... FOR UPDATE that may wait for row locks
};

}  // namespace celltnx::engine
