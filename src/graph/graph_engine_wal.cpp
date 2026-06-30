#include "sixseven/graph/graph_engine_wal.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/serialization.h"

#include <cstring>
#include <string>

namespace sixseven {

// -- Serialization helpers -----------------------------------------------------

static void append_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

static void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    uint8_t tmp[4];
    std::memcpy(tmp, &v, 4);
    buf.insert(buf.end(), tmp, tmp + 4);
}

static void append_u64(std::vector<uint8_t>& buf, uint64_t v) {
    uint8_t tmp[8];
    std::memcpy(tmp, &v, 8);
    buf.insert(buf.end(), tmp, tmp + 8);
}

static void append_i32(std::vector<uint8_t>& buf, int32_t v) {
    uint8_t tmp[4];
    std::memcpy(tmp, &v, 4);
    buf.insert(buf.end(), tmp, tmp + 4);
}

static void append_bytes(std::vector<uint8_t>& buf, const std::vector<uint8_t>& src) {
    buf.insert(buf.end(), src.begin(), src.end());
}

// Read helpers that advance an offset pointer.  Each returns false on underflow.
static bool read_u8(std::span<const uint8_t> data, size_t& off, uint8_t& out) {
    if (off + 1 > data.size()) return false;
    out = data[off];
    off += 1;
    return true;
}

static bool read_u32(std::span<const uint8_t> data, size_t& off, uint32_t& out) {
    if (off + 4 > data.size()) return false;
    std::memcpy(&out, data.data() + off, 4);
    off += 4;
    return true;
}

static bool read_u64(std::span<const uint8_t> data, size_t& off, uint64_t& out) {
    if (off + 8 > data.size()) return false;
    std::memcpy(&out, data.data() + off, 8);
    off += 8;
    return true;
}

static bool read_i32(std::span<const uint8_t> data, size_t& off, int32_t& out) {
    if (off + 4 > data.size()) return false;
    std::memcpy(&out, data.data() + off, 4);
    off += 4;
    return true;
}

// -- Payload implementation ----------------------------------------------------

std::vector<uint8_t> serialize_edge_wal_payload(const EdgeWalPayload& payload) {
    std::vector<uint8_t> buf;
    // Reserve approximate size.
    buf.reserve(64 + payload.edge_type_name.size() + payload.properties.size() * 16);

    // edge_row_id
    append_u64(buf, payload.edge_row_id);
    // edge_type_name
    auto name_len = static_cast<uint32_t>(payload.edge_type_name.size());
    append_u32(buf, name_len);
    buf.insert(buf.end(), payload.edge_type_name.begin(), payload.edge_type_name.end());
    // database_id
    append_i32(buf, payload.database_id);
    // source_pk
    append_u8(buf, static_cast<uint8_t>(payload.source_pk_type));
    append_bytes(buf, serialize(payload.source_pk));
    // target_pk
    append_u8(buf, static_cast<uint8_t>(payload.target_pk_type));
    append_bytes(buf, serialize(payload.target_pk));
    // properties
    auto prop_count = static_cast<uint32_t>(payload.properties.size());
    append_u32(buf, prop_count);
    for (size_t i = 0; i < payload.properties.size(); ++i) {
        TypeId type_id =
            (i < payload.property_types.size()) ? payload.property_types[i] : TypeId::INT64;
        append_u8(buf, static_cast<uint8_t>(type_id));
        append_bytes(buf, serialize(payload.properties[i]));
    }
    return buf;
}

Result<EdgeWalPayload> deserialize_edge_wal_payload(std::span<const uint8_t> data) {
    // Minimum size: u64 + u32 + 0-name + i32 + u8 + 2-byte-min-value + u8 + 2 + u32 = 21 bytes
    // Old format was: u64 + u32 + name -> much smaller with no database_id or PKs.
    // We detect old records by checking if they're too short to hold the minimum new fields
    // after the name (need at least i32 + u8 + 1[null] + u8 + 1[null] + u32 = 11 more).
    constexpr size_t k_min_header = 8 + 4; // edge_row_id + name_len
    if (data.size() < k_min_header) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "edge WAL payload too short (< 12 bytes) -- likely legacy record, skip");
    }

    size_t off = 0;
    EdgeWalPayload out;

    uint64_t row_id = 0;
    if (!read_u64(data, off, row_id)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated edge_row_id");
    }
    out.edge_row_id = row_id;

    uint32_t name_len = 0;
    if (!read_u32(data, off, name_len)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated name_len");
    }
    if (off + name_len > data.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated edge_type_name");
    }
    out.edge_type_name.assign(reinterpret_cast<const char*>(data.data() + off), name_len);
    off += name_len;

    // Check if this is a new-format record (has database_id and PKs) or legacy.
    // New format needs at least: i32(4) + u8(1) + 1(null-flag) + u8(1) + 1(null-flag) + u32(4)
    // = 12 more bytes after the name.
    constexpr size_t k_min_after_name = 4 + 1 + 1 + 1 + 1 + 4; // = 12
    if (off + k_min_after_name > data.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "edge WAL: legacy short payload (missing PK/property data) -- skip");
    }

    int32_t db_id = 0;
    if (!read_i32(data, off, db_id)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated database_id");
    }
    out.database_id = db_id;

    // source_pk
    uint8_t src_type_raw = 0;
    if (!read_u8(data, off, src_type_raw)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated source_pk type");
    }
    out.source_pk_type = static_cast<TypeId>(src_type_raw);
    {
        auto src_val = deserialize(data.subspan(off), out.source_pk_type);
        if (!src_val) {
            return make_error(src_val.error().code,
                              "edge WAL: source_pk deserialize: " + src_val.error().message);
        }
        out.source_pk = std::move(*src_val);
        // Advance offset by the serialized size.
        off += serialized_size(out.source_pk);
        if (off > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "edge WAL: offset past end after source_pk");
        }
    }

    // target_pk
    uint8_t tgt_type_raw = 0;
    if (!read_u8(data, off, tgt_type_raw)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated target_pk type");
    }
    out.target_pk_type = static_cast<TypeId>(tgt_type_raw);
    {
        auto tgt_val = deserialize(data.subspan(off), out.target_pk_type);
        if (!tgt_val) {
            return make_error(tgt_val.error().code,
                              "edge WAL: target_pk deserialize: " + tgt_val.error().message);
        }
        out.target_pk = std::move(*tgt_val);
        off += serialized_size(out.target_pk);
        if (off > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "edge WAL: offset past end after target_pk");
        }
    }

    // properties
    uint32_t prop_count = 0;
    if (!read_u32(data, off, prop_count)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated prop_count");
    }
    out.properties.reserve(prop_count);
    out.property_types.reserve(prop_count);
    for (uint32_t i = 0; i < prop_count; ++i) {
        uint8_t type_raw = 0;
        if (!read_u8(data, off, type_raw)) {
            return make_error(StatusCode::INVALID_ARGUMENT, "edge WAL: truncated property type");
        }
        TypeId prop_type = static_cast<TypeId>(type_raw);
        out.property_types.push_back(prop_type);

        auto prop_val = deserialize(data.subspan(off), prop_type);
        if (!prop_val) {
            return make_error(prop_val.error().code,
                              "edge WAL: property[" + std::to_string(i) +
                                  "] deserialize: " + prop_val.error().message);
        }
        out.properties.push_back(std::move(*prop_val));
        off += serialized_size(out.properties.back());
        if (off > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "edge WAL: offset past end after property " + std::to_string(i));
        }
    }

    return ok(std::move(out));
}

// -- GraphEngineRecoveryHandler ------------------------------------------------

std::string GraphEngineRecoveryHandler::make_key(database_id_t database_id,
                                                 const std::string& edge_type_name) {
    return std::to_string(database_id) + ":" + edge_type_name;
}

void GraphEngineRecoveryHandler::register_edge_table(database_id_t database_id,
                                                     const std::string& edge_type_name,
                                                     EdgeTable* table,
                                                     std::vector<TypeId> property_types) {
    auto key = make_key(database_id, edge_type_name);
    tables_[key] = {table, std::move(property_types)};
}

Result<void> GraphEngineRecoveryHandler::redo(const WalRecord& record) {
    if (record.type != WalRecordType::EDGE_INSERT &&
        record.type != WalRecordType::EDGE_DELETE) {
        return ok(); // Not an edge record -- pass through.
    }

    auto payload = deserialize_edge_wal_payload(record.data);
    if (!payload) {
        // Legacy short payload or malformed -- warn and skip rather than
        // aborting recovery.
        SIXSEVEN_LOG_WARN("edge WAL redo: skipping malformed/legacy record at lsn={}: {}",
                          record.lsn,
                          payload.error().message);
        return ok();
    }

    auto key = make_key(payload->database_id, payload->edge_type_name);
    auto it = tables_.find(key);
    if (it == tables_.end()) {
        // Edge type not registered -- could happen if the edge type was
        // subsequently dropped.  Skip silently.
        SIXSEVEN_LOG_DEBUG("edge WAL redo: no registered table for '{}' (db={}) -- skipped",
                           payload->edge_type_name,
                           payload->database_id);
        return ok();
    }

    EdgeTable* table = it->second.table;

    if (record.type == WalRecordType::EDGE_INSERT) {
        // restore_edge is idempotent: if edge_row_id already exists (because
        // the heap file was flushed and load_edges() already loaded it), it
        // updates next_row_id_ and returns ok() without double-inserting.
        auto res = table->restore_edge(payload->edge_row_id,
                                       payload->source_pk,
                                       payload->target_pk,
                                       payload->properties);
        if (!res) {
            return make_error(res.error().code,
                              "edge WAL redo INSERT edge_row_id=" +
                                  std::to_string(payload->edge_row_id) +
                                  ": " + res.error().message);
        }
        SIXSEVEN_LOG_DEBUG("edge WAL redo: restored edge_row_id={} for '{}'",
                           payload->edge_row_id,
                           payload->edge_type_name);
    } else {
        // EDGE_DELETE: remove the edge.  If it's already absent (idempotent),
        // delete_edge returns NOT_FOUND; we treat that as success.
        auto res = table->delete_edge(payload->edge_row_id);
        if (!res) {
            if (res.error().code == StatusCode::NOT_FOUND) {
                SIXSEVEN_LOG_DEBUG(
                    "edge WAL redo DELETE: edge_row_id={} already absent -- ok",
                    payload->edge_row_id);
                return ok();
            }
            return make_error(res.error().code,
                              "edge WAL redo DELETE edge_row_id=" +
                                  std::to_string(payload->edge_row_id) +
                                  ": " + res.error().message);
        }
        SIXSEVEN_LOG_DEBUG("edge WAL redo: deleted edge_row_id={} for '{}'",
                           payload->edge_row_id,
                           payload->edge_type_name);
    }

    return ok();
}

Result<void> GraphEngineRecoveryHandler::undo(const WalRecord& record) {
    // Edge operations use frozen_txn_id (autocommit).  The WAL recovery
    // analysis phase never adds frozen_txn_id to aborted_txns, so undo()
    // is never called for edge records in practice.  Guard defensively.
    if (record.type == WalRecordType::EDGE_INSERT ||
        record.type == WalRecordType::EDGE_DELETE) {
        SIXSEVEN_LOG_WARN("edge WAL undo called unexpectedly for lsn={} -- no-op", record.lsn);
    }
    return ok();
}

// -- CompositeRecoveryHandler --------------------------------------------------

CompositeRecoveryHandler::CompositeRecoveryHandler(RecoveryHandler& table_handler,
                                                   RecoveryHandler& graph_handler)
    : table_handler_(table_handler), graph_handler_(graph_handler) {}

Result<void> CompositeRecoveryHandler::redo(const WalRecord& record) {
    if (record.type == WalRecordType::EDGE_INSERT ||
        record.type == WalRecordType::EDGE_DELETE) {
        return graph_handler_.redo(record);
    }
    return table_handler_.redo(record);
}

Result<void> CompositeRecoveryHandler::undo(const WalRecord& record) {
    if (record.type == WalRecordType::EDGE_INSERT ||
        record.type == WalRecordType::EDGE_DELETE) {
        return graph_handler_.undo(record);
    }
    return table_handler_.undo(record);
}

} // namespace sixseven
