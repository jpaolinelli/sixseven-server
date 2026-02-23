#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/graph/graph_engine.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

// Forward declarations.
struct BoundStatement;
struct CreateDatabaseStmt;
struct CreateEdgeTypeStmt;
struct CreateTableStmt;
struct DropDatabaseStmt;
struct DropEdgeTypeStmt;
struct DropTableStmt;
struct LinkStmt;
struct ReembedStmt;
struct SetStmt;
struct ShowStmt;
struct UnlinkStmt;
class HnswIndex;
class ProviderCache;
class ProviderRegistry;
class SettingsCache;

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
    /// @param catalog       System catalog (schema metadata).
    /// @param storage       StorageManager (physical table storage).
    /// @param graph_engine  Optional GraphEngine for graph operations.
    QueryEngine(Catalog& catalog, StorageManager& storage, GraphEngine* graph_engine = nullptr);

    /// Execute a SQL statement string and return the result.
    /// Uses the current database context for name resolution.
    [[nodiscard]] Result<QueryResult> execute(const std::string& sql);

    /// Set the current database context by ID.
    void set_current_database(database_id_t database_id);

    /// Get the current database ID.
    [[nodiscard]] database_id_t current_database_id() const;

    /// Set the provider registry for embedding operations (REEMBED, NEAREST).
    void set_provider_registry(ProviderRegistry* registry);

    /// Set the HNSW index map for vector operations (REEMBED, NEAREST).
    void set_hnsw_indexes(std::unordered_map<std::string, HnswIndex*>* indexes);

    /// Set the settings cache for SET/SHOW commands.
    void set_settings_cache(SettingsCache* cache);

    /// Set the provider cache for SHOW PROVIDERS and provider management.
    void set_provider_cache(ProviderCache* cache);

private:
    /// Execute a SET parameter = value statement.
    [[nodiscard]] Result<QueryResult> execute_set(const SetStmt& stmt);

    /// Execute a SHOW statement (SHOW parameter, SHOW ALL, SHOW TABLES, etc.).
    [[nodiscard]] Result<QueryResult> execute_show(const ShowStmt& stmt);

    /// Execute a DDL CREATE DATABASE statement.
    [[nodiscard]] Result<QueryResult> execute_create_database(const CreateDatabaseStmt& stmt);

    /// Execute a DDL DROP DATABASE statement.
    [[nodiscard]] Result<QueryResult> execute_drop_database(const DropDatabaseStmt& stmt);

    /// Execute a DDL CREATE TABLE statement.
    [[nodiscard]] Result<QueryResult> execute_create_table(const CreateTableStmt& stmt);

    /// Execute a DDL DROP TABLE statement.
    [[nodiscard]] Result<QueryResult> execute_drop_table(const DropTableStmt& stmt);

    /// Execute a DDL CREATE EDGE TYPE statement.
    [[nodiscard]] Result<QueryResult> execute_create_edge_type(const CreateEdgeTypeStmt& stmt);

    /// Execute a DDL DROP EDGE TYPE statement.
    [[nodiscard]] Result<QueryResult> execute_drop_edge_type(const DropEdgeTypeStmt& stmt);

    /// Execute a LINK statement (create edge).
    [[nodiscard]] Result<QueryResult> execute_link(const LinkStmt& stmt,
                                                   const BoundStatement& bound);

    /// Execute an UNLINK statement (delete edge).
    [[nodiscard]] Result<QueryResult> execute_unlink(const UnlinkStmt& stmt,
                                                     const BoundStatement& bound);

    /// Execute a REEMBED TABLE statement (bulk embedding regeneration).
    [[nodiscard]] Result<QueryResult> execute_reembed(const ReembedStmt& stmt);

    /// Execute a DML/query via the Planner + Iterator pipeline.
    /// After successful DML on sys_providers, automatically reloads the provider cache.
    [[nodiscard]] Result<QueryResult> execute_plan(const BoundStatement& bound);

    /// Invalidate the provider cache after DML on sys_providers.
    void maybe_invalidate_provider_cache(const BoundStatement& bound);

    Catalog& catalog_;
    StorageManager& storage_;
    GraphEngine* graph_engine_;
    ProviderRegistry* provider_registry_ = nullptr;
    std::unordered_map<std::string, HnswIndex*>* hnsw_indexes_ = nullptr;
    SettingsCache* settings_cache_ = nullptr;
    ProviderCache* provider_cache_ = nullptr;
    database_id_t current_database_id_ = default_database_id;
};

} // namespace giodb
