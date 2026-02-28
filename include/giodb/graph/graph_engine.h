#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/value.h"
#include "giodb/graph/edge_table.h"
#include "giodb/storage/wal_record.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Manages graph edge types and provides LINK / UNLINK operations.
///
/// The GraphEngine sits above individual EdgeTables, coordinating with the
/// Catalog for metadata and managing the lifecycle of edge tables.
///
/// Usage:
/// ```
///   GraphEngine engine(catalog);
///   engine.create_edge_type("follows", src_id, tgt_id, INT64, INT64, {}, true);
///   engine.link("follows", Value(1L), Value(2L), {});
///   auto edges = engine.get_edges_from("follows", Value(1L));
///   engine.unlink("follows", Value(1L), Value(2L));
/// ```
///
/// Thread safety: All public methods are protected by a mutex.
class WalWriter;

class GraphEngine {
public:
    /// @param catalog  System catalog for metadata.
    /// @param wal      Optional WAL writer for durability. May be nullptr.
    explicit GraphEngine(Catalog& catalog, WalWriter* wal = nullptr);

    /// Create an edge type and its backing EdgeTable.
    /// Registers the edge type in the catalog and creates the in-memory EdgeTable.
    [[nodiscard]] Result<edge_id_t> create_edge_type(const std::string& name,
                                                     table_id_t source_table_id,
                                                     table_id_t target_table_id,
                                                     TypeId source_pk_type,
                                                     TypeId target_pk_type,
                                                     const std::vector<ColumnDef>& property_columns,
                                                     bool prevent_duplicates = false);

    /// Drop an edge type and its backing EdgeTable.
    [[nodiscard]] Result<void> drop_edge_type(const std::string& name);

    /// LINK: create an edge between two nodes.
    /// Inserts a row into the edge table and updates both adjacency indexes.
    /// Returns the edge_row_id of the newly created edge.
    [[nodiscard]] Result<uint64_t> link(const std::string& edge_type,
                                        const Value& source_pk,
                                        const Value& target_pk,
                                        const std::vector<Value>& properties = {});

    /// UNLINK: delete the edge between two specific nodes.
    /// Removes the first matching edge from the edge table and both indexes.
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

private:
    /// Log a LINK or UNLINK operation to the WAL.
    void log_edge_wal(WalRecordType type,
                      edge_id_t edge_id,
                      uint64_t edge_row_id,
                      const std::string& edge_type_name);

    Catalog& catalog_;
    WalWriter* wal_ = nullptr;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<EdgeTable>> edge_tables_;
};

} // namespace giodb
