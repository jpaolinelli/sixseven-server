#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sixseven {

// Forward declarations.
class AlgorithmRegistry;
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
struct BulkLinkStmt;
struct LinkStmt;
struct ReembedStmt;
struct SetStmt;
struct ShowStmt;
struct UnlinkStmt;
class EmbeddingWorkerPool;
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

/// Describes a single result column (name + type) for Describe responses.
struct ColumnDescription {
    std::string name;
    TypeId type_id = TypeId::INT32;
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

    /// Execute a SQL statement in a specific database context.
    /// Thread-safe: temporarily overrides the database for this call only.
    [[nodiscard]] Result<QueryResult> execute(const std::string& sql, database_id_t database_id);

    /// Describe the result columns of a SQL statement without executing it.
    /// Returns column metadata for SELECT statements (parse + bind only).
    /// Returns an empty vector for non-SELECT statements (INSERT/UPDATE/DELETE/DDL).
    [[nodiscard]] Result<std::vector<ColumnDescription>> describe(const std::string& sql);

    /// Describe result columns in a specific database context.
    [[nodiscard]] Result<std::vector<ColumnDescription>> describe(const std::string& sql,
                                                                  database_id_t database_id);

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

    /// Set the embedding worker pool for async embedding on INSERT.
    /// Also configures the store callback to write embeddings back to tuples.
    void set_embedding_worker_pool(EmbeddingWorkerPool* pool);

    /// Set the algorithm registry for graph algorithm table-valued functions.
    void set_algorithm_registry(AlgorithmRegistry* registry);

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

    /// Execute a bulk LINK statement (batch edge creation).
    [[nodiscard]] Result<QueryResult> execute_bulk_link(const BulkLinkStmt& stmt,
                                                        const BoundStatement& bound);

    /// Execute an UNLINK statement (delete edge).
    [[nodiscard]] Result<QueryResult> execute_unlink(const UnlinkStmt& stmt,
                                                     const BoundStatement& bound);

    /// Execute a REEMBED TABLE statement (bulk embedding regeneration).
    [[nodiscard]] Result<QueryResult> execute_reembed(const ReembedStmt& stmt);

    /// Execute an EXPLAIN or EXPLAIN ANALYZE statement.
    [[nodiscard]] Result<QueryResult> execute_explain(const ExplainStmt& stmt,
                                                      const BoundStatement& bound);

    /// Coerce LINK/UNLINK key values to match the PK types of the source/target tables.
    [[nodiscard]] Result<std::pair<Value, Value>>
    coerce_link_keys(const std::string& edge_type, const Value& src_key, const Value& tgt_key);

    /// Verify that a row with the given PK value exists in the specified table.
    /// Uses a per-table PK hash set cache for O(1) lookups after first scan.
    [[nodiscard]] Result<bool> verify_pk_exists(table_id_t table_id, const Value& pk_value);

    /// Populate the PK cache for a table (one-time full scan).
    [[nodiscard]] Result<void> ensure_pk_cache(table_id_t table_id);

    /// Invalidate the PK cache for a table (after INSERT/DELETE).
    void invalidate_pk_cache(table_id_t table_id);

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
    EmbeddingWorkerPool* embedding_pool_ = nullptr;
    AlgorithmRegistry* algorithm_registry_ = nullptr;
    CatalogPersistence* catalog_persistence_ = nullptr;
    AuthMethod auth_method_{};
    database_id_t current_database_id_ = default_database_id;
    int skip_masking_depth_ = 0;
    bool standby_mode_ = false;

    /// Per-table PK value cache for fast LINK existence checks.
    /// Maps table_id -> set of serialized PK values.
    std::unordered_map<table_id_t, std::unordered_set<std::string>> pk_cache_;
};

} // namespace sixseven
