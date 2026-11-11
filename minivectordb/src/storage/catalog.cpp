#include "storage/catalog.h"

#include <algorithm>
#include <utility>

#include "platform/fs.h"
#include "protocol/json.h"

namespace mvdb {
namespace {

const char* const kCatalogFile = "CATALOG";

const char* TypeName(ColumnType type) {
  switch (type) {
    case ColumnType::BigInt: return "bigint";
    case ColumnType::VarChar: return "varchar";
    case ColumnType::Vector: return "vector";
  }
  return "bigint";
}

bool ParseType(const std::string& text, ColumnType* out) {
  if (text == "bigint") {
    *out = ColumnType::BigInt;
    return true;
  }
  if (text == "varchar") {
    *out = ColumnType::VarChar;
    return true;
  }
  if (text == "vector") {
    *out = ColumnType::Vector;
    return true;
  }
  return false;
}

bool RequireString(const JsonValue& object, const char* key, std::string* out) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr || !value->IsString()) return false;
  *out = value->s;
  return true;
}

bool RequireInt(const JsonValue& object, const char* key, int64_t* out) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr || value->type != JsonType::Int) return false;
  *out = value->i;
  return true;
}

}  // namespace

bool Catalog::Load(const std::string& data_dir, std::string* error) {
  tables_.clear();
  next_table_id = 1;

  const std::string path = JoinPath(data_dir, kCatalogFile);
  std::string text;
  if (!ReadFileToString(path, &text)) {
    return true;  // empty database
  }
  if (text.empty()) return true;

  JsonValue root;
  std::string parse_error;
  if (!ParseJson(text, &root, &parse_error) || !root.IsObject()) {
    if (error != nullptr) *error = "malformed catalog: " + parse_error;
    return false;
  }

  int64_t next_id = 1;
  if (RequireInt(root, "next_table_id", &next_id) && next_id > 0) {
    next_table_id = static_cast<uint64_t>(next_id);
  }

  const JsonValue* tables = root.Find("tables");
  if (tables == nullptr || tables->type != JsonType::Array) {
    if (error != nullptr) *error = "catalog has no table list";
    return false;
  }

  uint64_t highest_id = 0;
  for (const JsonValue& entry : tables->arr) {
    if (!entry.IsObject()) continue;
    TableDef def;

    int64_t id = 0;
    if (!RequireInt(entry, "id", &id) || id <= 0) {
      if (error != nullptr) *error = "catalog table has no usable id";
      return false;
    }
    def.id = static_cast<uint64_t>(id);
    if (!RequireString(entry, "name", &def.name)) {
      if (error != nullptr) *error = "catalog table has no name";
      return false;
    }

    const JsonValue* columns = entry.Find("columns");
    if (columns == nullptr || columns->type != JsonType::Array) {
      if (error != nullptr) *error = "catalog table has no columns";
      return false;
    }
    for (const JsonValue& column : columns->arr) {
      if (!column.IsObject()) continue;
      ColumnDef definition;
      std::string type_name;
      if (!RequireString(column, "name", &definition.name) ||
          !RequireString(column, "type", &type_name) ||
          !ParseType(type_name, &definition.type)) {
        if (error != nullptr) *error = "catalog column is malformed";
        return false;
      }
      int64_t length = 0;
      if (RequireInt(column, "length", &length) && length >= 0) {
        definition.length = static_cast<uint32_t>(length);
      }
      const JsonValue* primary = column.Find("primary_key");
      definition.primary_key = primary != nullptr && primary->type == JsonType::Bool && primary->b;
      def.columns.push_back(std::move(definition));
    }

    if (const JsonValue* index = entry.Find("vector_index");
        index != nullptr && index->IsString()) {
      def.has_vector_index = true;
      def.vector_index_name = index->s;
    }

    highest_id = std::max(highest_id, def.id);
    tables_.push_back(std::move(def));
  }

  if (next_table_id <= highest_id) next_table_id = highest_id + 1;
  return true;
}

bool Catalog::Save(const std::string& data_dir, std::string* error) {
  JsonValue root = MakeObject();
  root.obj.emplace_back("next_table_id", MakeInt(static_cast<int64_t>(next_table_id)));

  JsonValue table_list = MakeArray();
  for (const TableDef& def : tables_) {
    JsonValue entry = MakeObject();
    entry.obj.emplace_back("id", MakeInt(static_cast<int64_t>(def.id)));
    entry.obj.emplace_back("name", MakeString(def.name));

    JsonValue column_list = MakeArray();
    for (const ColumnDef& column : def.columns) {
      JsonValue item = MakeObject();
      item.obj.emplace_back("name", MakeString(column.name));
      item.obj.emplace_back("type", MakeString(TypeName(column.type)));
      item.obj.emplace_back("length", MakeInt(static_cast<int64_t>(column.length)));
      if (column.primary_key) {
        item.obj.emplace_back("primary_key", MakeBool(true));
      }
      column_list.arr.push_back(std::move(item));
    }
    entry.obj.emplace_back("columns", std::move(column_list));
    if (def.has_vector_index) {
      entry.obj.emplace_back("vector_index", MakeString(def.vector_index_name));
    }
    table_list.arr.push_back(std::move(entry));
  }
  root.obj.emplace_back("tables", std::move(table_list));

  const std::string path = JoinPath(data_dir, kCatalogFile);
  if (!WriteFileAtomically(path, SerializeJson(root) + "\n")) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace mvdb
