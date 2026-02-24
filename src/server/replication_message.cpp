#include "giodb/server/replication_message.h"

#include "giodb/storage/disk_manager.h" // crc32c()

#include <cstring>

namespace giodb {

// -- Helpers ------------------------------------------------------------------

namespace {

template <typename T>
void write_native(std::vector<uint8_t>& buf, size_t& offset, T value) {
    std::memcpy(buf.data() + offset, &value, sizeof(T));
    offset += sizeof(T);
}

template <typename T>
T read_native(std::span<const uint8_t> buf, size_t& offset) {
    T value{};
    std::memcpy(&value, buf.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

/// Write the common header: [type: uint8][payload_length: uint32].
void write_header(std::vector<uint8_t>& buf,
                  size_t& offset,
                  ReplicationMessageType type,
                  uint32_t payload_length) {
    write_native(buf, offset, static_cast<uint8_t>(type));
    write_native(buf, offset, payload_length);
}

/// Validate buffer has at least the header and the declared payload.
Result<void> validate_header(std::span<const uint8_t> buf, ReplicationMessageType expected_type) {
    if (buf.size() < replication_header_size) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "replication message buffer too small for header");
    }
    size_t offset = 0;
    auto raw_type = read_native<uint8_t>(buf, offset);
    if (raw_type != static_cast<uint8_t>(expected_type)) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "unexpected message type: expected " +
                              std::string(replication_message_type_name(expected_type)) + ", got " +
                              std::to_string(raw_type));
    }
    auto payload_length = read_native<uint32_t>(buf, offset);
    if (buf.size() < replication_header_size + payload_length) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "replication message buffer too small for payload");
    }
    return ok();
}

/// Validate CRC over the payload (everything between header and trailing CRC).
Result<void> validate_crc(std::span<const uint8_t> buf, size_t payload_start, size_t crc_offset) {
    size_t crc_data_len = crc_offset - payload_start;
    uint32_t computed = crc32c(buf.data() + payload_start, crc_data_len);
    uint32_t stored = 0;
    std::memcpy(&stored, buf.data() + crc_offset, sizeof(uint32_t));
    if (computed != stored) {
        return make_error(StatusCode::IO_ERROR,
                          "replication message CRC mismatch: stored=" + std::to_string(stored) +
                              " computed=" + std::to_string(computed));
    }
    return ok();
}

} // namespace

// -- Type name ----------------------------------------------------------------

const char* replication_message_type_name(ReplicationMessageType type) {
    switch (type) {
    case ReplicationMessageType::WAL_DATA:
        return "WAL_DATA";
    case ReplicationMessageType::KEEPALIVE:
        return "KEEPALIVE";
    case ReplicationMessageType::SEGMENT_START:
        return "SEGMENT_START";
    case ReplicationMessageType::CATCHUP_COMPLETE:
        return "CATCHUP_COMPLETE";
    case ReplicationMessageType::STANDBY_STATUS:
        return "STANDBY_STATUS";
    case ReplicationMessageType::HOT_STANDBY_FEEDBACK:
        return "HOT_STANDBY_FEEDBACK";
    }
    return "UNKNOWN";
}

// -- Serialize: WalData -------------------------------------------------------

std::vector<uint8_t> serialize_wal_data(const WalDataMessage& msg) {
    // Payload: start_lsn(8) + end_lsn(8) + record_count(4) + data(N) + crc(4)
    uint32_t payload_len = static_cast<uint32_t>(8 + 8 + 4 + msg.data.size() + 4);
    size_t total = replication_header_size + payload_len;
    std::vector<uint8_t> buf(total);
    size_t offset = 0;

    write_header(buf, offset, ReplicationMessageType::WAL_DATA, payload_len);

    size_t crc_start = offset;
    write_native(buf, offset, msg.start_lsn);
    write_native(buf, offset, msg.end_lsn);
    write_native(buf, offset, msg.record_count);
    if (!msg.data.empty()) {
        std::memcpy(buf.data() + offset, msg.data.data(), msg.data.size());
        offset += msg.data.size();
    }

    uint32_t crc = crc32c(buf.data() + crc_start, offset - crc_start);
    write_native(buf, offset, crc);

    return buf;
}

// -- Serialize: Keepalive -----------------------------------------------------

std::vector<uint8_t> serialize_keepalive(const KeepaliveMessage& msg) {
    // Payload: current_lsn(8) + timestamp_us(8) + reply_requested(1) + crc(4)
    uint32_t payload_len = 8 + 8 + 1 + 4;
    size_t total = replication_header_size + payload_len;
    std::vector<uint8_t> buf(total);
    size_t offset = 0;

    write_header(buf, offset, ReplicationMessageType::KEEPALIVE, payload_len);

    size_t crc_start = offset;
    write_native(buf, offset, msg.current_lsn);
    write_native(buf, offset, msg.timestamp_us);
    write_native(buf, offset, static_cast<uint8_t>(msg.reply_requested ? 1 : 0));

    uint32_t crc = crc32c(buf.data() + crc_start, offset - crc_start);
    write_native(buf, offset, crc);

    return buf;
}

// -- Serialize: SegmentStart --------------------------------------------------

std::vector<uint8_t> serialize_segment_start(const SegmentStartMessage& msg) {
    // Payload: segment_number(8) + crc(4)
    uint32_t payload_len = 8 + 4;
    size_t total = replication_header_size + payload_len;
    std::vector<uint8_t> buf(total);
    size_t offset = 0;

    write_header(buf, offset, ReplicationMessageType::SEGMENT_START, payload_len);

    size_t crc_start = offset;
    write_native(buf, offset, msg.segment_number);

    uint32_t crc = crc32c(buf.data() + crc_start, offset - crc_start);
    write_native(buf, offset, crc);

    return buf;
}

// -- Serialize: CatchupComplete -----------------------------------------------

std::vector<uint8_t> serialize_catchup_complete(const CatchupCompleteMessage& msg) {
    // Payload: current_lsn(8) + crc(4)
    uint32_t payload_len = 8 + 4;
    size_t total = replication_header_size + payload_len;
    std::vector<uint8_t> buf(total);
    size_t offset = 0;

    write_header(buf, offset, ReplicationMessageType::CATCHUP_COMPLETE, payload_len);

    size_t crc_start = offset;
    write_native(buf, offset, msg.current_lsn);

    uint32_t crc = crc32c(buf.data() + crc_start, offset - crc_start);
    write_native(buf, offset, crc);

    return buf;
}

// -- Serialize: StandbyStatus -------------------------------------------------

std::vector<uint8_t> serialize_standby_status(const StandbyStatusMessage& msg) {
    // Payload: received(8) + applied(8) + flushed(8) + timestamp(8) + crc(4)
    uint32_t payload_len = 8 + 8 + 8 + 8 + 4;
    size_t total = replication_header_size + payload_len;
    std::vector<uint8_t> buf(total);
    size_t offset = 0;

    write_header(buf, offset, ReplicationMessageType::STANDBY_STATUS, payload_len);

    size_t crc_start = offset;
    write_native(buf, offset, msg.received_lsn);
    write_native(buf, offset, msg.applied_lsn);
    write_native(buf, offset, msg.flushed_lsn);
    write_native(buf, offset, msg.timestamp_us);

    uint32_t crc = crc32c(buf.data() + crc_start, offset - crc_start);
    write_native(buf, offset, crc);

    return buf;
}

// -- Serialize: HotStandbyFeedback --------------------------------------------

std::vector<uint8_t> serialize_hot_standby_feedback(const HotStandbyFeedbackMessage& msg) {
    // Payload: xmin(8) + timestamp(8) + crc(4)
    uint32_t payload_len = 8 + 8 + 4;
    size_t total = replication_header_size + payload_len;
    std::vector<uint8_t> buf(total);
    size_t offset = 0;

    write_header(buf, offset, ReplicationMessageType::HOT_STANDBY_FEEDBACK, payload_len);

    size_t crc_start = offset;
    write_native(buf, offset, msg.xmin);
    write_native(buf, offset, msg.timestamp_us);

    uint32_t crc = crc32c(buf.data() + crc_start, offset - crc_start);
    write_native(buf, offset, crc);

    return buf;
}

// -- Peek / total size --------------------------------------------------------

Result<ReplicationMessageType> peek_message_type(std::span<const uint8_t> buf) {
    if (buf.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "empty buffer for peek_message_type");
    }
    auto raw = buf[0];
    if (raw < 1 || raw > 6) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid replication message type: " + std::to_string(raw));
    }
    return ok(static_cast<ReplicationMessageType>(raw));
}

Result<size_t> message_total_size(std::span<const uint8_t> buf) {
    if (buf.size() < replication_header_size) {
        return make_error(StatusCode::INVALID_ARGUMENT, "buffer too small for message header");
    }
    size_t offset = 1; // skip type byte
    auto payload_len = read_native<uint32_t>(buf, offset);
    return ok(replication_header_size + static_cast<size_t>(payload_len));
}

// -- Deserialize: WalData -----------------------------------------------------

Result<WalDataMessage> deserialize_wal_data(std::span<const uint8_t> buf) {
    auto hdr = validate_header(buf, ReplicationMessageType::WAL_DATA);
    if (!hdr)
        return make_error(hdr.error().code, hdr.error().message);

    size_t offset = 1; // skip type
    auto payload_len = read_native<uint32_t>(buf, offset);

    // Minimum payload: start_lsn(8) + end_lsn(8) + record_count(4) + crc(4) = 24
    if (payload_len < 24) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WAL_DATA payload too small");
    }

    size_t crc_start = offset;
    WalDataMessage msg;
    msg.start_lsn = read_native<uint64_t>(buf, offset);
    msg.end_lsn = read_native<uint64_t>(buf, offset);
    msg.record_count = read_native<uint32_t>(buf, offset);

    size_t data_len = payload_len - 24; // subtract fixed fields + crc
    if (data_len > 0) {
        msg.data.resize(data_len);
        std::memcpy(msg.data.data(), buf.data() + offset, data_len);
        offset += data_len;
    }

    auto crc_result = validate_crc(buf, crc_start, offset);
    if (!crc_result)
        return make_error(crc_result.error().code, crc_result.error().message);

    return ok(std::move(msg));
}

// -- Deserialize: Keepalive ---------------------------------------------------

Result<KeepaliveMessage> deserialize_keepalive(std::span<const uint8_t> buf) {
    auto hdr = validate_header(buf, ReplicationMessageType::KEEPALIVE);
    if (!hdr)
        return make_error(hdr.error().code, hdr.error().message);

    size_t offset = replication_header_size;
    size_t crc_start = offset;

    KeepaliveMessage msg;
    msg.current_lsn = read_native<uint64_t>(buf, offset);
    msg.timestamp_us = read_native<uint64_t>(buf, offset);
    msg.reply_requested = read_native<uint8_t>(buf, offset) != 0;

    auto crc_result = validate_crc(buf, crc_start, offset);
    if (!crc_result)
        return make_error(crc_result.error().code, crc_result.error().message);

    return ok(msg);
}

// -- Deserialize: SegmentStart ------------------------------------------------

Result<SegmentStartMessage> deserialize_segment_start(std::span<const uint8_t> buf) {
    auto hdr = validate_header(buf, ReplicationMessageType::SEGMENT_START);
    if (!hdr)
        return make_error(hdr.error().code, hdr.error().message);

    size_t offset = replication_header_size;
    size_t crc_start = offset;

    SegmentStartMessage msg;
    msg.segment_number = read_native<uint64_t>(buf, offset);

    auto crc_result = validate_crc(buf, crc_start, offset);
    if (!crc_result)
        return make_error(crc_result.error().code, crc_result.error().message);

    return ok(msg);
}

// -- Deserialize: CatchupComplete ---------------------------------------------

Result<CatchupCompleteMessage> deserialize_catchup_complete(std::span<const uint8_t> buf) {
    auto hdr = validate_header(buf, ReplicationMessageType::CATCHUP_COMPLETE);
    if (!hdr)
        return make_error(hdr.error().code, hdr.error().message);

    size_t offset = replication_header_size;
    size_t crc_start = offset;

    CatchupCompleteMessage msg;
    msg.current_lsn = read_native<uint64_t>(buf, offset);

    auto crc_result = validate_crc(buf, crc_start, offset);
    if (!crc_result)
        return make_error(crc_result.error().code, crc_result.error().message);

    return ok(msg);
}

// -- Deserialize: StandbyStatus -----------------------------------------------

Result<StandbyStatusMessage> deserialize_standby_status(std::span<const uint8_t> buf) {
    auto hdr = validate_header(buf, ReplicationMessageType::STANDBY_STATUS);
    if (!hdr)
        return make_error(hdr.error().code, hdr.error().message);

    size_t offset = replication_header_size;
    size_t crc_start = offset;

    StandbyStatusMessage msg;
    msg.received_lsn = read_native<uint64_t>(buf, offset);
    msg.applied_lsn = read_native<uint64_t>(buf, offset);
    msg.flushed_lsn = read_native<uint64_t>(buf, offset);
    msg.timestamp_us = read_native<uint64_t>(buf, offset);

    auto crc_result = validate_crc(buf, crc_start, offset);
    if (!crc_result)
        return make_error(crc_result.error().code, crc_result.error().message);

    return ok(msg);
}

// -- Deserialize: HotStandbyFeedback ------------------------------------------

Result<HotStandbyFeedbackMessage> deserialize_hot_standby_feedback(std::span<const uint8_t> buf) {
    auto hdr = validate_header(buf, ReplicationMessageType::HOT_STANDBY_FEEDBACK);
    if (!hdr)
        return make_error(hdr.error().code, hdr.error().message);

    size_t offset = replication_header_size;
    size_t crc_start = offset;

    HotStandbyFeedbackMessage msg;
    msg.xmin = read_native<uint64_t>(buf, offset);
    msg.timestamp_us = read_native<uint64_t>(buf, offset);

    auto crc_result = validate_crc(buf, crc_start, offset);
    if (!crc_result)
        return make_error(crc_result.error().code, crc_result.error().message);

    return ok(msg);
}

} // namespace giodb
