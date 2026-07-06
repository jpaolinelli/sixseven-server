#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/planner/statistics.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/txn/txn_manager.h"

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
struct BackfillStmt;
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
struct ReindexStmt;
struct SetStmt;
struct ShowStmt;
struct UnlinkStmt;
struct VacuumStmt;
struct AnalyzeStmt;
class BackfillManager;
class EmbeddingWorkerPool;
class HnswIndex;
class IndexManager;
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
    void set_hnsw_indexes(std::unordered_map<index_id_t, HnswIndex*>* indexes);

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

    /// Set the index manager for B+ tree and hash index lifecycle.
    void set_index_manager(IndexManager* mgr);

    /// Set the backfill manager for BACKFILL EMBEDDINGS command.
    void set_backfill_manager(BackfillManager* mgr);

    /// Read-only access to the ANALYZE statistics store (GDB-754). The
    /// planner consults this store for cost-based access path and join
    /// method selection; tests use it to verify ANALYZE populated stats.
    [[nodiscard]] const StatisticsStore& statistics() const { return statistics_store_; }

    /// Shared transaction manager (GDB-747). DML statements stamp tuple
    /// versions with ids allocated here; VACUUM integration (GDB-1230) reads
    /// its horizon from the same instance.
    [[nodiscard]] TransactionManager& transaction_manager() { return txn_mgr_; }

    /// The id of the explicit transaction opened by BEGIN, or invalid_txn_id
    /// when the engine is in autocommit mode.
    [[nodiscard]] txn_id_t active_transaction_id() const { return active_txn_id_; }

    /// Set the session-level default isolation level used by subsequent BEGIN
    /// statements (GDB-978). RC = statement-level snapshot; SI = transaction-
    /// level snapshot frozen at BEGIN. SERIALIZABLE is accepted but rw-
    /// dependency enforcement is out of scope (GDB-966/968).
    void set_session_isolation(IsolationLevel level);

    /// Return the current session isolation level (GDB-978).
    [[nodiscard]] IsolationLevel session_isolation() const;

    /// Parse an isolation level string ("read committed", "snapshot isolation",
    /// "repeatable read", "serializable") into an IsolationLevel enum value.
    /// Returns an error for unrecognized strings. Case-insensitive (GDB-978).
    [[nodiscard]] static Result<IsolationLevel> parse_isolation_level(const std::string& value);

private:
    /// Execute BEGIN: open an explicit transaction (GDB-747).
    [[nodiscard]] Result<QueryResult> execute_begin();

    /// Execute COMMIT: commit the explicit transaction, if any (GDB-747).
    [[nodiscard]] Result<QueryResult> execute_commit();

    /// Execute ROLLBACK: abort the explicit transaction, if any, and
    /// compensate row counters for its logical inserts/deletes (GDB-747).
    [[nodiscard]] Result<QueryResult> execute_rollback();
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

    /// Execute a BACKFILL EMBEDDINGS ON table statement.
    [[nodiscard]] Result<QueryResult> execute_backfill(const BackfillStmt& stmt);

    /// Execute a REEMBED TABLE statement (bulk embedding regeneration).
    [[nodiscard]] Result<QueryResult> execute_reembed(const ReembedStmt& stmt);

    /// Execute a REINDEX statement (rebuild one or all indexes).
    [[nodiscard]] Result<QueryResult> execute_reindex(const ReindexStmt& stmt);

    /// Execute a VACUUM statement. Resolves and validates the target table(s)
    /// (the named table, or every user table in the current database) and
    /// returns the VACUUM completion tag. Reclamation is deferred until the
    /// MVCC epic provides headed tuples and a shared transaction manager
    /// (GDB-1230); running txn::Vacuum over raw heap tuples destroys live rows.
    [[nodiscard]] Result<QueryResult> execute_vacuum(const VacuumStmt& stmt);

    /// Execute an ANALYZE statement (gather table/column statistics).
    /// With a table name, analyzes that table; otherwise analyzes every user
    /// table in the current database. Populates the StatisticsStore.
    [[nodiscard]] Result<QueryResult> execute_analyze(const AnalyzeStmt& stmt);

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
    std::unordered_map<index_id_t, HnswIndex*>* hnsw_indexes_ = nullptr;
    SettingsCache* settings_cache_ = nullptr;
    ProviderCache* provider_cache_ = nullptr;
    ReplicationSlotManager* slot_mgr_ = nullptr;
    WalSenderManager* sender_mgr_ = nullptr;
    WalReceiver* wal_receiver_ = nullptr;
    WalWriter* wal_writer_ = nullptr;
    UserManager* user_mgr_ = nullptr;
    EmbeddingWorkerPool* embedding_pool_ = nullptr;
    AlgorithmRegistry* algorithm_registry_ = nullptr;
    IndexManager* index_manager_ = nullptr;
    BackfillManager* backfill_manager_ = nullptr;
    CatalogPersistence* catalog_persistence_ = nullptr;
    AuthMethod auth_method_{};

    /// Transaction manager for MVCC stamping and visibility (GDB-747).
    TransactionManager txn_mgr_;

    /// Explicit transaction opened by BEGIN (invalid_txn_id = autocommit).
    txn_id_t active_txn_id_ = invalid_txn_id;

    /// Session-level isolation level, applied by the next BEGIN (GDB-978).
    IsolationLevel session_isolation_ = IsolationLevel::READ_COMMITTED;

    /// Per-table live-row-count deltas accumulated by the current transaction
    /// (+inserts, -deletes), keyed by table_id_t rather than TableHeap*
    /// (GDB-1243): a raw heap pointer can be freed by DROP TABLE before the
    /// transaction ends (e.g. DROP TABLE inside an explicit txn followed by
    /// ROLLBACK), which would otherwise be a use-after-free when compensating.
    /// Applied in reverse on ROLLBACK / implicit-txn abort so COUNT(*) stays
    /// accurate after aborted logical inserts/deletes. See
    /// compensate_row_deltas_and_clear().
    std::unordered_map<table_id_t, int64_t> active_txn_row_deltas_;

    /// Reverse every accumulated per-table row-count delta in
    /// active_txn_row_deltas_ (restoring row_count_ to what it was before the
    /// failed/rolled-back statement(s)), then clear the map. Shared by the
    /// explicit ROLLBACK path and the implicit-transaction abort path
    /// (GDB-1243) so both compensate identically. Resolves each table_id to
    /// its live TableHeap via storage_; tables dropped mid-transaction are
    /// silently skipped (their storage no longer exists, so there is nothing
    /// to compensate and no freed pointer is ever touched).
    void compensate_row_deltas_and_clear();

    /// GDB-1246: compute the WAL LSN that must be durably flushed before a
    /// commit (explicit or implicit/autocommit) may be acknowledged. Returns
    /// invalid_lsn if wal_writer_ is null (WAL disabled) or nothing has been
    /// appended yet. See the .cpp for why the last-appended LSN (not a
    /// dedicated COMMIT record) is the correct watermark.
    [[nodiscard]] lsn_t commit_lsn_watermark() const;

    /// GDB-1246: block until the WAL is durably flushed at least to
    /// commit_lsn, so a commit ack always implies durability. Shared by both
    /// the explicit COMMIT path (execute_commit()) and the implicit/
    /// autocommit statement-transaction commit path (execute_plan()), since
    /// both need the identical null-guard + flush_until + error-mapping
    /// behavior. No-op (returns ok()) if wal_writer_ is null (WAL disabled)
    /// or commit_lsn is invalid_lsn (nothing was appended). Does not hold any
    /// lock across the call, so WalWriter's group-commit fsync batching
    /// (GDB-769) across concurrent committers is preserved.
    [[nodiscard]] Result<void> wait_for_commit_durability(lsn_t commit_lsn);

    database_id_t current_database_id_ = default_database_id;
    int skip_masking_depth_ = 0;
    bool standby_mode_ = false;

    /// Statistics gathered by ANALYZE, keyed by table_id. Populated only;
    /// not yet consumed by the planner (optimizer wiring belongs to the
    /// Planner Completion epic).
    StatisticsStore statistics_store_;

    /// Per-table PK value cache for fast LINK existence checks.
    /// Maps table_id -> set of serialized PK values.
    std::unordered_map<table_id_t, std::unordered_set<std::string>> pk_cache_;
};

} // namespace sixseven
