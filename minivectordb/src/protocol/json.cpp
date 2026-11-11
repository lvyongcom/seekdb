#include "protocol/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mvdb {
namespace {

constexpr int kMaxDepth = 64;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp <= 0x7F) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  bool Parse(JsonValue* out) {
    SkipWs();
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    if (pos_ != text_.size()) return Fail("trailing characters");
    return true;
  }

  const std::string& error() const { return error_; }

 private:
  bool Fail(const char* message) {
    if (error_.empty()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s at offset %zu", message, pos_);
      error_ = buf;
    }
    return false;
  }

  void SkipWs() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Peek(char c) const { return pos_ < text_.size() && text_[pos_] == c; }

  bool Consume(char c) {
    if (!Peek(c)) return Fail("unexpected character");
    ++pos_;
    return true;
  }

  bool ParseLiteral(const char* literal, size_t length) {
    if (text_.compare(pos_, length, literal) != 0) return Fail("invalid literal");
    pos_ += length;
    return true;
  }

  bool ParseValue(JsonValue* out, int depth) {
    if (depth > kMaxDepth) return Fail("nesting too deep");
    SkipWs();
    if (pos_ >= text_.size()) return Fail("unexpected end of input");
    switch (text_[pos_]) {
      case '{': return ParseObject(out, depth);
      case '[': return ParseArray(out, depth);
      case '"':
        out->type = JsonType::String;
        return ParseString(&out->s);
      case 't':
        out->type = JsonType::Bool;
        out->b = true;
        return ParseLiteral("true", 4);
      case 'f':
        out->type = JsonType::Bool;
        out->b = false;
        return ParseLiteral("false", 5);
      case 'n':
        out->type = JsonType::Null;
        return ParseLiteral("null", 4);
      default: return ParseNumber(out);
    }
  }

  bool ParseObject(JsonValue* out, int depth) {
    out->type = JsonType::Object;
    out->obj.clear();
    ++pos_;  // '{'
    SkipWs();
    if (Peek('}')) {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWs();
      if (!Peek('"')) return Fail("expected object key");
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWs();
      if (!Consume(':')) return false;
      JsonValue value;
      if (!ParseValue(&value, depth + 1)) return false;
      out->obj.emplace_back(std::move(key), std::move(value));
      SkipWs();
      if (Peek(',')) {
        ++pos_;
        continue;
      }
      if (Peek('}')) {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(JsonValue* out, int depth) {
    out->type = JsonType::Array;
    out->arr.clear();
    ++pos_;  // '['
    SkipWs();
    if (Peek(']')) {
      ++pos_;
      return true;
    }
    while (true) {
      JsonValue value;
      if (!ParseValue(&value, depth + 1)) return false;
      out->arr.push_back(std::move(value));
      SkipWs();
      if (Peek(',')) {
        ++pos_;
        continue;
      }
      if (Peek(']')) {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseHex4(uint32_t* out) {
    if (pos_ + 4 > text_.size()) return Fail("truncated unicode escape");
    uint32_t value = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = text_[pos_ + k];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return Fail("invalid hex digit");
      }
    }
    pos_ += 4;
    *out = value;
    return true;
  }

  bool ParseString(std::string* out) {
    if (!Consume('"')) return false;
    out->clear();
    while (true) {
      if (pos_ >= text_.size()) return Fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) return Fail("unterminated escape");
        const char esc = text_[pos_++];
        switch (esc) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!ParseHex4(&cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                pos_ += 2;
                uint32_t low = 0;
                if (!ParseHex4(&low)) return false;
                if (low < 0xDC00 || low > 0xDFFF) return Fail("invalid surrogate pair");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              } else {
                return Fail("unpaired high surrogate");
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return Fail("unpaired low surrogate");
            }
            AppendUtf8(out, cp);
            break;
          }
          default: return Fail("invalid escape");
        }
        continue;
      }
      if (c < 0x20) return Fail("control character in string");
      out->push_back(static_cast<char>(c));
      ++pos_;
    }
  }

  bool ParseNumber(JsonValue* out) {
    const size_t start = pos_;
    bool is_double = false;
    if (Peek('-')) ++pos_;
    if (pos_ >= text_.size()) return Fail("invalid number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (IsDigit(text_[pos_])) {
      while (pos_ < text_.size() && IsDigit(text_[pos_])) ++pos_;
    } else {
      return Fail("invalid number");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_double = true;
      ++pos_;
      if (pos_ >= text_.size() || !IsDigit(text_[pos_])) return Fail("invalid number");
      while (pos_ < text_.size() && IsDigit(text_[pos_])) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      is_double = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || !IsDigit(text_[pos_])) return Fail("invalid number");
      while (pos_ < text_.size() && IsDigit(text_[pos_])) ++pos_;
    }

    const std::string token = text_.substr(start, pos_ - start);
    if (!is_double) {
      errno = 0;
      char* end = nullptr;
      const long long value = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end != nullptr && *end == '\0') {
        out->type = JsonType::Int;
        out->i = static_cast<int64_t>(value);
        return true;
      }
      // Falls through: integer literal too large for int64, keep it as a double.
    }

    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (errno != 0 || end == nullptr || *end != '\0' || !std::isfinite(value)) {
      return Fail("invalid number");
    }
    out->type = JsonType::Double;
    out->d = value;
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

void AppendJsonValue(std::string* out, const JsonValue& value);

}  // namespace

const JsonValue* JsonValue::Find(const std::string& key) const {
  if (type != JsonType::Object) return nullptr;
  for (const auto& entry : obj) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

JsonValue MakeNull() { return JsonValue(); }

JsonValue MakeBool(bool value) {
  JsonValue v;
  v.type = JsonType::Bool;
  v.b = value;
  return v;
}

JsonValue MakeInt(int64_t value) {
  JsonValue v;
  v.type = JsonType::Int;
  v.i = value;
  return v;
}

JsonValue MakeDouble(double value) {
  JsonValue v;
  v.type = JsonType::Double;
  v.d = value;
  return v;
}

JsonValue MakeString(std::string value) {
  JsonValue v;
  v.type = JsonType::String;
  v.s = std::move(value);
  return v;
}

JsonValue MakeArray() {
  JsonValue v;
  v.type = JsonType::Array;
  return v;
}

JsonValue MakeObject() {
  JsonValue v;
  v.type = JsonType::Object;
  return v;
}

bool ParseJson(const std::string& text, JsonValue* out, std::string* error) {
  Parser parser(text);
  if (!parser.Parse(out)) {
    if (error != nullptr) *error = parser.error();
    return false;
  }
  return true;
}

void AppendJsonString(std::string* out, const std::string& text) {
  out->push_back('"');
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out->append(buf);
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

std::string FormatDouble(double value) {
  if (!std::isfinite(value)) return "null";
  char buf[48];
  // Keep integral distances readable as "5.0" (matches the expectation files).
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
  }
  for (int precision = 15; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
    if (std::strtod(buf, nullptr) == value) return buf;
  }
  std::snprintf(buf, sizeof(buf), "%.17g", value);
  return buf;
}

namespace {

void AppendJsonValue(std::string* out, const JsonValue& value) {
  switch (value.type) {
    case JsonType::Null: out->append("null"); return;
    case JsonType::Bool: out->append(value.b ? "true" : "false"); return;
    case JsonType::Int: out->append(std::to_string(value.i)); return;
    case JsonType::Double: out->append(FormatDouble(value.d)); return;
    case JsonType::String: AppendJsonString(out, value.s); return;
    case JsonType::Array:
      out->push_back('[');
      for (size_t k = 0; k < value.arr.size(); ++k) {
        if (k != 0) out->push_back(',');
        AppendJsonValue(out, value.arr[k]);
      }
      out->push_back(']');
      return;
    case JsonType::Object:
      out->push_back('{');
      for (size_t k = 0; k < value.obj.size(); ++k) {
        if (k != 0) out->push_back(',');
        AppendJsonString(out, value.obj[k].first);
        out->push_back(':');
        AppendJsonValue(out, value.obj[k].second);
      }
      out->push_back('}');
      return;
  }
}

}  // namespace

std::string SerializeJson(const JsonValue& value) {
  std::string out;
  out.reserve(128);
  AppendJsonValue(&out, value);
  return out;
}

}  // namespace mvdb
