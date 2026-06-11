#include "sixseven/table/table_wal.h"

#include "sixseven/txn/mvcc_tuple.h"

#include <cstring>

namespace sixseven {

// -- Payload serialization -----------------------------------------------------

std::vector<uint8_t> serialize_table_wal_payload(std::span<const uint8_t> before_image,
                                                 std::span<const uint8_t> after_image) {
    std::vector<uint8_t> buf(2 * sizeof(uint32_t) + before_image.size() + after_image.size());
    size_t offset = 0;

    auto before_len = static_cast<uint32_t>(before_image.size());
    std::memcpy(buf.data() + offset, &before_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (!before_image.empty()) {
        std::memcpy(buf.data() + offset, before_image.data(), before_image.size());
        offset += before_image.size();
    }

    auto after_len = static_cast<uint32_t>(after_image.size());
    std::memcpy(buf.data() + offset, &after_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (!after_image.empty()) {
        std::memcpy(buf.data() + offset, after_image.data(), after_image.size());
    }

    return buf;
}

std::vector<uint8_t> serialize_table_wal_payload(const TableWalPayload& payload) {
    return serialize_table_wal_payload(payload.before_image, payload.after_image);
}

Result<TableWalPayload> deserialize_table_wal_payload(std::span<const uint8_t> data) {
    size_t offset = 0;

    auto read_image = [&](std::vector<uint8_t>& out) -> Result<void> {
        if (data.size() < offset + sizeof(uint32_t)) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "table WAL payload truncated (length field)");
        }
        uint32_t len = 0;
        std::memcpy(&len, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        if (data.size() < offset + len) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "table WAL payload truncated (image bytes)");
        }
        out.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                   data.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
        return ok();
    };

    TableWalPayload payload;
    auto before = read_image(payload.before_image);
    if (!before) {
        return tl::unexpected(before.error());
    }
    auto after = read_image(payload.after_image);
    if (!after) {
        return tl::unexpected(after.error());
    }
    if (offset != data.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "table WAL payload has trailing bytes");
    }
    return ok(std::move(payload));
}

// -- TableHeapRecoveryHandler ----------------------------------------------------

void TableHeapRecoveryHandler::register_table(uint32_t table_id, TableHeap* heap) {
    tables_[table_id] = heap;
}

Result<void> TableHeapRecoveryHandler::redo(const WalRecord& record) {
    auto it = tables_.find(record.table_id);
    if (it == tables_.end()) {
        // Not a registered table heap record (e.g. index/graph subsystem
        // records sharing the WAL stream) — nothing to do.
        return ok();
    }
    TableHeap* heap = it->second;
    RID rid{record.page_id, static_cast<SlotId>(record.slot_id)};

    switch (record.type) {
    case WalRecordType::INSERT:
    case WalRecordType::UPDATE: {
        auto payload = deserialize_table_wal_payload(record.data);
        if (!payload) {
            return tl::unexpected(payload.error());
        }
        if (payload->after_image.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "table WAL redo record has no after-image");
        }
        return heap->restore_raw_tuple(rid, payload->after_image);
    }
    case WalRecordType::DELETE:
        return heap->delete_raw_tuple(rid);
    default:
        return ok(); // Non-tuple record types are not handled here.
    }
}

Result<void> TableHeapRecoveryHandler::undo(const WalRecord& record) {
    auto it = tables_.find(record.table_id);
    if (it == tables_.end()) {
        return ok();
    }
    TableHeap* heap = it->second;
    RID rid{record.page_id, static_cast<SlotId>(record.slot_id)};

    switch (record.type) {
    case WalRecordType::INSERT:
        return heap->delete_raw_tuple(rid);
    case WalRecordType::UPDATE: {
        auto payload = deserialize_table_wal_payload(record.data);
        if (!payload) {
            return tl::unexpected(payload.error());
        }
        if (payload->before_image.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "table WAL undo record has no before-image");
        }
        return heap->restore_raw_tuple(rid, payload->before_image);
    }
    case WalRecordType::DELETE: {
        auto payload = deserialize_table_wal_payload(record.data);
        if (!payload) {
            return tl::unexpected(payload.error());
        }
        if (payload->before_image.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "table WAL undo record has no before-image");
        }
        // The deleting transaction did not commit: restore the tuple and
        // clear the stamped xmax so the version is live again.
        std::vector<uint8_t> image = payload->before_image;
        if (heap->mvcc_headers() && image.size() >= mvcc_header_size) {
            write_mvcc_xmax(image.data(), invalid_txn_id);
        }
        return heap->restore_raw_tuple(rid, image);
    }
    default:
        return ok();
    }
}

} // namespace sixseven
