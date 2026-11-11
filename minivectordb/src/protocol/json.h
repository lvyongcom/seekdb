// Minimal, dependency-free JSON (RFC 8259) value + parser + serializer.
//
// The tournament judge is strict about the wire format (see STDIO_PROTOCOL.md):
//   * one JSON object per physical line, UTF-8, LF terminated
//   * no NaN / Infinity, no duplicate object keys
//   * the request `id` must be echoed back with the *same* JSON type
// So this layer keeps the id as a full JsonValue instead of a string.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mvdb {

enum class JsonType { Null, Bool, Int, Double, String, Array, Object };

struct JsonValue {
  JsonType type = JsonType::Null;
  bool b = false;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  bool IsNull() const { return type == JsonType::Null; }
  bool IsString() const { return type == JsonType::String; }
  bool IsObject() const { return type == JsonType::Object; }

  // Returns nullptr when this is not an object or the key is absent.
  const JsonValue* Find(const std::string& key) const;
};

JsonValue MakeNull();
JsonValue MakeBool(bool value);
JsonValue MakeInt(int64_t value);
JsonValue MakeDouble(double value);
JsonValue MakeString(std::string value);
JsonValue MakeArray();
JsonValue MakeObject();

// Parses exactly one JSON value and requires the whole input to be consumed.
// Returns false and fills `error` on malformed input.
bool ParseJson(const std::string& text, JsonValue* out, std::string* error);

std::string SerializeJson(const JsonValue& value);

// Appends `text` as a quoted JSON string to `out` (escapes control chars).
void AppendJsonString(std::string* out, const std::string& text);

// Formats a double as a JSON number. Never emits NaN/Infinity (returns "null").
std::string FormatDouble(double value);

}  // namespace mvdb
