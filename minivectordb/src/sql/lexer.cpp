#include "sql/lexer.h"

#include <cerrno>
#include <cstdlib>

namespace mvdb {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsIdentifierStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentifierPart(char c) { return IsIdentifierStart(c) || IsDigit(c); }

}  // namespace

bool Tokenize(const std::string& sql, std::vector<Token>* tokens, std::string* error) {
  tokens->clear();
  size_t pos = 0;
  const size_t size = sql.size();

  while (pos < size) {
    const char c = sql[pos];

    if (IsSpace(c)) {
      ++pos;
      continue;
    }

    // "--" introduces a comment that runs to end of line.
    if (c == '-' && pos + 1 < size && sql[pos + 1] == '-') {
      while (pos < size && sql[pos] != '\n') ++pos;
      continue;
    }

    Token token;
    token.position = pos;

    if (c == '(' || c == ')' || c == ',' || c == ';' || c == '*') {
      token.kind = (c == '(') ? TokenKind::LParen
                 : (c == ')') ? TokenKind::RParen
                 : (c == ',') ? TokenKind::Comma
                 : (c == ';') ? TokenKind::Semicolon
                              : TokenKind::Star;
      token.text.assign(1, c);
      tokens->push_back(token);
      ++pos;
      continue;
    }

    if (c == '=') {
      token.kind = TokenKind::Eq;
      token.text = "=";
      tokens->push_back(token);
      ++pos;
      continue;
    }

    if (c == '<' || c == '>') {
      if (pos + 1 < size && sql[pos + 1] == '=') {
        token.kind = (c == '<') ? TokenKind::Le : TokenKind::Ge;
        token.text.assign(sql, pos, 2);
        pos += 2;
      } else {
        token.kind = (c == '<') ? TokenKind::Lt : TokenKind::Gt;
        token.text.assign(1, c);
        ++pos;
      }
      tokens->push_back(token);
      continue;
    }

    if (c == '\'') {
      ++pos;
      std::string text;
      bool closed = false;
      while (pos < size) {
        if (sql[pos] == '\'') {
          // '' is an escaped quote inside a string literal.
          if (pos + 1 < size && sql[pos + 1] == '\'') {
            text.push_back('\'');
            pos += 2;
            continue;
          }
          ++pos;
          closed = true;
          break;
        }
        text.push_back(sql[pos]);
        ++pos;
      }
      if (!closed) {
        if (error != nullptr) *error = "unterminated string literal";
        return false;
      }
      token.kind = TokenKind::String;
      token.text = std::move(text);
      tokens->push_back(token);
      continue;
    }

    // Numbers, including a leading '-' because the dialect has no arithmetic.
    if (IsDigit(c) || (c == '-' && pos + 1 < size && IsDigit(sql[pos + 1])) ||
        (c == '.' && pos + 1 < size && IsDigit(sql[pos + 1]))) {
      const size_t start = pos;
      if (sql[pos] == '-') ++pos;
      bool is_float = false;
      while (pos < size && IsDigit(sql[pos])) ++pos;
      if (pos < size && sql[pos] == '.') {
        is_float = true;
        ++pos;
        while (pos < size && IsDigit(sql[pos])) ++pos;
      }
      if (pos < size && (sql[pos] == 'e' || sql[pos] == 'E')) {
        is_float = true;
        ++pos;
        if (pos < size && (sql[pos] == '+' || sql[pos] == '-')) ++pos;
        if (pos >= size || !IsDigit(sql[pos])) {
          if (error != nullptr) *error = "malformed exponent in numeric literal";
          return false;
        }
        while (pos < size && IsDigit(sql[pos])) ++pos;
      }

      token.text.assign(sql, start, pos - start);
      token.kind = TokenKind::Number;
      if (is_float) {
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(token.text.c_str(), &end);
        if (errno != 0 || end == nullptr || *end != '\0') {
          if (error != nullptr) *error = "invalid numeric literal: " + token.text;
          return false;
        }
        token.number = value;
        token.is_float = true;
      } else {
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(token.text.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0') {
          if (error != nullptr) *error = "integer literal out of range: " + token.text;
          return false;
        }
        token.integer = static_cast<int64_t>(value);
      }
      tokens->push_back(token);
      continue;
    }

    if (IsIdentifierStart(c)) {
      const size_t start = pos;
      while (pos < size && IsIdentifierPart(sql[pos])) ++pos;
      token.kind = TokenKind::Identifier;
      token.text.assign(sql, start, pos - start);
      tokens->push_back(token);
      continue;
    }

    if (error != nullptr) {
      *error = std::string("unexpected character '") + c + "' at offset " +
               std::to_string(pos);
    }
    return false;
  }

  Token end;
  end.kind = TokenKind::End;
  end.position = size;
  tokens->push_back(end);
  return true;
}

}  // namespace mvdb
