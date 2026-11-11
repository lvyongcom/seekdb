#include "storage/row_codec.h"

#include <cstring>

namespace mvdb {
namespace {

void AppendU32(std::string* out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>((value >> 16) & 0xFF));
  out->push_back(static_cast<char>((value >> 24) & 0xFF));
}

void AppendI64(std::string* out, int64_t value) {
  const uint64_t bits = static_cast<uint64_t>(value);
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((bits >> shift) & 0xFF));
  }
}

void AppendF32(std::string* out, float value) {
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "float32 assumed");
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32(out, bits);
}

bool ReadU32(const std::string& bytes, size_t* pos, uint32_t* out) {
  if (*pos + 4 > bytes.size()) return false;
  *out = static_cast<uint32_t>(static_cast<unsigned char>(bytes[*pos])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(bytes[*pos + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(bytes[*pos + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(bytes[*pos + 3])) << 24);
  *pos += 4;
  return true;
}

bool ReadI64(const std::string& bytes, size_t* pos, int64_t* out) {
  if (*pos + 8 > bytes.size()) return false;
  uint64_t bits = 0;
  for (int k = 0; k < 8; ++k) {
    bits |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[*pos + static_cast<size_t>(k)]))
            << (8 * k);
  }
  *pos += 8;
  *out = static_cast<int64_t>(bits);
  return true;
}

bool ReadF32(const std::string& bytes, size_t* pos, float* out) {
  uint32_t bits = 0;
  if (!ReadU32(bytes, pos, &bits)) return false;
  std::memcpy(out, &bits, sizeof(bits));
  return true;
}

}  // namespace

std::string EncodeRow(const std::vector<ColumnDef>& columns,
                      const std::vector<Value>& values) {
  std::string out;
  out.reserve(32);
  for (size_t i = 0; i < columns.size(); ++i) {
    const ColumnDef& column = columns[i];
    const Value& value = values[i];
    switch (column.type) {
      case ColumnType::BigInt:
        AppendI64(&out, value.integer);
        break;
      case ColumnType::VarChar:
        AppendU32(&out, static_cast<uint32_t>(value.text.size()));
        out.append(value.text);
        break;
      case ColumnType::Vector:
        for (const float element : value.vec) AppendF32(&out, element);
        break;
    }
  }
  return out;
}

bool DecodeRow(const std::vector<ColumnDef>& columns, const std::string& bytes,
               std::vector<Value>* values) {
  values->assign(columns.size(), Value());
  size_t pos = 0;
  for (size_t i = 0; i < columns.size(); ++i) {
    const ColumnDef& column = columns[i];
    Value& value = (*values)[i];
    switch (column.type) {
      case ColumnType::BigInt: {
        value.kind = Value::Kind::Int;
        if (!ReadI64(bytes, &pos, &value.integer)) return false;
        break;
      }
      case ColumnType::VarChar: {
        value.kind = Value::Kind::Str;
        uint32_t length = 0;
        if (!ReadU32(bytes, &pos, &length)) return false;
        if (pos + length > bytes.size()) return false;
        value.text.assign(bytes, pos, length);
        pos += length;
        break;
      }
      case ColumnType::Vector: {
        value.kind = Value::Kind::Vec;
        value.vec.resize(column.length);
        for (uint32_t k = 0; k < column.length; ++k) {
          if (!ReadF32(bytes, &pos, &value.vec[k])) return false;
        }
        break;
      }
    }
  }
  return pos == bytes.size();
}

std::string EncodeKey(int64_t key) {
  std::string out;
  out.reserve(8);
  AppendI64(&out, key);
  return out;
}

bool DecodeKey(const std::string& bytes, int64_t* key) {
  size_t pos = 0;
  if (!ReadI64(bytes, &pos, key)) return false;
  return pos == bytes.size();
}

}  // namespace mvdb
