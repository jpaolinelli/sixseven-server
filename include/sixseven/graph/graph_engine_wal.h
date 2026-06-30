#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/edge_table.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

// -- Edge WAL payload (GDB-1067) -----------------------------------------------

/// Payload carried by EDGE_INSERT and EDGE_DELETE WAL records.
///
/// Binary format (native byte order, all lengths little-endian uint32):
/// ```
///   [edge_row_id:  uint64]
///   [name_len:     uint32][edge_type_name: bytes]
///   [database_id:  int32]
///   [source_type:  uint8][source_pk_bytes: serialized Value (null-flag + data)]
///   [target_type:  uint8][target_pk_bytes: serialized Value (null-flag + data)]
///   [prop_count:   uint32]
///   per property: [type: uint8][bytes: serialized Value]
/// ```
///
/// Migration safety: the deserializer bounds-checks every field. A legacy
/// short payload (old log_edge_wal format, < 13 bytes) is detected and
/// skipped with a WARN rather than crashing recovery.
struct EdgeWalPayload {
    uint64_t edge_row_id = 0;
    std::string edge_type_name;
    database_id_t database_id = 0;
    TypeId source_pk_type = TypeId::INT64;
    Value source_pk;
    TypeId target_pk_type = TypeId::INT64;
    Value target_pk;
    std::vector<TypeId> property_types;
    std::vector<Value> properties;
};

/// Serialize an edge WAL payload to bytes.
[[nodiscard]] std::vector<uint8_t> serialize_edge_wal_payload(const EdgeWalPayload& payload);

/// Deserialize an edge WAL payload from bytes.
/// Returns INVALID_ARGUMENT if the buffer is too short or malformed.
[[nodiscard]] Result<EdgeWalPayload> deserialize_edge_wal_payload(std::span<const uint8_t> data);

// -- Recovery handler ----------------------------------------------------------

/// Applies EDGE_INSERT / EDGE_DELETE WAL records to GraphEngine EdgeTables
/// during crash recovery (GDB-1067).
///
/// Register each EdgeTable by (database_id, edge_type_name) via register_edge_table().
/// Records for unregistered edge types are skipped. All operations are
/// idempotent per the RecoveryHandler contract:
///   - EDGE_INSERT redo: calls EdgeTable::restore_edge(); if the edge_row_id
///     already exists (from the data file), restore_edge() is idempotent.
///   - EDGE_DELETE redo: calls EdgeTable::delete_edge(); if the edge is
///     already absent, returns ok() without error.
///   - undo is a no-op (edges are autocommit / frozen_txn_id; aborted
///     edge records will not appear in the redo set and the undo set for
///     frozen records is empty by definition).
class GraphEngineRecoveryHandler : public RecoveryHandler {
public:
    /// Register an EdgeTable and its property type list for recovery.
    /// @param database_id  Database that owns this edge type.
    /// @param edge_type_name  Name of the edge type.
    /// @param table  The EdgeTable to replay into.
    /// @param property_types  TypeId for each property column (in declaration order).
    void register_edge_table(database_id_t database_id,
                             const std::string& edge_type_name,
                             EdgeTable* table,
                             std::vector<TypeId> property_types);

    [[nodiscard]] Result<void> redo(const WalRecord& record) override;

    /// Undo is intentionally a no-op: edge ops use frozen_txn_id (autocommit),
    /// so they are never placed in the aborted transaction set and undo() is
    /// never called for them.
    [[nodiscard]] Result<void> undo(const WalRecord& record) override;

private:
    struct EdgeTableEntry {
        EdgeTable* table = nullptr;
        std::vector<TypeId> property_types;
    };

    // Key: "database_id:edge_type_name"
    std::unordered_map<std::string, EdgeTableEntry> tables_;

    static std::string make_key(database_id_t database_id, const std::string& edge_type_name);
};

// -- Composite recovery handler ------------------------------------------------

/// Dispatches WAL records to two sub-handlers:
///   - EDGE_INSERT / EDGE_DELETE records go to the graph handler.
///   - All other data records go to the table handler.
///
/// This lets main.cpp wire a single RecoveryHandler into WalRecovery while
/// both the table heap and the graph engine are recovered correctly.
class CompositeRecoveryHandler : public RecoveryHandler {
public:
    CompositeRecoveryHandler(RecoveryHandler& table_handler, RecoveryHandler& graph_handler);

    [[nodiscard]] Result<void> redo(const WalRecord& record) override;
    [[nodiscard]] Result<void> undo(const WalRecord& record) override;

private:
    RecoveryHandler& table_handler_;
    RecoveryHandler& graph_handler_;
};

} // namespace sixseven
