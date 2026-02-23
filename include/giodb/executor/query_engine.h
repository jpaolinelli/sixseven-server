#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/storage_manager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace giodb {

// Forward declarations.
struct BoundStatement;
struct CreateDatabaseStmt;
struct CreateTableStmt;
struct DropDatabaseStmt;
struct DropTableStmt;

/// Result of executing a SQL statement.
struct QueryResult {
    /// Column names for SELECT queries.
    std::vector<std::string> column_names;

    /// Column types for SELECT queries.
    std::vector<TypeId> column_types;

    /// Result rows for SELECT queries.
    std::vector<std::vector<Value>> rows;

    /// Affected row count for DML statements (INSERT/UPDATE/DELETE).
    /// -1 for queries (SELECT) and DDL.
    int64_t affected_rows = -1;

    /// Human-readable message for DDL/utility statements.
    std::string message;
};

/// Top-level query execution engine.
///
/// Ties together the full SQL pipeline:
///   SQL string → Lexer → Parser → Binder → Planner → Executor → QueryResult
///
/// Also handles DDL statements (CREATE/DROP TABLE, CREATE/DROP DATABASE)
/// which bypass the planner and directly modify the Catalog + StorageManager.
class QueryEngine {
public:
    /// @param catalog  System catalog (schema metadata).
    /// @param storage  StorageManager (physical table storage).
    QueryEngine(Catalog& catalog, StorageManager& storage);

    /// Execute a SQL statement string and return the result.
    /// Uses the current database context for name resolution.
    [[nodiscard]] Result<QueryResult> execute(const std::string& sql);

    /// Set the current database context by ID.
    void set_current_database(database_id_t database_id);

    /// Get the current database ID.
    [[nodiscard]] database_id_t current_database_id() const;

private:
    /// Execute a DDL CREATE DATABASE statement.
    [[nodiscard]] Result<QueryResult> execute_create_database(const CreateDatabaseStmt& stmt);

    /// Execute a DDL DROP DATABASE statement.
    [[nodiscard]] Result<QueryResult> execute_drop_database(const DropDatabaseStmt& stmt);

    /// Execute a DDL CREATE TABLE statement.
    [[nodiscard]] Result<QueryResult> execute_create_table(const CreateTableStmt& stmt);

    /// Execute a DDL DROP TABLE statement.
    [[nodiscard]] Result<QueryResult> execute_drop_table(const DropTableStmt& stmt);

    /// Execute a DML/query via the Planner + Iterator pipeline.
    [[nodiscard]] Result<QueryResult> execute_plan(const BoundStatement& bound);

    Catalog& catalog_;
    StorageManager& storage_;
    database_id_t current_database_id_ = default_database_id;
};

} // namespace giodb
