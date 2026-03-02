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
class CatalogPersistence;
enum class AuthMethod : uint8_t;
struct AlterTableStmt;
struct AlterUserStmt;
struct BoundStatement;
struct CreateDatabaseStmt;
struct CreateEdgeTypeStmt;
struct CreateIndexStmt;
struct CreateTableStmt;
struct CreateUserStmt;
struct DropDatabaseStmt;
struct DropEdgeTypeStmt;
struct DropIndexStmt;
struct DropTableStmt;
struct DropUserStmt;
struct ExplainStmt;
struct LinkStmt;
struct ReembedStmt;
struct SetStmt;
struct ShowStmt;
struct UnlinkStmt;
class HnswIndex;
class UserManager;
class ProviderCache;
class ProviderRegistry;
class ReplicationSlotManager;
class SettingsCache;
class WalReceiver;
class WalSenderManager;
class WalWriter;

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

    /// Set the replication slot manager for SHOW REPLICATION SLOTS.
    void set_slot_manager(ReplicationSlotManager* slot_mgr);

    /// Set the WAL sender manager for SHOW REPLICATION STATUS (primary).
    void set_wal_sender_manager(WalSenderManager* sender_mgr);

    /// Set the WAL receiver for SHOW STANDBY STATUS (replica).
    void set_wal_receiver(WalReceiver* receiver);

    /// Set the WAL writer for pg_current_wal_lsn().
    void set_wal_writer(WalWriter* writer);

    /// Enable or disable standby (read-only) mode.
    /// When enabled, all write operations are rejected.
    void set_standby_mode(bool enabled);

    /// Return true if the engine is in standby (read-only) mode.
    [[nodiscard]] bool is_standby_mode() const;

    /// Increment the skip-masking counter. While > 0, api_key_encrypted
    /// values in SELECT results are not masked. Used by ProviderCache::load()
    /// which needs raw encrypted values.
    void push_skip_masking();

    /// Decrement the skip-masking counter.
    void pop_skip_masking();

    /// Set the user manager for CREATE/DROP/ALTER USER commands.
    void set_user_manager(UserManager* user_mgr);

    /// Set the authentication method used for password hashing in CREATE/ALTER USER.
    void set_auth_method(AuthMethod method);

    /// Set the catalog persistence layer for persisting DDL changes.
    void set_catalog_persistence(CatalogPersistence* persistence);

private:
    /// Execute a CREATE USER statement.
    [[nodiscard]] Result<QueryResult> execute_create_user(const CreateUserStmt& stmt);

    /// Execute a DROP USER statement.
    [[nodiscard]] Result<QueryResult> execute_drop_user(const DropUserStmt& stmt);

    /// Execute an ALTER TABLE statement (ADD/DROP/RENAME COLUMN).
    [[nodiscard]] Result<QueryResult> execute_alter_table(const AlterTableStmt& stmt);

    /// Execute an ALTER USER statement.
    [[nodiscard]] Result<QueryResult> execute_alter_user(const AlterUserStmt& stmt);
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

    /// Execute a DDL CREATE INDEX statement.
    [[nodiscard]] Result<QueryResult> execute_create_index(const CreateIndexStmt& stmt);

    /// Execute a DDL DROP INDEX statement.
    [[nodiscard]] Result<QueryResult> execute_drop_index(const DropIndexStmt& stmt);

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

    /// Execute an EXPLAIN or EXPLAIN ANALYZE statement.
    [[nodiscard]] Result<QueryResult> execute_explain(const ExplainStmt& stmt,
                                                      const BoundStatement& bound);

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
    ReplicationSlotManager* slot_mgr_ = nullptr;
    WalSenderManager* sender_mgr_ = nullptr;
    WalReceiver* wal_receiver_ = nullptr;
    WalWriter* wal_writer_ = nullptr;
    UserManager* user_mgr_ = nullptr;
    CatalogPersistence* catalog_persistence_ = nullptr;
    AuthMethod auth_method_{};
    database_id_t current_database_id_ = default_database_id;
    int skip_masking_depth_ = 0;
    bool standby_mode_ = false;
};

} // namespace giodb
