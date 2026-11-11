#pragma once

#include <string>

#include "common/result.h"
#include "common/status.h"
#include "sql/engine.h"

namespace mvdb {

class Database {
 public:
  // Prepares the data directory and runs crash recovery.
  // The protocol's `{"type":"ready"}` line may only be emitted after this
  // returns Ok, because recovery (including index rebuild) must be finished.
  Status Open(const std::string& data_dir);

  Status Execute(const std::string& sql, const std::string& search_mode,
                 StatementResult* result);

  // Rolls back any open transaction and flushes state. Called on shutdown.
  Status Shutdown();

  const std::string& data_dir() const { return data_dir_; }

 private:
  bool opened_ = false;
  std::string data_dir_;
  Engine engine_;
};

}  // namespace mvdb
