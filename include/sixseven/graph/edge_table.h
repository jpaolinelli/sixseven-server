#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/table/tuple.h"

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

/// A single row in an edge table.
struct EdgeRow {
    uint64_t edge_row_id = 0;
    Value source_pk;
    Value target_pk;
    std::vector<Value> properties;
};

/// Configuration for creating an EdgeTable.
struct EdgeTableConfig {
    edge_id_t edge_id = 0;
    std::string name;
    table_id_t source_table_id = 0;
    table_id_t target_table_id = 0;

    /// Column definitions for edge properties (not including source/target PKs).
    std::vector<ColumnDef> property_columns;

    /// Type of the source table's primary key.
    TypeId source_pk_type = TypeId::INT64;

    /// Type of the target table's primary key.
    TypeId target_pk_type = TypeId::INT64;

    /// If true, enforce uniqueness on (source_pk, target_pk) pairs.
    bool prevent_duplicates = false;
};

/// A request to insert one edge in a batch operation.
struct EdgeInsertRequest {
    Value source_pk;
    Value target_pk;
    std::vector<Value> properties;
};

/// Stores edges for a single edge type with dual B+ tree adjacency indexes.
///
/// Each edge type gets its own EdgeTable that stores:
///   (source_pk, target_pk, property_0, property_1, ...)
///
/// Two B+ tree adjacency indexes enable efficient traversal:
///   - Forward index:  source_pk -> edge_row_id (for outgoing edges)
///   - Reverse index:  target_pk -> edge_row_id (for incoming edges)
///
/// An optional unique constraint index on (source_pk, target_pk) prevents
/// duplicate edges when configured.
///
/// Thread safety: All public methods are protected by a shared_mutex.
class EdgeTable {
public:
    explicit EdgeTable(EdgeTableConfig config);

    /// Insert an edge into the table. Updates both adjacency indexes.
    /// Returns CONSTRAINT_VIOLATION if duplicate prevention is enabled
    /// and the edge already exists.
    [[nodiscard]] Result<uint64_t> insert_edge(const Value& source_pk,
                                               const Value& target_pk,
                                               const std::vector<Value>& properties);

    /// Batch-insert multiple edges under a single lock acquisition.
    /// Returns a vector of row IDs (one per successfully inserted edge).
    /// On any failure mid-batch, all edges inserted so far in this batch
    /// are rolled back and the error is returned.
    [[nodiscard]] Result<std::vector<uint64_t>> insert_edges_batch(
        const std::vector<EdgeInsertRequest>& edges);

    /// Delete an edge by row ID. Removes from both adjacency indexes.
    [[nodiscard]] Result<void> delete_edge(uint64_t edge_row_id);

    /// Look up all edges originating from source_pk (forward traversal).
    [[nodiscard]] Result<std::vector<EdgeRow>> get_edges_from(const Value& source_pk) const;

    /// Look up all edges targeting target_pk (reverse traversal).
    [[nodiscard]] Result<std::vector<EdgeRow>> get_edges_to(const Value& target_pk) const;

    /// Get a specific edge by its row ID.
    [[nodiscard]] Result<EdgeRow> get_edge(uint64_t edge_row_id) const;

    /// Find a specific edge by source and target primary keys.
    /// Returns nullopt if the edge does not exist.
    [[nodiscard]] Result<std::optional<EdgeRow>> find_edge(const Value& source_pk,
                                                           const Value& target_pk) const;

    /// Restore an edge with a specific row ID (used during startup recovery).
    /// Inserts the edge into all indexes and the edge map without assigning a new ID.
    /// Updates next_row_id_ to ensure future inserts don't collide.
    [[nodiscard]] Result<void> restore_edge(uint64_t edge_row_id,
                                            const Value& source_pk,
                                            const Value& target_pk,
                                            const std::vector<Value>& properties);

    /// Return the total number of edges in the table.
    [[nodiscard]] uint64_t size() const;

    /// Return whether the table is empty.
    [[nodiscard]] bool empty() const;

    /// Return all edges in the table (unordered).
    [[nodiscard]] std::vector<EdgeRow> get_all_edges() const;

    /// Access the table configuration.
    [[nodiscard]] const EdgeTableConfig& config() const;

private:
    /// Look up all edge_row_ids from an adjacency index for a given key.
    [[nodiscard]] Result<std::vector<uint64_t>> lookup_adjacency(const BTreeIndex& index,
                                                                 const Value& key) const;

    /// Encode an edge_row_id as a RID for B+ tree storage.
    /// Requires edge_row_id <= kMaxEncodableRowId (48-bit limit).
    static RID encode_rid(uint64_t edge_row_id);

    /// Decode an edge_row_id from a RID.
    static uint64_t decode_rid(const RID& rid);

    /// Maximum row ID that can be losslessly encoded into a RID.
    /// RID uses 32-bit PageId + 16-bit SlotId = 48 bits total.
    static constexpr uint64_t kMaxEncodableRowId = (1ULL << 48) - 1;

    EdgeTableConfig config_;
    uint64_t next_row_id_ = 1;

    /// Edge row storage: edge_row_id -> EdgeRow.
    std::unordered_map<uint64_t, EdgeRow> edges_;

    /// Forward adjacency index: source_pk -> edge_row_id.
    std::unique_ptr<BTreeIndex> forward_index_;

    /// Reverse adjacency index: target_pk -> edge_row_id.
    std::unique_ptr<BTreeIndex> reverse_index_;

    /// Optional unique constraint index: (source_pk, target_pk) -> edge_row_id.
    /// Only created when prevent_duplicates is true.
    std::unique_ptr<BTreeIndex> unique_index_;

    mutable std::shared_mutex mu_;
};

} // namespace sixseven
