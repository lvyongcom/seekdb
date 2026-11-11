#include "sql/parser.h"

#include <utility>

#include "protocol/errors.h"
#include "sql/lexer.h"

namespace mvdb {
namespace {

bool EqualsIgnoreCase(const std::string& text, const char* keyword) {
  size_t i = 0;
  for (; i < text.size() && keyword[i] != '\0'; ++i) {
    char a = text[i];
    char b = keyword[i];
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
    if (a != b) return false;
  }
  return i == text.size() && keyword[i] == '\0';
}

// Statements that are valid SQL but outside the required subset. Returning an
// explicit error is mandatory: silently doing something else is not allowed.
const char* const kUnsupportedLeading[] = {
    "ALTER", "DROP",   "TRUNCATE", "RENAME", "GRANT",  "REVOKE", "SHOW",
    "DESCRIBE", "DESC", "USE",     "SET",    "EXPLAIN", "VACUUM", "ANALYZE",
    "REPLACE", "MERGE", "WITH",    "CALL",   "PREPARE", "LOCK",   "UNLOCK",
};

class Parser {
 public:
  explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

  bool Parse(Statement* out) {
    if (!ParseLeading(out)) return false;
    // A single trailing ';' is allowed; anything after it is an error.
    Match(TokenKind::Semicolon);
    if (peek().kind != TokenKind::End) {
      if (peek().kind == TokenKind::Semicolon) {
        return Unsupported("multiple statements per request are not supported");
      }
      return Syntax("unexpected token after end of statement");
    }
    return true;
  }

  const std::string& code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  // -- token helpers -----------------------------------------------------
  const Token& peek(size_t ahead = 0) const {
    const size_t index = position_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
  }

  void advance() {
    if (position_ + 1 < tokens_.size()) ++position_;
  }

  bool Match(TokenKind kind) {
    if (peek().kind != kind) return false;
    advance();
    return true;
  }

  bool MatchKeyword(const char* keyword) {
    if (peek().kind != TokenKind::Identifier || !EqualsIgnoreCase(peek().text, keyword)) {
      return false;
    }
    advance();
    return true;
  }

  bool PeekKeyword(const char* keyword) const {
    return peek().kind == TokenKind::Identifier && EqualsIgnoreCase(peek().text, keyword);
  }

  bool Expect(TokenKind kind, const char* what) {
    if (Match(kind)) return true;
    return Syntax(std::string("expected ") + what);
  }

  bool ExpectIdentifier(std::string* out, const char* what) {
    if (peek().kind != TokenKind::Identifier) {
      return Syntax(std::string("expected ") + what);
    }
    *out = peek().text;
    advance();
    return true;
  }

  bool Fail(const char* code, std::string message) {
    if (code_.empty()) {
      code_ = code;
      message_ = std::move(message);
    }
    return false;
  }

  bool Syntax(std::string message) { return Fail(errc::kSyntax, std::move(message)); }

  bool Unsupported(std::string message) {
    return Fail(errc::kUnsupportedSql, std::move(message));
  }

  // -- values ------------------------------------------------------------
  bool ParseLiteral(Value* out) {
    const Token& token = peek();
    if (token.kind == TokenKind::Number) {
      if (token.is_float) {
        return Unsupported("fractional numeric literals are not supported");
      }
      *out = Value::FromInt(token.integer);
      advance();
      return true;
    }
    if (token.kind == TokenKind::String) {
      // A quoted string may be a VARCHAR value or a vector literal. The target
      // column type is unknown here, so keep both interpretations and let the
      // executor pick: `vec` for VECTOR columns, `text` for VARCHAR.
      std::vector<double> numbers;
      std::string message;
      if (!token.text.empty() && token.text[0] == '[' &&
          ParseVectorLiteral(token.text, &numbers, &message)) {
        std::vector<float> narrowed;
        if (!CastToFloat32(numbers, &narrowed, &message)) {
          return Fail(errc::kInvalidVector, message);
        }
        Value value = Value::FromVec(std::move(narrowed));
        value.text = token.text;
        *out = std::move(value);
        advance();
        return true;
      }
      *out = Value::FromStr(token.text);
      advance();
      return true;
    }
    return Syntax("expected a literal value");
  }

  // -- shared clauses ----------------------------------------------------
  bool ParseWhere(std::vector<Compare>* out) {
    if (!MatchKeyword("WHERE")) return true;
    while (true) {
      Compare compare;
      if (!ExpectIdentifier(&compare.column, "a column name in WHERE")) return false;
      compare.op = ParseCompareOp(&ok_);
      if (!ok_) return Syntax("expected a comparison operator");
      if (!ParseLiteral(&compare.literal)) return false;
      out->push_back(std::move(compare));

      if (MatchKeyword("AND")) continue;
      if (PeekKeyword("OR")) return Unsupported("OR is not supported");
      break;
    }
    return true;
  }

  CompareOp ParseCompareOp(bool* ok) {
    *ok = true;
    switch (peek().kind) {
      case TokenKind::Eq: advance(); return CompareOp::Eq;
      case TokenKind::Lt: advance(); return CompareOp::Lt;
      case TokenKind::Le: advance(); return CompareOp::Le;
      case TokenKind::Gt: advance(); return CompareOp::Gt;
      case TokenKind::Ge: advance(); return CompareOp::Ge;
      default:
        *ok = false;
        return CompareOp::Eq;
    }
  }

  // -- statement dispatch ------------------------------------------------
  bool ParseLeading(Statement* out) {
    if (peek().kind != TokenKind::Identifier) {
      return Syntax("statement must start with a keyword");
    }
    const std::string& word = peek().text;

    if (EqualsIgnoreCase(word, "CREATE")) return ParseCreate(out);
    if (EqualsIgnoreCase(word, "INSERT")) return ParseInsert(out);
    if (EqualsIgnoreCase(word, "SELECT")) return ParseSelect(out);
    if (EqualsIgnoreCase(word, "UPDATE")) return ParseUpdate(out);
    if (EqualsIgnoreCase(word, "DELETE")) return ParseDelete(out);
    if (EqualsIgnoreCase(word, "BEGIN")) {
      advance();
      TransactionStmt stmt;
      stmt.op = TransactionStmt::Op::Begin;
      *out = std::move(stmt);
      return true;
    }
    if (EqualsIgnoreCase(word, "COMMIT")) {
      advance();
      TransactionStmt stmt;
      stmt.op = TransactionStmt::Op::Commit;
      *out = std::move(stmt);
      return true;
    }
    if (EqualsIgnoreCase(word, "ROLLBACK")) {
      advance();
      TransactionStmt stmt;
      stmt.op = TransactionStmt::Op::Rollback;
      *out = std::move(stmt);
      return true;
    }
    for (const char* unsupported : kUnsupportedLeading) {
      if (EqualsIgnoreCase(word, unsupported)) {
        return Unsupported("statement is outside the supported SQL subset: " + word);
      }
    }
    return Syntax("unrecognised statement: " + word);
  }

  bool ParseCreate(Statement* out) {
    advance();  // CREATE
    if (MatchKeyword("TABLE")) return ParseCreateTable(out);
    if (MatchKeyword("VECTOR")) {
      if (!MatchKeyword("INDEX")) return Syntax("expected INDEX after CREATE VECTOR");
      return ParseCreateVectorIndex(out);
    }
    if (PeekKeyword("INDEX")) {
      return Unsupported("only CREATE VECTOR INDEX is supported");
    }
    return Unsupported("CREATE is limited to TABLE and VECTOR INDEX");
  }

  bool ParseColumnType(ColumnDef* column) {
    if (MatchKeyword("BIGINT")) {
      column->type = ColumnType::BigInt;
      return true;
    }
    if (MatchKeyword("VARCHAR")) {
      column->type = ColumnType::VarChar;
      if (!Expect(TokenKind::LParen, "'(' after VARCHAR")) return false;
      if (peek().kind != TokenKind::Number || peek().is_float || peek().integer <= 0) {
        return Syntax("VARCHAR length must be a positive integer");
      }
      column->length = static_cast<uint32_t>(peek().integer);
      advance();
      return Expect(TokenKind::RParen, "')' after VARCHAR length");
    }
    if (MatchKeyword("VECTOR")) {
      column->type = ColumnType::Vector;
      if (!Expect(TokenKind::LParen, "'(' after VECTOR")) return false;
      if (peek().kind != TokenKind::Number || peek().is_float || peek().integer <= 0) {
        return Syntax("VECTOR dimension must be a positive integer");
      }
      column->length = static_cast<uint32_t>(peek().integer);
      advance();
      return Expect(TokenKind::RParen, "')' after VECTOR dimension");
    }
    return Unsupported("column types are limited to BIGINT, VARCHAR(n) and VECTOR(n)");
  }

  bool ParseCreateTable(Statement* out) {
    CreateTableStmt stmt;
    if (!ExpectIdentifier(&stmt.table, "a table name")) return false;
    if (!Expect(TokenKind::LParen, "'(' after table name")) return false;

    while (true) {
      ColumnDef column;
      if (!ExpectIdentifier(&column.name, "a column name")) return false;
      for (const ColumnDef& existing : stmt.columns) {
        if (EqualsIgnoreCase(existing.name, column.name.c_str())) {
          return Fail(errc::kColumnExists, "duplicate column name: " + column.name);
        }
      }
      if (!ParseColumnType(&column)) return false;
      if (MatchKeyword("PRIMARY")) {
        if (!MatchKeyword("KEY")) return Syntax("expected KEY after PRIMARY");
        column.primary_key = true;
      }
      stmt.columns.push_back(std::move(column));

      if (Match(TokenKind::Comma)) continue;
      if (Match(TokenKind::RParen)) break;
      return Expect(TokenKind::RParen, "')' or ',' in column list");
    }

    // Schema constraints from the task description.
    size_t primary_keys = 0;
    size_t vector_columns = 0;
    for (const ColumnDef& column : stmt.columns) {
      if (column.primary_key) {
        ++primary_keys;
        if (column.type != ColumnType::BigInt) {
          return Unsupported("the primary key must be BIGINT");
        }
      }
      if (column.type == ColumnType::Vector) ++vector_columns;
    }
    if (primary_keys != 1) {
      return Unsupported("exactly one BIGINT PRIMARY KEY column is required");
    }
    if (vector_columns > 1) {
      return Unsupported("at most one VECTOR column per table is supported");
    }

    *out = std::move(stmt);
    return true;
  }

  bool ParseCreateVectorIndex(Statement* out) {
    CreateVectorIndexStmt stmt;
    if (!ExpectIdentifier(&stmt.index_name, "an index name")) return false;
    if (!MatchKeyword("ON")) return Syntax("expected ON in CREATE VECTOR INDEX");
    if (!ExpectIdentifier(&stmt.table, "a table name")) return false;
    if (!Expect(TokenKind::LParen, "'(' after table name")) return false;
    if (!ExpectIdentifier(&stmt.column, "a vector column name")) return false;
    if (!Expect(TokenKind::RParen, "')' after column name")) return false;
    *out = std::move(stmt);
    return true;
  }

  bool ParseInsert(Statement* out) {
    advance();  // INSERT
    if (!MatchKeyword("INTO")) return Syntax("expected INTO after INSERT");
    InsertStmt stmt;
    if (!ExpectIdentifier(&stmt.table, "a table name")) return false;

    if (Match(TokenKind::LParen)) {
      while (true) {
        std::string column;
        if (!ExpectIdentifier(&column, "a column name")) return false;
        stmt.columns.push_back(std::move(column));
        if (Match(TokenKind::Comma)) continue;
        if (Match(TokenKind::RParen)) break;
        return Expect(TokenKind::RParen, "')' or ',' in column list");
      }
    }

    if (!MatchKeyword("VALUES")) return Syntax("expected VALUES");
    while (true) {
      if (!Expect(TokenKind::LParen, "'(' starting a row")) return false;
      std::vector<Value> row;
      if (!Match(TokenKind::RParen)) {
        while (true) {
          Value value;
          if (!ParseLiteral(&value)) return false;
          row.push_back(std::move(value));
          if (Match(TokenKind::Comma)) continue;
          if (Match(TokenKind::RParen)) break;
          return Expect(TokenKind::RParen, "')' or ',' in VALUES");
        }
      }
      stmt.rows.push_back(std::move(row));
      if (Match(TokenKind::Comma)) continue;
      break;
    }

    if (stmt.rows.empty()) return Syntax("INSERT requires at least one row");
    *out = std::move(stmt);
    return true;
  }

  bool ParseSelectItem(SelectItem* item) {
    if (Match(TokenKind::Star)) {
      item->kind = SelectItem::Kind::Star;
      return true;
    }

    if (PeekKeyword("L2_DISTANCE")) {
      advance();
      if (!Expect(TokenKind::LParen, "'(' after L2_DISTANCE")) return false;
      if (!ExpectIdentifier(&item->column, "a vector column name")) return false;
      if (!Expect(TokenKind::Comma, "',' after the vector column")) return false;
      if (peek().kind != TokenKind::String) {
        return Syntax("L2_DISTANCE requires a quoted vector literal");
      }
      std::vector<double> numbers;
      std::string message;
      if (!ParseVectorLiteral(peek().text, &numbers, &message)) {
        return Fail(errc::kInvalidVector, message);
      }
      if (!CastToFloat32(numbers, &item->query, &message)) {
        return Fail(errc::kInvalidVector, message);
      }
      advance();
      if (!Expect(TokenKind::RParen, "')' after L2_DISTANCE arguments")) return false;
      item->kind = SelectItem::Kind::Distance;
    } else {
      if (!ExpectIdentifier(&item->column, "a column name or *")) return false;
      item->kind = SelectItem::Kind::Column;
    }

    if (MatchKeyword("AS")) {
      if (!ExpectIdentifier(&item->alias, "an alias after AS")) return false;
    }
    return true;
  }

  bool ParseSelect(Statement* out) {
    advance();  // SELECT
    SelectStmt stmt;

    while (true) {
      SelectItem item;
      if (!ParseSelectItem(&item)) return false;
      if (item.kind == SelectItem::Kind::Star && stmt.items.size() > 0) {
        return Unsupported("'*' must be the only projection");
      }
      stmt.items.push_back(std::move(item));
      if (Match(TokenKind::Comma)) continue;
      break;
    }

    if (!MatchKeyword("FROM")) return Syntax("expected FROM");
    if (!ExpectIdentifier(&stmt.table, "a table name")) return false;

    if (!ParseWhere(&stmt.where)) return false;

    if (MatchKeyword("ORDER")) {
      if (!MatchKeyword("BY")) return Syntax("expected BY after ORDER");
      while (true) {
        OrderItem order;
        if (!ExpectIdentifier(&order.name, "a column name in ORDER BY")) return false;
        if (MatchKeyword("DESC")) {
          order.descending = true;
        } else {
          MatchKeyword("ASC");
        }
        stmt.order.push_back(std::move(order));
        if (Match(TokenKind::Comma)) continue;
        break;
      }
    }

    if (MatchKeyword("LIMIT")) {
      if (peek().kind != TokenKind::Number || peek().is_float || peek().integer < 0) {
        return Syntax("LIMIT requires a non-negative integer");
      }
      stmt.limit = peek().integer;
      advance();
    }

    if (PeekKeyword("GROUP") || PeekKeyword("HAVING")) {
      return Unsupported("aggregation is not supported");
    }

    *out = std::move(stmt);
    return true;
  }

  bool ParseUpdate(Statement* out) {
    advance();  // UPDATE
    UpdateStmt stmt;
    if (!ExpectIdentifier(&stmt.table, "a table name")) return false;
    if (!MatchKeyword("SET")) return Syntax("expected SET");
    while (true) {
      std::string column;
      if (!ExpectIdentifier(&column, "a column name")) return false;
      if (!Expect(TokenKind::Eq, "'=' in SET")) return false;
      Value value;
      if (!ParseLiteral(&value)) return false;
      stmt.assignments.emplace_back(std::move(column), std::move(value));
      if (Match(TokenKind::Comma)) continue;
      break;
    }
    if (!ParseWhere(&stmt.where)) return false;
    *out = std::move(stmt);
    return true;
  }

  bool ParseDelete(Statement* out) {
    advance();  // DELETE
    if (!MatchKeyword("FROM")) return Syntax("expected FROM after DELETE");
    DeleteStmt stmt;
    if (!ExpectIdentifier(&stmt.table, "a table name")) return false;
    if (!ParseWhere(&stmt.where)) return false;
    *out = std::move(stmt);
    return true;
  }

  const std::vector<Token>& tokens_;
  size_t position_ = 0;
  bool ok_ = true;
  std::string code_;
  std::string message_;
};

}  // namespace

bool ParseStatement(const std::string& sql, Statement* out, std::string* error_code,
                    std::string* message) {
  std::vector<Token> tokens;
  std::string lexer_error;
  if (!Tokenize(sql, &tokens, &lexer_error)) {
    if (error_code != nullptr) *error_code = errc::kSyntax;
    if (message != nullptr) *message = lexer_error;
    return false;
  }

  Parser parser(tokens);
  if (!parser.Parse(out)) {
    if (error_code != nullptr) *error_code = parser.code().empty() ? errc::kSyntax : parser.code();
    if (message != nullptr) *message = parser.message();
    return false;
  }
  return true;
}

}  // namespace mvdb
