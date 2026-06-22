#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/edge_table.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

/// Storage for a single persisted edge B+ tree index file.
struct EdgeIndexStorage {
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;
};

/// Per-edge-type persistent storage state.
struct EdgeStorage {
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TableHeap> heap;
    Schema storage_schema;

    /// Maps edge_row_id -> heap RID for O(1) delete lookups.
    std::unordered_map<uint64_t, RID> rid_map;

    /// Persisted B+ tree index storage (one file each).
    std::unique_ptr<EdgeIndexStorage> fwd_idx;
    std::unique_ptr<EdgeIndexStorage> rev_idx;
    std::unique_ptr<EdgeIndexStorage> uniq_idx; ///< nullptr if no unique constraint.
};

/// Manages graph edge types and provides LINK / UNLINK operations.
///
/// The GraphEngine sits above individual EdgeTables, coordinating with the
/// Catalog for metadata and managing the lifecycle of edge tables.
///
/// Edge data is persisted to disk using TableHeap-based storage files.
/// On startup, load_edges() scans these files and repopulates in-memory
/// EdgeTables and adjacency indexes.
///
/// Edge types are scoped per database. Two databases can each have an edge
/// type named "follows" without conflict.
///
/// Thread safety: All public methods are protected by a mutex.
class WalWriter;

class GraphEngine {
public:
    /// @param catalog  System catalog for metadata.
    /// @param wal      Optional WAL writer for durability. May be nullptr.
    explicit GraphEngine(Catalog& catalog, WalWriter* wal = nullptr);

    /// @param catalog   System catalog for metadata.
    /// @param dm        DiskManager for edge file I/O.
    /// @param data_dir  Root data directory for edge storage files.
    /// @param wal       Optional WAL writer for durability. May be nullptr.
    GraphEngine(Catalog& catalog,
                DiskManager& dm,
                std::filesystem::path data_dir,
                WalWriter* wal = nullptr);

    /// Destructor: flushes and closes all edge storage files.
    ~GraphEngine();

    // Non-copyable, non-movable.
    GraphEngine(const GraphEngine&) = delete;
    GraphEngine& operator=(const GraphEngine&) = delete;
    GraphEngine(GraphEngine&&) = delete;
    GraphEngine& operator=(GraphEngine&&) = delete;

    /// Create an edge type and its backing EdgeTable.
    /// Registers the edge type in the catalog and creates the in-memory EdgeTable.
    /// If persistence is enabled, also creates the backing storage file.
    [[nodiscard]] Result<edge_id_t> create_edge_type(database_id_t database_id,
                                                     const std::string& name,
                                                     table_id_t source_table_id,
                                                     table_id_t target_table_id,
                                                     TypeId source_pk_type,
                                                     TypeId target_pk_type,
                                                     const std::vector<ColumnDef>& property_columns,
                                                     bool prevent_duplicates = false);

    /// Drop an edge type and its backing EdgeTable.
    /// If persistence is enabled, also removes the storage file.
    [[nodiscard]] Result<void> drop_edge_type(database_id_t database_id, const std::string& name);

    /// Drop all edge types that reference a given table (source or target).
    /// Called during DROP TABLE CASCADE to clean up the graph engine state.
    [[nodiscard]] Result<void> drop_edge_types_for_table(database_id_t database_id,
                                                         table_id_t table_id);

    /// LINK: create an edge between two nodes.
    /// Inserts a row into the edge table and updates both adjacency indexes.
    /// If persistence is enabled, also writes the edge to the heap file.
    /// Returns the edge_row_id of the newly created edge.
    [[nodiscard]] Result<uint64_t> link(database_id_t database_id,
                                        const std::string& edge_type,
                                        const Value& source_pk,
                                        const Value& target_pk,
                                        const std::vector<Value>& properties = {});

    /// Bulk LINK: create multiple edges of the same type in a single lock
    /// acquisition. Significantly faster than calling link() in a loop because
    /// both GraphEngine::mu_ and EdgeTable::mu_ are acquired only once.
    /// Returns the count of edges successfully inserted.
    [[nodiscard]] Result<uint64_t> link_batch(database_id_t database_id,
                                              const std::string& edge_type,
                                              const std::vector<EdgeInsertRequest>& edges);

    /// UNLINK: delete the edge between two specific nodes.
    /// Removes the first matching edge from the edge table and both indexes.
    /// If persistence is enabled, also deletes the edge from the heap file.
    [[nodiscard]] Result<void> unlink(database_id_t database_id,
                                      const std::string& edge_type,
                                      const Value& source_pk,
                                      const Value& target_pk);

    /// UNLINK with WHERE: delete edges matching a predicate.
    /// Returns the number of edges deleted.
    [[nodiscard]] Result<uint64_t> unlink_where(database_id_t database_id,
                                                const std::string& edge_type,
                                                const Value& source_pk,
                                                const Value& target_pk,
                                                std::function<bool(const EdgeRow&)> predicate);

    /// Get all outgoing edges from a node.
    [[nodiscard]] Result<std::vector<EdgeRow>> get_edges_from(database_id_t database_id,
                                                              const std::string& edge_type,
                                                              const Value& source_pk) const;

    /// Get all incoming edges to a node.
    [[nodiscard]] Result<std::vector<EdgeRow>> get_edges_to(database_id_t database_id,
                                                            const std::string& edge_type,
                                                            const Value& target_pk) const;

    /// Get the EdgeTable for a given edge type (for advanced queries).
    [[nodiscard]] Result<EdgeTable*> get_edge_table(database_id_t database_id,
                                                    const std::string& name);

    /// Get all edges for a given edge type.
    [[nodiscard]] Result<std::vector<EdgeRow>> get_all_edges(database_id_t database_id,
                                                             const std::string& edge_type) const;

    /// List all registered edge type names for a database.
    [[nodiscard]] std::vector<std::string> list_edge_types(database_id_t database_id) const;

    /// Load all edge data from disk files on startup.
    /// For each edge type in the catalog, opens the storage file and
    /// repopulates the in-memory EdgeTable and adjacency indexes.
    /// Must be called after catalog loading is complete.
    [[nodiscard]] Result<void> load_edges();

    /// Persist all in-memory edge B+ tree indexes to disk.
    /// Called at shutdown so indexes can be loaded directly on next startup
    /// instead of being rebuilt from the edge heap.
    [[nodiscard]] Result<void> flush_edge_indexes();

    /// Durably flush all edge data — both the edge heap pages (the source of
    /// truth that load_edges() scans) and the B+ tree index pages — to disk.
    ///
    /// Unlike flush_edge_indexes(), which only persists the index structures,
    /// this also flushes the edge heap buffer pools. Edge inserts are normally
    /// only made durable by a clean shutdown (the destructor flushes the heaps)
    /// or eviction/checkpoint; there is no WAL when the GraphEngine runs without
    /// one. Call this after bulk-loading edges (e.g. demo bootstrap) so they
    /// survive a hard kill that never reaches the destructor.
    [[nodiscard]] Result<void> flush_edges();

private:
    /// Build a compound map key from database_id and edge type name.
    [[nodiscard]] static std::string make_edge_key(database_id_t database_id,
                                                   const std::string& name);

    /// Log a LINK or UNLINK operation to the WAL.
    void log_edge_wal(WalRecordType type,
                      edge_id_t edge_id,
                      uint64_t edge_row_id,
                      const std::string& edge_type_name);

    /// Build the storage schema for an edge type.
    /// Schema: (edge_row_id:INT64, source_pk, target_pk, prop0, prop1, ...).
    [[nodiscard]] static Schema
    build_edge_storage_schema(TypeId source_pk_type,
                              TypeId target_pk_type,
                              const std::vector<ColumnDef>& property_columns);

    /// Build the file path for an edge type storage file.
    [[nodiscard]] std::filesystem::path edge_file_path(database_id_t database_id,
                                                       edge_id_t edge_id) const;

    /// Build the file path for an edge index file (fwd, rev, or uniq).
    [[nodiscard]] std::filesystem::path edge_index_path(database_id_t database_id,
                                                        edge_id_t edge_id,
                                                        const std::string& suffix) const;

    /// Persist the B+ tree indexes for a single edge type to disk.
    [[nodiscard]] Result<void> persist_edge_indexes(const std::string& edge_key,
                                                     database_id_t database_id,
                                                     edge_id_t edge_id);

    /// Load B+ tree indexes for a single edge type from disk.
    /// Returns true if indexes were loaded, false if fallback rebuild is needed.
    [[nodiscard]] Result<bool> load_edge_indexes(const std::string& edge_key,
                                                  database_id_t database_id,
                                                  edge_id_t edge_id,
                                                  EdgeTable& edge_table);

    /// Create persistent storage for an edge type.
    [[nodiscard]] Result<void> create_edge_storage(database_id_t database_id,
                                                   edge_id_t edge_id,
                                                   TypeId source_pk_type,
                                                   TypeId target_pk_type,
                                                   const std::vector<ColumnDef>& property_columns);

    /// Persist a single edge row to the heap file.
    [[nodiscard]] Result<void> persist_edge(const std::string& edge_key,
                                            uint64_t edge_row_id,
                                            const Value& source_pk,
                                            const Value& target_pk,
                                            const std::vector<Value>& properties);

    /// Delete a single edge row from the heap file.
    [[nodiscard]] Result<void> delete_persisted_edge(const std::string& edge_key,
                                                     uint64_t edge_row_id);

    /// Parse property columns from the catalog's comma-separated format.
    [[nodiscard]] static std::vector<ColumnDef>
    parse_property_columns(const std::string& properties_str);

    /// Returns true if persistence is enabled (DiskManager is available).
    [[nodiscard]] bool has_persistence() const;

    /// Drop a single edge type by its compound key (must hold mu_).
    /// Removes from catalog, cleans up storage files, and erases from maps.
    void drop_edge_type_locked(const std::string& edge_key,
                               database_id_t database_id,
                               const std::string& name);

    /// Tear down persistent storage for an edge type and erase it from the
    /// in-memory maps (must hold mu_). Caller is responsible for catalog
    /// removal. All dm_ accesses are null-guarded.
    void teardown_edge_storage_locked(const std::string& edge_key,
                                      database_id_t database_id,
                                      const std::string& name,
                                      edge_id_t edge_id);

    Catalog& catalog_;
    DiskManager* dm_ = nullptr;
    std::filesystem::path data_dir_;
    WalWriter* wal_ = nullptr;
    mutable std::mutex mu_;

    /// Edge tables keyed by compound key: "database_id:edge_type_name".
    std::unordered_map<std::string, std::unique_ptr<EdgeTable>> edge_tables_;

    /// Edge storage keyed by compound key: "database_id:edge_type_name".
    std::unordered_map<std::string, std::unique_ptr<EdgeStorage>> edge_storage_;
};

} // namespace sixseven
