// Abstract syntax tree for the supported SQL subset.
//
// Scope is deliberately narrow (see the design notes, section 4): one table per
// statement, AND-connected comparisons only, no joins, aggregates, subqueries,
// literals on the left of a comparison, or expressions.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "sql/value.h"

namespace mvdb {

enum class CompareOp { Eq, Lt, Le, Gt, Ge };

// `column <op> literal`, the only predicate form that exists in this dialect.
// WHERE a = 1 AND b >= 2 becomes a vector of these.
struct Compare {
  std::string column;
  CompareOp op = CompareOp::Eq;
  Value literal;
};

struct CreateTableStmt {
  std::string table;
  std::vector<ColumnDef> columns;
};

struct CreateVectorIndexStmt {
  std::string index_name;
  std::string table;
  std::string column;
};

struct InsertStmt {
  std::string table;
  // Empty means positional: values map to every column in declaration order.
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

struct SelectItem {
  enum class Kind { Star, Column, Distance };
  Kind kind = Kind::Column;
  std::string column;        // source column for Column and Distance
  std::string alias;         // optional AS name
  std::vector<float> query;  // Distance: the probe vector
};

struct OrderItem {
  std::string name;  // a column name or a SELECT alias
  bool descending = false;
};

struct SelectStmt {
  std::vector<SelectItem> items;
  std::string table;
  std::vector<Compare> where;
  std::vector<OrderItem> order;
  int64_t limit = -1;  // -1 means no LIMIT
};

struct UpdateStmt {
  std::string table;
  std::vector<std::pair<std::string, Value>> assignments;
  std::vector<Compare> where;
};

struct DeleteStmt {
  std::string table;
  std::vector<Compare> where;
};

struct TransactionStmt {
  enum class Op { Begin, Commit, Rollback };
  Op op = Op::Begin;
};

using Statement = std::variant<CreateTableStmt, CreateVectorIndexStmt, InsertStmt,
                               SelectStmt, UpdateStmt, DeleteStmt, TransactionStmt>;

}  // namespace mvdb
