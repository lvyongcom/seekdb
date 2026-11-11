#include "db/database.h"

#include <cstdio>
#include <string>

#include "platform/fs.h"
#include "protocol/errors.h"
#include "sql/parser.h"

namespace mvdb {

Status Database::Open(const std::string& data_dir) {
  data_dir_ = data_dir;
  if (!EnsureDirectory(data_dir_)) {
    return Status::Error(errc::kInternal, "cannot create data directory: " + data_dir_);
  }

  // Loads the catalog and replays every redo log. Nothing may be reported as
  // ready before this returns.
  const Status status = engine_.Open(data_dir_);
  if (!status.ok()) return status;

  opened_ = true;
  std::fprintf(stderr, "[mvdb] data dir ready: %s\n", data_dir_.c_str());
  return Status::Ok();
}

Status Database::Execute(const std::string& sql, const std::string& search_mode,
                         StatementResult* result) {
  if (!opened_) return Status::Error(errc::kInternal, "database is not open");
  if (sql.empty()) return Status::Error(errc::kInvalidRequest, "empty sql statement");

  Statement statement;
  std::string error_code;
  std::string message;
  if (!ParseStatement(sql, &statement, &error_code, &message)) {
    return Status::Error(error_code.empty() ? errc::kSyntax : error_code, message);
  }
  return engine_.Execute(statement, search_mode, result);
}

Status Database::Shutdown() {
  if (opened_) engine_.Close();
  opened_ = false;
  return Status::Ok();
}

}  // namespace mvdb
