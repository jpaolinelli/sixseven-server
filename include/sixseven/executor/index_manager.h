#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/vector/hnsw_index.h"

#include <atomic>
#include <latch>
#include <memory>
#include <mutex>
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
///   - Per-row index maintenance: insert_entry / remove_entry for DML.
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

    // -----------------------------------------------------------------------
    // Per-row maintenance API (GDB-984)
    //
    // Thread-safety model: maintenance_latch_ (exclusive for write, shared for
    // read) guards the per-table target vectors (maintenance_btree_,
    // maintenance_hash_, maintenance_bm25_, maintenance_hnsw_).  The individual
    // index objects (BTreeIndex, HashIndex, etc.) carry their own internal
    // latches, so concurrent scan readers are safe while a writer holds only
    // the index's own latch.  insert_entry / remove_entry acquire an exclusive
    // lock on maintenance_latch_ before iterating the target lists, then
    // release it before calling into each index's own locking methods.
    // -----------------------------------------------------------------------

    /// Register a B-tree maintenance target for a table.
    /// The BTreeIndex pointer must remain valid for the lifetime of this
    /// IndexManager (or until the target is dropped via drop_index).
    void register_btree_target(table_id_t table_id, BtreeMaintenanceTarget target);

    /// Register a hash index maintenance target for a table.
    void register_hash_target(table_id_t table_id, HashMaintenanceTarget target);

    /// Register a BM25 maintenance target for a table.
    void register_bm25_target(table_id_t table_id, Bm25MaintenanceTarget target);

    /// Register an HNSW maintenance target for a table.
    /// The target's rid_map must point to the hnsw_rid_maps_ slot for this index.
    void register_hnsw_target(table_id_t table_id, HnswMaintenanceTarget target);

    /// Insert a row into all secondary indexes registered for @p table_id.
    ///
    /// For each registered target:
    ///   - B-tree/hash: extract key columns from @p values via key_column_ordinals,
    ///     build a KeyType, and call index->insert(key, rid).
    ///   - BM25: extract the text column, call index->add_document(rid, text).
    ///   - HNSW: extract the float vector from @p values[0] (the embedding column),
    ///     call index->insert(vector) -> node_id, and record rid_map[node_id] = rid.
    ///
    /// On any sub-index failure the error is propagated immediately (fail-fast,
    /// mirroring GDB-932 behavior in the executor).  Partial maintenance may
    /// occur for the indexes processed before the failing one.
    ///
    /// @param table_id  Table whose indexes are maintained.
    /// @param rid       Row identifier assigned by the heap.
    /// @param values    Row values in storage-schema column order.
    [[nodiscard]] Result<void>
    insert_entry(table_id_t table_id, const RID& rid, const std::vector<Value>& values);

    /// Remove a row from all secondary indexes registered for @p table_id.
    ///
    /// For each registered target:
    ///   - B-tree: calls index->remove(key, rid) (RID-qualified delete).
    ///   - Hash: calls index->remove(key, rid) (RID-qualified delete).
    ///   - BM25: calls index->remove_document(rid).
    ///   - HNSW: reverse-scans rid_map to find the node_id whose slot equals
    ///     @p rid, calls index->remove(node_id), then sets rid_map[node_id] =
    ///     RID::invalid() to tombstone the slot.
    ///
    /// Errors are propagated fail-fast (same as insert_entry).
    ///
    /// @param table_id  Table whose indexes are maintained.
    /// @param rid       Row identifier to remove.
    /// @param values    Row values used to reconstruct keys (btree/hash).
    [[nodiscard]] Result<void>
    remove_entry(table_id_t table_id, const RID& rid, const std::vector<Value>& values);

    // -----------------------------------------------------------------------
    // Accessor maps (for Planner construction)
    // -----------------------------------------------------------------------

    [[nodiscard]] std::unordered_map<index_id_t, BTreeIndex*>* btree_map();
    [[nodiscard]] std::unordered_map<index_id_t, HashIndex*>* hash_map();
    [[nodiscard]] std::unordered_map<index_id_t, HnswIndex*>* hnsw_map();
    [[nodiscard]] std::unordered_map<index_id_t, Bm25Index*>* bm25_map();

    /// HNSW node_id -> RID maps for direct tuple lookup (avoids full table scan).
    [[nodiscard]] std::unordered_map<index_id_t, std::vector<RID>>* hnsw_rid_maps();

    /// Append a RID to an HNSW index's node->RID mapping.
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

    /// HNSW node_id -> heap RID mapping. node_id N maps to hnsw_rid_maps_[id][N].
    /// Built during rebuild/load, extended during INSERT via append_hnsw_rid().
    std::unordered_map<index_id_t, std::vector<RID>> hnsw_rid_maps_;

    /// Thread safety for async loading.
    mutable std::shared_mutex maps_latch_;
    std::vector<std::jthread> load_threads_;
    std::unique_ptr<std::latch> load_latch_;
    std::atomic<bool> async_load_done_{false};

    // -----------------------------------------------------------------------
    // Per-table maintenance target storage (GDB-984)
    //
    // Guarded by maintenance_latch_ (exclusive for register_*; exclusive for
    // insert_entry / remove_entry while the target list is copied, then the
    // latch is released before calling into each index's own locking layer).
    // -----------------------------------------------------------------------
    mutable std::mutex maintenance_latch_;
    std::unordered_map<table_id_t, std::vector<BtreeMaintenanceTarget>> maintenance_btree_;
    std::unordered_map<table_id_t, std::vector<HashMaintenanceTarget>> maintenance_hash_;
    std::unordered_map<table_id_t, std::vector<Bm25MaintenanceTarget>> maintenance_bm25_;
    std::unordered_map<table_id_t, std::vector<HnswMaintenanceTarget>> maintenance_hnsw_;
};

} // namespace sixseven
