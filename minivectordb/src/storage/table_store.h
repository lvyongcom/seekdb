// Disk-resident row store for one table.
//
// Why this exists: the task description states that formal cases include a data
// set larger than the memory budget. Keeping decoded rows in a std::vector cost
// roughly 4x the on-disk size, so the store keeps *only* the primary key ->
// record-offset map in memory and streams row bodies from the file.
//
// File layout is the same framing the redo log uses:
//     [u32 body_length][u8 op][payload][u32 crc32(op and payload)]
// with payload = primary key (8 bytes little endian) followed by the encoded
// row image for insert/update, and just the primary key for delete.
//
// A record is live when its offset equals the offset the key map holds. That
// makes a later version or a delete mask earlier ones with no rewriting, and
// makes recovery a single streaming pass.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

namespace mvdb {

// Record kinds. Insert and Update both carry a full row image, so recovery and
// replay treat them as the same upsert.
enum class LogOp : uint8_t { Insert = 1, Update = 2, Delete = 3 };

class TableStore {
 public:
  TableStore() = default;
  ~TableStore();

  TableStore(const TableStore&) = delete;
  TableStore& operator=(const TableStore&) = delete;

  bool Open(const std::string& data_dir, uint64_t table_id);

  // Appends one record and reports its offset. `row_image` must already be the
  // encoded row; the primary key is prefixed here so liveness checks never have
  // to decode the row.
  bool Append(LogOp op, int64_t key, const std::string& row_image, uint64_t* offset);

  bool Flush();

  // Rebuilds the key map by streaming the file. Bounded memory: one record at a
  // time, never the whole file.
  bool Recover();

  // The row image is passed as a view into the scan buffer, not a copy: a scan
  // touches every row, so a per-row std::string would dominate the cost.
  using RowCallback = std::function<bool(int64_t key, uint64_t offset,
                                         const char* data, size_t size)>;

  // Streams every live row in write order. Returning false stops early.
  bool Scan(const RowCallback& callback);

  // Reads one row image. Returns false when the key is absent.
  bool Read(int64_t key, std::string* row_image);

  bool Contains(int64_t key) const { return live_.find(key) != live_.end(); }

  // Current end-of-file offset; a transaction begin-mark uses this.
  uint64_t append_offset() const { return append_offset_; }

  // Offset of the live version of `key`. Returns false when absent.
  bool GetOffset(int64_t key, uint64_t* offset) const {
    const auto found = live_.find(key);
    if (found == live_.end()) return false;
    *offset = found->second;
    return true;
  }

  // Rollback support: put the key map back the way it was. The record itself
  // is discarded by truncating the file, so no rewriting is needed.
  void SetOffset(int64_t key, uint64_t offset) { live_[key] = offset; }
  void EraseOffset(int64_t key) { live_.erase(key); }

  // Drops everything past `offset` and resets the writer.
  bool Truncate(uint64_t offset);

  // Number of records that are no longer the live version of their key
  // (overwritten or deleted). When it is zero every record in the file is live,
  // which lets a scan skip the per-record key-map lookup entirely.
  size_t stale_records() const { return stale_records_; }

 private:
  using Visit = std::function<bool(LogOp, int64_t, uint64_t, const char*, size_t)>;

  // Streams every record, live or not; `visit` returns false to stop.
  bool ForEachRecord(const Visit& visit);
  // Parses records already held in one contiguous buffer.
  bool ScanBuffer(const char* base, size_t size, const Visit& visit);
  uint64_t FileSize() const;

  std::string path_;
  std::FILE* writer_ = nullptr;
  uint64_t append_offset_ = 0;
  // End offset of the last intact record seen by the last stream pass; used by
  // Recover to drop a torn tail and to position the writer.
  uint64_t last_valid_offset_ = 0;
  size_t stale_records_ = 0;

  // Whole-file byte cache, used only while the file fits the budget. It holds
  // the *encoded* records (about 1.0x the data size), not decoded rows, so a
  // table that fits stays fast without the 4x blow-up of materialising Values.
  std::string cache_;
  bool cache_active_ = false;

  std::unordered_map<int64_t, uint64_t> live_;
};

}  // namespace mvdb
