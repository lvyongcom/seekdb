#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/result.h"
#include "common/status.h"
#include "sql/ast.h"
#include "sql/value.h"
#include "storage/catalog.h"
#include "storage/table_store.h"

namespace mvdb {

struct Table {
  uint64_t id = 0;
  std::string name;
  std::vector<ColumnDef> columns;
  size_t primary_key_index = 0;
  int vector_column = -1;
  bool has_vector_index = false;
  std::string vector_index_name;

  // Row bodies live on disk; only this store's key -> offset map is resident.
  TableStore store;
};

// Executes parsed statements against disk-resident tables.
//
// Durability: a mutation is appended to the table's record file and flushed
// before the response is written, so an acknowledged statement survives a hard
// kill. Recovery is a single streaming pass per table.
//
// Transactions: because row bodies are on disk immediately, an uncommitted
// transaction cannot be "never written". Instead BEGIN remembers each table's
// end-of-file offset, the undo list remembers each row's previous offset, and
// ROLLBACK restores the key map and truncates the files back to those offsets.
// That keeps memory proportional to the number of touched rows, not to the data
// set size.
class Engine {
 public:
  Status Open(const std::string& data_dir);
  Status Close();

  Status Execute(const Statement& statement, const std::string& search_mode,
                 StatementResult* result);

  Status BeginTransaction();
  Status CommitTransaction();
  Status RollbackTransaction();

 private:
  Status CreateTable(const CreateTableStmt& statement, StatementResult* result);
  Status CreateVectorIndex(const CreateVectorIndexStmt& statement,
                           StatementResult* result);
  Status Insert(const InsertStmt& statement, StatementResult* result);
  Status Select(const SelectStmt& statement, StatementResult* result);
  Status Update(const UpdateStmt& statement, StatementResult* result);
  Status Delete(const DeleteStmt& statement, StatementResult* result);

  Table* FindTable(const std::string& name);
  Table* FindTableById(uint64_t id);
  int FindColumn(const Table& table, const std::string& name) const;

  Status PersistCatalog();
  Status FlushTable(Table& table);

  // Records the pre-mutation state needed by ROLLBACK. Must be called before
  // the record is appended.
  void NoteUndo(const Table& table, int64_t key);
  // Marks a table's end-of-file offset the first time a transaction touches it,
  // and persists the marker so a hard kill can undo by truncation too.
  Status NoteBeginMark(const Table& table);

  // /data/TXN holds the begin offsets of the active transaction. It exists only
  // while a transaction is open, which is what makes crash recovery able to tell
  // committed records from uncommitted ones.
  Status WriteTxnMarker();
  Status ClearTxnMarker();

  struct UndoEntry {
    uint64_t table_id = 0;
    int64_t key = 0;
    bool existed_before = false;
    uint64_t previous_offset = 0;
  };

  struct BeginMark {
    uint64_t table_id = 0;
    uint64_t end_offset = 0;
  };

  std::vector<std::unique_ptr<Table>> tables_;
  Catalog catalog_;
  std::string data_dir_;

  bool in_transaction_ = false;
  std::vector<UndoEntry> undo_;
  std::vector<BeginMark> begin_marks_;
};

}  // namespace mvdb
