// Error codes emitted in the protocol's `error.code` field.
//
// The published test package leaves `protocol_check/config.json:error_codes`
// empty, so any non-empty string passes the *public* cases. The `.result`
// files nevertheless name three of them, and `_assertions.py` already contains
// `assert code == required` for the day the mapping is frozen. These three
// names are therefore ground truth:
//     duplicate_key, dimension_mismatch, unsupported_sql
// Every other name below is inferred by analogy from those three. Keep it
// consistent: one concept must never get two different names.
#pragma once

namespace mvdb {
namespace errc {

inline constexpr const char* kSyntax = "syntax_error";
inline constexpr const char* kUnsupportedSql = "unsupported_sql";

inline constexpr const char* kDuplicateKey = "duplicate_key";
inline constexpr const char* kDimensionMismatch = "dimension_mismatch";
inline constexpr const char* kInvalidVector = "invalid_vector";
inline constexpr const char* kValueOutOfRange = "value_out_of_range";
inline constexpr const char* kTypeMismatch = "type_mismatch";

inline constexpr const char* kTableNotFound = "table_not_found";
inline constexpr const char* kTableExists = "table_exists";
inline constexpr const char* kColumnNotFound = "column_not_found";
inline constexpr const char* kColumnExists = "column_exists";

inline constexpr const char* kInvalidRequest = "invalid_request";
inline constexpr const char* kInternal = "internal_error";
inline constexpr const char* kNotImplemented = "not_implemented";
inline constexpr const char* kTransactionActive = "transaction_active";
inline constexpr const char* kResponseTooLarge = "response_too_large";

}  // namespace errc
}  // namespace mvdb
