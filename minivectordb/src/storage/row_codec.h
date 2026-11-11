// Compact row encoding shared by the data file and the redo log.
//
// Layout is schema-driven and variable length, in column declaration order:
//   BIGINT   -> 8 bytes, little endian
//   VARCHAR  -> u32 byte length, then that many UTF-8 bytes
//   VECTOR   -> dimension * 4 bytes of little-endian float32
//
// There is no NULL support in this dialect, so no null bitmap is needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "sql/value.h"

namespace mvdb {

std::string EncodeRow(const std::vector<ColumnDef>& columns,
                      const std::vector<Value>& values);

// Appends into a caller-owned buffer so a bulk insert does not allocate a fresh
// string per row. `out` is cleared first.
void EncodeRowInto(const std::vector<ColumnDef>& columns,
                   const std::vector<Value>& values, std::string* out);

// Returns false when the buffer is truncated or does not match the schema.
bool DecodeRow(const std::vector<ColumnDef>& columns, const std::string& bytes,
               std::vector<Value>* values);

// Primary key payload used by delete records, so a delete does not have to
// carry the whole row image.
std::string EncodeKey(int64_t key);
bool DecodeKey(const std::string& bytes, int64_t* key);

namespace detail {

inline uint32_t LoadU32(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

inline int64_t LoadI64(const char* p) {
  uint64_t bits = 0;
  for (int k = 0; k < 8; ++k) {
    bits |= static_cast<uint64_t>(static_cast<unsigned char>(p[k])) << (8 * k);
  }
  return static_cast<int64_t>(bits);
}

}  // namespace detail

// Zero-copy, read-only view over one encoded row.
//
// DecodeRow() is convenient but costs three or four heap allocations per row
// (one per VARCHAR, one for the vector, one for the Value vector). A scan runs
// on every row of the table, so it uses this instead: the column offsets are
// computed once into an inline array and every accessor reads directly from the
// record bytes.
//
// Everything is inline on purpose. These accessors sit in the innermost loop of
// every query; as out-of-line calls they cost more than the arithmetic.
constexpr size_t kMaxInlineColumns = 16;

class RowReader {
 public:
  RowReader(const std::vector<ColumnDef>& columns, const char* data, size_t size)
      : columns_(columns), data_(data), size_(size) {
    if (data == nullptr || columns.size() > kMaxInlineColumns) return;

    size_t position = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
      offsets_[i] = position;
      switch (columns[i].type) {
        case ColumnType::BigInt:
          if (position + 8 > size) return;
          position += 8;
          break;
        case ColumnType::VarChar: {
          if (position + 4 > size) return;
          const size_t length = detail::LoadU32(data + position);
          if (position + 4 + length > size) return;
          position += 4 + length;
          break;
        }
        case ColumnType::Vector: {
          const size_t bytes = static_cast<size_t>(columns[i].length) * 4;
          if (position + bytes > size) return;
          position += bytes;
          break;
        }
      }
    }
    if (position != size) return;
    valid_ = true;
  }

  bool valid() const { return valid_; }

  int64_t Int(int column) const { return detail::LoadI64(data_ + offsets_[column]); }

  std::string_view Str(int column) const {
    const size_t position = offsets_[column];
    const size_t length = detail::LoadU32(data_ + position);
    return std::string_view(data_ + position + 4, length);
  }

  // Start of a VECTOR column's float32 payload.
  const char* VecBytes(int column) const { return data_ + offsets_[column]; }

  float VecAt(int column, uint32_t index) const {
    float value = 0.0f;
    std::memcpy(&value, data_ + offsets_[column] + static_cast<size_t>(index) * 4,
                sizeof(value));
    return value;
  }

 private:
  const std::vector<ColumnDef>& columns_;
  const char* data_ = nullptr;
  size_t size_ = 0;
  size_t offsets_[kMaxInlineColumns] = {0};
  bool valid_ = false;
};

}  // namespace mvdb
