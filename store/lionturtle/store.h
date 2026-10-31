// A durable Store on lionturtle. Each flush is one atomic lt_write holding
// every commit's row images and the newest commit timestamp, and the writer
// issues flushes in order, so a crash keeps a prefix of the commits. Rows
// live under 0x01, the table's durable ID (big-endian) and the primary key;
// metadata lives under 0x00. Hosts give each table a durable ID that
// outlives renames, such as its data-dictionary ID, and bind the core's
// table ID to it.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "db.h"

struct lt_db;
struct lt_batch;

namespace celltnx::lionturtle {

class Store : public celltnx::Store {
 public:
  // Open opens or creates the store in dir.
  static Status Open(const std::string &dir, std::unique_ptr<Store> *out);
  // The destructor closes the store, syncing its log.
  ~Store() override;
  Store(const Store &) = delete;
  Store &operator=(const Store &) = delete;

  Status Write(std::span<const Commit> batch) override;
  Status Sync() override;

  // Last returns the newest commit timestamp the store holds.
  uint64_t Last() const { return last_; }

  // Bind sends the core table tbl's writes to durable table id. Unbind
  // stops them, after any write in progress; writes to an unbound table
  // are dropped, as for a dropped or temporary table.
  void Bind(int tbl, uint64_t id);
  void Unbind(int tbl);
  // Load passes every row of durable table id to put, in key order.
  Status Load(uint64_t id, const DB::RowPut &put);
  // Drop deletes every row of durable table id, synced before it returns.
  Status Drop(uint64_t id);

 private:
  explicit Store(lt_db *db);

  lt_db *db_;
  std::mutex mu_;  // guards everything below, and orders Write with Drop
  lt_batch *batch_;
  std::vector<uint64_t> ids_;  // durable ID by core table ID; 0 is unbound
  std::string key_, val_;      // scratch for Write
  uint64_t last_ = 0;
};

// EncodeRow appends row's image to out; DecodeRow parses one, pointing
// string values into b. DecodeRow reports false for a malformed image.
void EncodeRow(Row row, std::string *out);
bool DecodeRow(std::string_view b, std::vector<Value> *row);

}  // namespace celltnx::lionturtle
