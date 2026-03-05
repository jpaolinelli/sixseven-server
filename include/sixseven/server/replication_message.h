#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/wal_record.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sixseven {

/// Type tag for replication protocol messages.
enum class ReplicationMessageType : uint8_t {
    // Primary -> Replica
    WAL_DATA = 1,
    KEEPALIVE = 2,
    SEGMENT_START = 3,
    CATCHUP_COMPLETE = 4,

    // Replica -> Primary
    STANDBY_STATUS = 5,
    HOT_STANDBY_FEEDBACK = 6,
};

/// Return a human-readable name for a ReplicationMessageType.
const char* replication_message_type_name(ReplicationMessageType type);

/// Common header size: type(1) + payload_length(4) = 5 bytes.
inline constexpr size_t replication_header_size = 5;

// -- Primary -> Replica messages ----------------------------------------------

/// Batch of WAL records sent from primary to replica.
struct WalDataMessage {
    lsn_t start_lsn = invalid_lsn; ///< LSN of first WAL record in batch.
    lsn_t end_lsn = invalid_lsn;   ///< LSN after last WAL record in batch.
    uint32_t record_count = 0;     ///< Number of WAL records.
    std::vector<uint8_t> data;     ///< Concatenated serialized WAL records.
};

/// Heartbeat from primary to replica.
struct KeepaliveMessage {
    lsn_t current_lsn = invalid_lsn; ///< Primary's current WAL LSN.
    uint64_t timestamp_us = 0;       ///< Microseconds since epoch.
    bool reply_requested = false;    ///< Whether replica must respond.
};

/// Indicates the start of a new WAL segment.
struct SegmentStartMessage {
    uint64_t segment_number = 0;
};

/// Indicates the replica has caught up to real-time streaming.
struct CatchupCompleteMessage {
    lsn_t current_lsn = invalid_lsn; ///< Primary's LSN at catch-up completion.
};

// -- Replica -> Primary messages ----------------------------------------------

/// Progress report from replica to primary.
struct StandbyStatusMessage {
    lsn_t received_lsn = invalid_lsn; ///< Latest WAL received.
    lsn_t applied_lsn = invalid_lsn;  ///< Latest WAL applied to data.
    lsn_t flushed_lsn = invalid_lsn;  ///< Latest WAL durably flushed.
    uint64_t timestamp_us = 0;        ///< Microseconds since epoch.
};

/// Hot standby feedback from replica to primary.
struct HotStandbyFeedbackMessage {
    uint64_t xmin = 0;         ///< Oldest active transaction on replica.
    uint64_t timestamp_us = 0; ///< Microseconds since epoch.
};

// -- Serialization ------------------------------------------------------------

[[nodiscard]] std::vector<uint8_t> serialize_wal_data(const WalDataMessage& msg);
[[nodiscard]] std::vector<uint8_t> serialize_keepalive(const KeepaliveMessage& msg);
[[nodiscard]] std::vector<uint8_t> serialize_segment_start(const SegmentStartMessage& msg);
[[nodiscard]] std::vector<uint8_t> serialize_catchup_complete(const CatchupCompleteMessage& msg);
[[nodiscard]] std::vector<uint8_t> serialize_standby_status(const StandbyStatusMessage& msg);
[[nodiscard]] std::vector<uint8_t>
serialize_hot_standby_feedback(const HotStandbyFeedbackMessage& msg);

// -- Deserialization ----------------------------------------------------------

/// Peek at the message type from the first byte.
[[nodiscard]] Result<ReplicationMessageType> peek_message_type(std::span<const uint8_t> buf);

/// Get the total message size (header + payload) from the buffer.
/// Buffer must be >= replication_header_size bytes.
[[nodiscard]] Result<size_t> message_total_size(std::span<const uint8_t> buf);

[[nodiscard]] Result<WalDataMessage> deserialize_wal_data(std::span<const uint8_t> buf);
[[nodiscard]] Result<KeepaliveMessage> deserialize_keepalive(std::span<const uint8_t> buf);
[[nodiscard]] Result<SegmentStartMessage> deserialize_segment_start(std::span<const uint8_t> buf);
[[nodiscard]] Result<CatchupCompleteMessage>
deserialize_catchup_complete(std::span<const uint8_t> buf);
[[nodiscard]] Result<StandbyStatusMessage> deserialize_standby_status(std::span<const uint8_t> buf);
[[nodiscard]] Result<HotStandbyFeedbackMessage>
deserialize_hot_standby_feedback(std::span<const uint8_t> buf);

} // namespace sixseven
