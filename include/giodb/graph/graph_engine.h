#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/value.h"
#include "giodb/graph/edge_table.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/wal_record.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Per-edge-type persistent storage state.
struct EdgeStorage {
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TableHeap> heap;
    Schema storage_schema;

    /// Maps edge_row_id -> heap RID for O(1) delete lookups.
    std::unordered_map<uint64_t, RID> rid_map;
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
    [[nodiscard]] Result<edge_id_t> create_edge_type(const std::string& name,
                                                     table_id_t source_table_id,
                                                     table_id_t target_table_id,
                                                     TypeId source_pk_type,
                                                     TypeId target_pk_type,
                                                     const std::vector<ColumnDef>& property_columns,
                                                     bool prevent_duplicates = false);

    /// Drop an edge type and its backing EdgeTable.
    /// If persistence is enabled, also removes the storage file.
    [[nodiscard]] Result<void> drop_edge_type(const std::string& name);

    /// LINK: create an edge between two nodes.
    /// Inserts a row into the edge table and updates both adjacency indexes.
    /// If persistence is enabled, also writes the edge to the heap file.
    /// Returns the edge_row_id of the newly created edge.
    [[nodiscard]] Result<uint64_t> link(const std::string& edge_type,
                                        const Value& source_pk,
                                        const Value& target_pk,
                                        const std::vector<Value>& properties = {});

    /// UNLINK: delete the edge between two specific nodes.
    /// Removes the first matching edge from the edge table and both indexes.
    /// If persistence is enabled, also deletes the edge from the heap file.
    [[nodiscard]] Result<void>
    unlink(const std::string& edge_type, const Value& source_pk, const Value& target_pk);

    /// UNLINK with WHERE: delete edges matching a predicate.
    /// Returns the number of edges deleted.
    [[nodiscard]] Result<uint64_t> unlink_where(const std::string& edge_type,
                                                const Value& source_pk,
                                                const Value& target_pk,
                                                std::function<bool(const EdgeRow&)> predicate);

    /// Get all outgoing edges from a node.
    [[nodiscard]] Result<std::vector<EdgeRow>> get_edges_from(const std::string& edge_type,
                                                              const Value& source_pk) const;

    /// Get all incoming edges to a node.
    [[nodiscard]] Result<std::vector<EdgeRow>> get_edges_to(const std::string& edge_type,
                                                            const Value& target_pk) const;

    /// Get the EdgeTable for a given edge type (for advanced queries).
    [[nodiscard]] Result<EdgeTable*> get_edge_table(const std::string& name);

    /// List all registered edge type names.
    [[nodiscard]] std::vector<std::string> list_edge_types() const;

    /// Load all edge data from disk files on startup.
    /// For each edge type in the catalog, opens the storage file and
    /// repopulates the in-memory EdgeTable and adjacency indexes.
    /// Must be called after catalog loading is complete.
    [[nodiscard]] Result<void> load_edges();

private:
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
    [[nodiscard]] std::filesystem::path edge_file_path(edge_id_t edge_id) const;

    /// Create persistent storage for an edge type.
    [[nodiscard]] Result<void> create_edge_storage(edge_id_t edge_id,
                                                   TypeId source_pk_type,
                                                   TypeId target_pk_type,
                                                   const std::vector<ColumnDef>& property_columns);

    /// Open existing persistent storage for an edge type.
    [[nodiscard]] Result<void> open_edge_storage(edge_id_t edge_id,
                                                 TypeId source_pk_type,
                                                 TypeId target_pk_type,
                                                 const std::vector<ColumnDef>& property_columns);

    /// Persist a single edge row to the heap file.
    [[nodiscard]] Result<void> persist_edge(const std::string& edge_type,
                                            uint64_t edge_row_id,
                                            const Value& source_pk,
                                            const Value& target_pk,
                                            const std::vector<Value>& properties);

    /// Delete a single edge row from the heap file.
    [[nodiscard]] Result<void> delete_persisted_edge(const std::string& edge_type,
                                                     uint64_t edge_row_id);

    /// Parse property columns from the catalog's comma-separated format.
    [[nodiscard]] static std::vector<ColumnDef>
    parse_property_columns(const std::string& properties_str);

    /// Returns true if persistence is enabled (DiskManager is available).
    [[nodiscard]] bool has_persistence() const;

    Catalog& catalog_;
    DiskManager* dm_ = nullptr;
    std::filesystem::path data_dir_;
    WalWriter* wal_ = nullptr;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<EdgeTable>> edge_tables_;
    std::unordered_map<std::string, std::unique_ptr<EdgeStorage>> edge_storage_;
};

} // namespace giodb
