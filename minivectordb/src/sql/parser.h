#pragma once

#include <string>

#include "sql/ast.h"

namespace mvdb {

// Parses one SQL statement into an AST.
//
// On failure `error_code` receives a value ready for the protocol's
// `error.code` field:
//   syntax_error     -- malformed SQL
//   unsupported_sql  -- valid SQL outside the supported subset
//   invalid_vector   -- malformed or non-finite vector literal
//   column_exists    -- duplicate column names in CREATE TABLE
bool ParseStatement(const std::string& sql, Statement* out, std::string* error_code,
                    std::string* message);

}  // namespace mvdb
