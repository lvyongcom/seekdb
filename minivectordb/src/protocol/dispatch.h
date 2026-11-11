#pragma once

#include <string>

#include "db/database.h"

namespace mvdb {

// Turns one JSONL request into exactly one JSONL response.
//
// Contract from STDIO_PROTOCOL.md:
//   * every request gets exactly one response, echoing `id` with its JSON type
//   * `ok` is always a JSON boolean
//   * a failed statement must not kill the process
class Dispatcher {
 public:
  explicit Dispatcher(Database* db) : db_(db) {}

  // Returns true when the process should exit (after a successful shutdown).
  bool HandleLine(const std::string& line);

 private:
  void WriteError(const JsonValue& id, const std::string& code,
                  const std::string& message);
  void WritePlainOk(const JsonValue& id);

  Database* db_;
};

}  // namespace mvdb
