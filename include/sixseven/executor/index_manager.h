#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/vector/hnsw_index.h"

#include <atomic>
#include <latch>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sixseven {

// Forward declarations.
class Catalog;
class CatalogPersistence;
class StorageManager;

/// Owns all in-memory BTreeIndex and HashIndex instances.
///
/// Responsible for:
///   - Loading indexes from persisted disk files (fast path) or rebuilding
///     from table data (slow path, first time or after crash).
///   - Persisting indexes to disk on CREATE INDEX and shutdown.
///   - Creating and populating indexes at runtime (CREATE INDEX).
///   - Dropping indexes at runtime (DROP INDEX).
///   - Providing index pointer maps to the Planner for index scan planning.
///   - Async startup loading: server accepts connections while indexes load
///     in the background. Queries fall back to sequential scan until ready.
///
/// Also handles autoincrement counter initialization during startup,
/// using the persisted value from sys_tables when available, falling back
/// to a table scan otherwise.
class IndexManager {
public:
    IndexManager(Catalog& catalog, StorageManager& storage);

    /// Set the CatalogPersistence reference (needed for autoincrement persistence).
    void set_catalog_persistence(CatalogPersistence* persistence);

    /// Load all indexes: from disk files if available, else rebuild from table
    /// data and persist to disk. Also initializes autoincrement counters.
    /// This is the synchronous version used in tests.
    [[nodiscard]] Result<void> rebuild_all_indexes();

    /// Launch background threads to load indexes asynchronously.
    /// The server can accept connections immediately; queries fall back to
    /// sequential scan until indexes are ready.
    [[nodiscard]] Result<void> start_async_load();

    /// Block until all async loading is complete. Used by tests.
    void wait_for_load_complete();

    /// Persist all in-memory indexes to disk (called on shutdown).
    [[nodiscard]] Result<void> flush_all_indexes();

    /// Create and populate a single index from existing table data.
    /// Also persists the index to disk.
    [[nodiscard]] Result<void> create_and_populate_index(const IndexDef& def,
                                                         const TableSchema& schema);

    /// Drop an index by ID. Also removes the index file from disk.
    /// @param id The index ID.
    /// @param table_id The table the index belongs to (for finding the database directory).
    void drop_index(index_id_t id, table_id_t table_id = 0);

    /// Rebuild one or more indexes by name. If `name` matches an index,
    /// rebuilds that single index. If it matches a table, rebuilds all
    /// indexes on that table. Used by the REINDEX SQL command.
    [[nodiscard]] Result<void> reindex(const std::string& name, database_id_t db_id);

    /// Accessor maps for Planner construction.
    [[nodiscard]] std::unordered_map<index_id_t, BTreeIndex*>* btree_map();
    [[nodiscard]] std::unordered_map<index_id_t, HashIndex*>* hash_map();
    [[nodiscard]] std::unordered_map<index_id_t, HnswIndex*>* hnsw_map();
    [[nodiscard]] std::unordered_map<index_id_t, Bm25Index*>* bm25_map();

    /// HNSW node_id → RID maps for direct tuple lookup (avoids full table scan).
    [[nodiscard]] std::unordered_map<index_id_t, std::vector<RID>>* hnsw_rid_maps();

    /// Append a RID to an HNSW index's node→RID mapping.
    /// Called from the embedding store callback after a new vector is inserted.
    void append_hnsw_rid(index_id_t index_id, RID rid);

private:
    /// Resolve index column names to column ordinals and type IDs from the schema.
    [[nodiscard]] std::vector<std::pair<size_t, TypeId>>
    resolve_index_columns(const std::string& columns, const TableSchema& schema) const;

    /// Parse a comma-separated column list into individual names.
    [[nodiscard]] static std::vector<std::string> parse_columns(const std::string& columns);

    /// Load a single btree index from its persisted file.
    [[nodiscard]] Result<void> load_btree_from_disk(const IndexDef& def, database_id_t db_id);

    /// Load a single hash index from its persisted file.
    [[nodiscard]] Result<void> load_hash_from_disk(const IndexDef& def, database_id_t db_id);

    /// Load a single HNSW index from its persisted file.
    [[nodiscard]] Result<void> load_hnsw_from_disk(const IndexDef& def, database_id_t db_id);

    /// Rebuild an HNSW index from table data (slow path).
    [[nodiscard]] Result<void> rebuild_hnsw_from_table(const IndexDef& def, database_id_t db_id);

    /// Load a single BM25 index from its persisted file.
    [[nodiscard]] Result<void> load_bm25_from_disk(const IndexDef& def, database_id_t db_id);

    /// Rebuild a BM25 index from table data (slow path / first build).
    [[nodiscard]] Result<void> rebuild_bm25_from_table(const IndexDef& def, database_id_t db_id);

    /// Persist a single BM25 index to disk.
    [[nodiscard]] Result<void>
    persist_bm25(const IndexDef& def, database_id_t db_id, const Bm25Index& index);

    /// Persist a single btree index to disk.
    [[nodiscard]] Result<void>
    persist_btree(const IndexDef& def, database_id_t db_id, const BTreeIndex& index);

    /// Persist a single hash index to disk.
    [[nodiscard]] Result<void>
    persist_hash(const IndexDef& def, database_id_t db_id, const HashIndex& index);

    /// Flush an HNSW index's metadata and BPM to disk.
    [[nodiscard]] Result<void>
    flush_hnsw(const IndexDef& def, database_id_t db_id, HnswIndex& index);

    /// Find the database_id that owns a given table_id.
    [[nodiscard]] database_id_t find_database_for_table(table_id_t table_id) const;

    Catalog& catalog_;
    StorageManager& storage_;
    CatalogPersistence* persistence_ = nullptr;

    /// Owning storage for index objects.
    std::vector<std::unique_ptr<BTreeIndex>> owned_btrees_;
    std::vector<std::unique_ptr<HashIndex>> owned_hashes_;
    std::vector<std::unique_ptr<HnswIndex>> owned_hnsw_;
    std::vector<std::unique_ptr<Bm25Index>> owned_bm25_;

    /// Non-owning pointer maps keyed by index_id (for Planner).
    std::unordered_map<index_id_t, BTreeIndex*> btree_indexes_;
    std::unordered_map<index_id_t, HashIndex*> hash_indexes_;

    /// HNSW pointer map keyed by index_id (globally unique, no naming collisions).
    std::unordered_map<index_id_t, HnswIndex*> hnsw_indexes_;

    /// BM25 pointer map keyed by index_id.
    std::unordered_map<index_id_t, Bm25Index*> bm25_indexes_;

    /// HNSW node_id → heap RID mapping. node_id N maps to hnsw_rid_maps_[id][N].
    /// Built during rebuild/load, extended during INSERT via append_hnsw_rid().
    std::unordered_map<index_id_t, std::vector<RID>> hnsw_rid_maps_;

    /// Thread safety for async loading.
    mutable std::shared_mutex maps_latch_;
    std::vector<std::jthread> load_threads_;
    std::unique_ptr<std::latch> load_latch_;
    std::atomic<bool> async_load_done_{false};
};

} // namespace sixseven
