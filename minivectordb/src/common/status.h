#pragma once

#include <string>
#include <utility>

namespace mvdb {

// Return status of an operation. `code` is the protocol-level error code that
// ends up in `error.code`, so it must always be one of protocol/errors.h.
class Status {
 public:
  Status() = default;

  static Status Ok() { return Status(); }

  static Status Error(std::string code, std::string message) {
    Status status;
    status.code_ = std::move(code);
    status.message_ = std::move(message);
    return status;
  }

  bool ok() const { return code_.empty(); }
  const std::string& code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  std::string code_;
  std::string message_;
};

}  // namespace mvdb
