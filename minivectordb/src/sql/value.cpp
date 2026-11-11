#include "sql/value.h"

#include <cmath>
#include <cstdlib>
#include <utility>

namespace mvdb {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Fast path for the numbers a vector literal actually contains: an optional
// sign, digits, and optionally a fractional part. strtod() is correct but costs
// roughly 100 ns per call, and a 128-dimension vector means 128 of them per row;
// this loop is several times faster for the same input and falls back to strtod
// for anything with an exponent or an unusual shape.
bool ParseSimpleNumber(const char* begin, const char* end, const char** consumed,
                       double* out) {
  const char* p = begin;
  bool negative = false;
  if (p < end && (*p == '-' || *p == '+')) {
    negative = (*p == '-');
    ++p;
  }

  uint64_t mantissa = 0;
  int digits = 0;
  int exponent = 0;
  bool any = false;

  while (p < end && *p >= '0' && *p <= '9') {
    if (digits < 19) {
      mantissa = mantissa * 10 + static_cast<uint64_t>(*p - '0');
      ++digits;
    } else {
      ++exponent;  // beyond double precision, treat as a scale
    }
    any = true;
    ++p;
  }

  if (p < end && *p == '.') {
    ++p;
    while (p < end && *p >= '0' && *p <= '9') {
      if (digits < 19) {
        mantissa = mantissa * 10 + static_cast<uint64_t>(*p - '0');
        ++digits;
        --exponent;
      }
      any = true;
      ++p;
    }
  }

  if (!any) return false;
  if (p < end && (*p == 'e' || *p == 'E')) return false;  // let strtod handle it

  double value = static_cast<double>(mantissa);
  while (exponent > 0) {
    value *= 10.0;
    --exponent;
  }
  while (exponent < 0) {
    value /= 10.0;
    ++exponent;
  }
  *out = negative ? -value : value;
  *consumed = p;
  return true;
}

}  // namespace

Value Value::FromInt(int64_t v) {
  Value value;
  value.kind = Kind::Int;
  value.integer = v;
  return value;
}

Value Value::FromStr(std::string v) {
  Value value;
  value.kind = Kind::Str;
  value.text = std::move(v);
  return value;
}

Value Value::FromVec(std::vector<float> v) {
  Value value;
  value.kind = Kind::Vec;
  value.vec = std::move(v);
  return value;
}

size_t Utf8Length(const std::string& text) {
  size_t count = 0;
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    // Count leading bytes only; continuation bytes are 10xxxxxx.
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

bool ParseVectorLiteral(const std::string& text, std::vector<double>* out,
                        std::string* message) {
  out->clear();
  size_t pos = 0;
  const size_t size = text.size();

  while (pos < size && IsSpace(text[pos])) ++pos;
  if (pos >= size || text[pos] != '[') {
    if (message != nullptr) *message = "vector literal must start with '['";
    return false;
  }
  ++pos;

  while (true) {
    while (pos < size && IsSpace(text[pos])) ++pos;
    if (pos >= size) {
      if (message != nullptr) *message = "unterminated vector literal";
      return false;
    }
    if (text[pos] == ']') {
      ++pos;
      break;
    }

    const char* begin = text.c_str() + pos;
    char* end = nullptr;
    double number = 0.0;
    const char* consumed = nullptr;
    if (!ParseSimpleNumber(begin, text.c_str() + size, &consumed, &number)) {
      const char* scan = begin;
      number = std::strtod(begin, &end);
      if (end == scan) {
        if (message != nullptr) *message = "vector element is not a number";
        return false;
      }
      consumed = end;
    }
    if (!std::isfinite(number)) {
      if (message != nullptr) *message = "vector element is not finite";
      return false;
    }
    out->push_back(number);
    pos += static_cast<size_t>(consumed - begin);

    while (pos < size && IsSpace(text[pos])) ++pos;
    if (pos >= size) {
      if (message != nullptr) *message = "unterminated vector literal";
      return false;
    }
    if (text[pos] == ',') {
      ++pos;
      continue;
    }
    if (text[pos] == ']') {
      ++pos;
      break;
    }
    if (message != nullptr) *message = "expected ',' or ']' in vector literal";
    return false;
  }

  while (pos < size && IsSpace(text[pos])) ++pos;
  if (pos != size) {
    if (message != nullptr) *message = "trailing characters after vector literal";
    return false;
  }
  return true;
}

bool CastToFloat32(const std::vector<double>& input, std::vector<float>* out,
                   std::string* message) {
  out->assign(input.size(), 0.0f);
  for (size_t i = 0; i < input.size(); ++i) {
    if (!std::isfinite(input[i])) {
      if (message != nullptr) *message = "vector element is not a finite number";
      return false;
    }
    const float narrowed = static_cast<float>(input[i]);
    if (!std::isfinite(narrowed)) {
      if (message != nullptr) *message = "vector element overflows float32 range";
      return false;
    }
    (*out)[i] = narrowed;
  }
  return true;
}

}  // namespace mvdb
