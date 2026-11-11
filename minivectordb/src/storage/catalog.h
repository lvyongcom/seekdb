// Table definitions, persisted as one JSON document in the data directory.
//
// DDL is rare, so the whole catalog is rewritten atomically on every change
// (write temp + rename). Row data is *not* part of the catalog: it lives in the
// per-table redo logs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sql/value.h"

namespace mvdb {

struct TableDef {
  uint64_t id = 0;
  std::string name;
  std::vector<ColumnDef> columns;
  bool has_vector_index = false;
  std::string vector_index_name;
};

class Catalog {
 public:
  // A missing catalog file is not an error: it means an empty database.
  bool Load(const std::string& data_dir, std::string* error);
  bool Save(const std::string& data_dir, std::string* error);

  std::vector<TableDef>& tables() { return tables_; }
  const std::vector<TableDef>& tables() const { return tables_; }

  uint64_t next_table_id = 1;

 private:
  std::vector<TableDef> tables_;
};

}  // namespace mvdb
