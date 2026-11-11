#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mvdb {

enum class TokenKind {
  End,
  Identifier,
  Number,
  String,
  LParen,
  RParen,
  Comma,
  Semicolon,
  Star,
  Eq,
  Lt,
  Le,
  Gt,
  Ge,
};

struct Token {
  TokenKind kind = TokenKind::End;
  // Identifier spelling, string contents, or the raw number lexeme.
  std::string text;
  int64_t integer = 0;
  double number = 0.0;
  bool is_float = false;
  size_t position = 0;  // byte offset, for error messages
};

// Tokenizes a single SQL statement.
//
// Keywords are deliberately not recognized here: the parser compares
// identifiers case-insensitively, which keeps this layer small and lets a
// column be named "limit" without special cases.
bool Tokenize(const std::string& sql, std::vector<Token>* tokens, std::string* error);

}  // namespace mvdb
