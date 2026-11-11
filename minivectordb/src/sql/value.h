// SQL value and column type definitions.
//
// M1 keeps values in a plain, readable form (std::string / std::vector). The
// packed, allocation-free row encoding described in the design notes is an M2
// storage-layer concern; nothing here should be taken as the on-disk layout.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mvdb {

enum class ColumnType { BigInt, VarChar, Vector };

struct ColumnDef {
  std::string name;
  ColumnType type = ColumnType::BigInt;
  // VARCHAR: maximum length in Unicode code points.
  // VECTOR:  dimension.
  uint32_t length = 0;
  bool primary_key = false;
};

struct Value {
  enum class Kind { Int, Str, Vec };

  Kind kind = Kind::Int;
  int64_t integer = 0;
  std::string text;
  std::vector<float> vec;

  static Value FromInt(int64_t v);
  static Value FromStr(std::string v);
  static Value FromVec(std::vector<float> v);
};

// Number of Unicode code points in a UTF-8 string. The spec counts VARCHAR
// length in characters, not bytes: "中文" is 2, not 6.
size_t Utf8Length(const std::string& text);

// Parses a vector literal body such as `[1, 0, 0, 0]` (the part inside the SQL
// string quotes). Rejects malformed syntax and non-finite numbers.
bool ParseVectorLiteral(const std::string& text, std::vector<double>* out,
                        std::string* message);

// Converts to the stored float32 representation. Rejects anything that is not a
// finite float32, i.e. NaN, +/-Infinity and values that overflow the range.
// Rounding is expected and allowed: 16777217 becomes 16777216.
bool CastToFloat32(const std::vector<double>& input, std::vector<float>* out,
                   std::string* message);

}  // namespace mvdb
