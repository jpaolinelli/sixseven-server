#include "sixseven/storage/wal_record.h"

#include "sixseven/storage/disk_manager.h" // crc32c()

#include <cstring>

namespace sixseven {

// -- Serialization helpers ----------------------------------------------------

namespace {

/// Write a value to a byte buffer at the given offset using memcpy (safe,
/// unaligned-friendly, native byte order). Advances offset by sizeof(T).
template <typename T>
void write_native(std::vector<uint8_t>& buf, size_t& offset, T value) {
    std::memcpy(buf.data() + offset, &value, sizeof(T));
    offset += sizeof(T);
}

/// Read a value from a byte span at the given offset using memcpy (native
/// byte order). Advances offset by sizeof(T). Caller must ensure
/// offset + sizeof(T) <= buf.size().
template <typename T>
T read_native(std::span<const uint8_t> buf, size_t& offset) {
    T value{};
    std::memcpy(&value, buf.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

} // namespace

// -- Serialize ----------------------------------------------------------------

std::vector<uint8_t> serialize_wal_record(const WalRecord& record) {
    size_t total_size = serialized_wal_record_size(record);
    std::vector<uint8_t> buf(total_size);
    size_t offset = 0;

    // record_length: total bytes after this field (header + data + crc).
    auto record_length = static_cast<uint32_t>(total_size - sizeof(uint32_t));
    write_native(buf, offset, record_length);

    // Fixed header fields.
    size_t crc_start = offset; // CRC covers from here to end of data.
    write_native(buf, offset, record.lsn);
    write_native(buf, offset, record.txn_id);
    write_native(buf, offset, record.prev_lsn);
    write_native(buf, offset, static_cast<uint8_t>(record.type));
    write_native(buf, offset, record.table_id);
    write_native(buf, offset, record.page_id);
    write_native(buf, offset, record.slot_id);

    // Data length + data.
    auto data_length = static_cast<uint32_t>(record.data.size());
    write_native(buf, offset, data_length);
    if (!record.data.empty()) {
        std::memcpy(buf.data() + offset, record.data.data(), record.data.size());
        offset += record.data.size();
    }

    // CRC32C over header + data (everything between record_length and crc).
    size_t crc_length = offset - crc_start;
    uint32_t crc = crc32c(buf.data() + crc_start, crc_length);
    write_native(buf, offset, crc);

    return buf;
}

// -- Deserialize --------------------------------------------------------------

Result<WalRecord> deserialize_wal_record(std::span<const uint8_t> buf) {
    // Minimum size: record_length(4) + header(39) + crc(4) = 47.
    if (buf.size() < wal_record_overhead) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WAL record buffer too small: " + std::to_string(buf.size()) +
                              " bytes (minimum " + std::to_string(wal_record_overhead) + ")");
    }

    size_t offset = 0;

    // Read record_length.
    auto record_length = read_native<uint32_t>(buf, offset);

    // Validate total size: record_length + sizeof(record_length) must fit in buf.
    size_t expected_total = static_cast<size_t>(record_length) + sizeof(uint32_t);
    if (buf.size() < expected_total) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WAL record buffer too small for declared length: need " +
                              std::to_string(expected_total) + ", have " +
                              std::to_string(buf.size()));
    }

    // Validate record_length is large enough to hold at least the trailing CRC.
    if (record_length < sizeof(uint32_t)) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WAL record_length too small: " + std::to_string(record_length));
    }

    // CRC check: covers everything after record_length, excluding the trailing CRC.
    size_t crc_start = offset;
    size_t crc_length = static_cast<size_t>(record_length) - sizeof(uint32_t);
    uint32_t computed_crc = crc32c(buf.data() + crc_start, crc_length);

    // Read the stored CRC (last 4 bytes of the record).
    size_t crc_offset = crc_start + crc_length;
    uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, buf.data() + crc_offset, sizeof(uint32_t));

    if (stored_crc != computed_crc) {
        return make_error(StatusCode::IO_ERROR,
                          "WAL record CRC mismatch: stored=" + std::to_string(stored_crc) +
                              " computed=" + std::to_string(computed_crc));
    }

    // Deserialize fixed header fields.
    WalRecord record;
    record.lsn = read_native<uint64_t>(buf, offset);
    record.txn_id = read_native<uint64_t>(buf, offset);
    record.prev_lsn = read_native<uint64_t>(buf, offset);
    auto raw_type = read_native<uint8_t>(buf, offset);
    if (raw_type > static_cast<uint8_t>(WalRecordType::PROMOTE)) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WAL record has invalid type: " + std::to_string(raw_type));
    }
    record.type = static_cast<WalRecordType>(raw_type);
    record.table_id = read_native<uint32_t>(buf, offset);
    record.page_id = read_native<uint32_t>(buf, offset);
    record.slot_id = read_native<uint16_t>(buf, offset);

    // Data.
    auto data_length = read_native<uint32_t>(buf, offset);
    if (offset + data_length > buf.size()) {
        return make_error(
            StatusCode::INVALID_ARGUMENT,
            "WAL record data extends beyond buffer: offset=" + std::to_string(offset) +
                " data_length=" + std::to_string(data_length) +
                " buf_size=" + std::to_string(buf.size()));
    }
    if (data_length > 0) {
        record.data.resize(data_length);
        std::memcpy(record.data.data(), buf.data() + offset, data_length);
    }

    return ok(std::move(record));
}

} // namespace sixseven
