// Shape of a successful statement response, shared by the SQL and protocol
// layers. Kept in its own header so neither layer has to include the other.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/json.h"

namespace mvdb {

//   Plain    -> {"id":N,"ok":true}                        DDL, BEGIN, COMMIT, ...
//   Affected -> {"id":N,"ok":true,"affected_rows":K}       INSERT/UPDATE/DELETE
//   Rows     -> {"id":N,"ok":true,"columns":[...],"rows":[...]}
enum class ResultKind { Plain, Affected, Rows };

struct StatementResult {
  ResultKind kind = ResultKind::Plain;
  int64_t affected_rows = 0;
  std::vector<std::string> columns;
  std::vector<JsonValue> rows;  // every element is a JsonType::Array
};

}  // namespace mvdb
