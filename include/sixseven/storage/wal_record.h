#pragma once

#include "sixseven/common/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sixseven {

/// Log Sequence Number — monotonically increasing identifier for WAL records.
using lsn_t = uint64_t;

/// Transaction identifier.
using txn_id_t = uint64_t;

/// Invalid/unset LSN sentinel.
inline constexpr lsn_t invalid_lsn = 0;

/// Invalid/unset transaction ID sentinel.
inline constexpr txn_id_t invalid_txn_id = 0;

/// "Frozen" transaction ID (GDB-714): marks a tuple as created (or deleted)
/// by an operation that is committed and visible to every snapshot, similar
/// to PostgreSQL's FrozenTransactionId. Used as the default xmin/xmax stamp
/// while DML runs without an enclosing transaction (autocommit) and for
/// bootstrap-written rows. The value is the maximum txn id so it can never
/// collide with a real id allocated by TransactionManager (which counts up
/// from 1); consumers (TransactionManager::get_status, MVCC visibility,
/// WAL recovery) special-case it as COMMITTED rather than comparing it
/// numerically against snapshot windows.
inline constexpr txn_id_t frozen_txn_id = ~txn_id_t{0};

// -- WAL Record Type ---------------------------------------------------------

/// Type of WAL record, identifying the operation that was performed.
enum class WalRecordType : uint8_t {
    BEGIN = 0,
    INSERT = 1,
    UPDATE = 2,
    DELETE = 3,
    PAGE_SPLIT = 4,
    COMMIT = 5,
    ABORT = 6,
    CHECKPOINT = 7,
    CREATE_TABLE = 8,
    DROP_TABLE = 9,
    PROMOTE = 10,
};

/// Return a human-readable name for a WalRecordType.
inline const char* wal_record_type_name(WalRecordType type) {
    switch (type) {
    case WalRecordType::BEGIN:
        return "BEGIN";
    case WalRecordType::INSERT:
        return "INSERT";
    case WalRecordType::UPDATE:
        return "UPDATE";
    case WalRecordType::DELETE:
        return "DELETE";
    case WalRecordType::PAGE_SPLIT:
        return "PAGE_SPLIT";
    case WalRecordType::COMMIT:
        return "COMMIT";
    case WalRecordType::ABORT:
        return "ABORT";
    case WalRecordType::CHECKPOINT:
        return "CHECKPOINT";
    case WalRecordType::CREATE_TABLE:
        return "CREATE_TABLE";
    case WalRecordType::DROP_TABLE:
        return "DROP_TABLE";
    case WalRecordType::PROMOTE:
        return "PROMOTE";
    }
    return "UNKNOWN";
}

// -- WAL Record --------------------------------------------------------------

/// A single WAL record representing an atomic change to the database.
///
/// Binary format (all fields in native byte order):
/// ```
///   [record_length: uint32]   — total bytes following this field (excl. itself)
///   [lsn:           uint64]   — log sequence number
///   [txn_id:        uint64]   — transaction ID
///   [prev_lsn:      uint64]   — previous LSN for this transaction (undo chain)
///   [type:          uint8]    — WalRecordType
///   [table_id:      uint32]   — target table ID (0 for txn-level records)
///   [page_id:       uint32]   — target page ID (0 for txn-level records)
///   [slot_id:       uint16]   — target slot ID (0 for txn-level records)
///   [data_length:   uint32]   — length of variable data payload
///   [data:          bytes]    — variable-length payload
///   [crc:           uint32]   — CRC32C of all preceding fields (excl. record_length)
/// ```
///
/// Fixed header size (after record_length): 39 bytes
/// Total overhead per record: 4 (length) + 39 (header) + 4 (crc) = 47 bytes + data
struct WalRecord {
    lsn_t lsn = invalid_lsn;          ///< Log sequence number.
    txn_id_t txn_id = invalid_txn_id; ///< Transaction that produced this record.
    lsn_t prev_lsn = invalid_lsn;     ///< Reserved for future undo-chain support.
    WalRecordType type = WalRecordType::BEGIN;
    uint32_t table_id = 0;     ///< Target table (0 for txn-level records).
    uint32_t page_id = 0;      ///< Target page (0 for txn-level records).
    uint16_t slot_id = 0;      ///< Target slot (0 for txn-level records).
    std::vector<uint8_t> data; ///< Variable-length payload.
};

/// Size of the fixed-length header fields (after record_length, before data).
/// lsn(8) + txn_id(8) + prev_lsn(8) + type(1) + table_id(4) + page_id(4) +
/// slot_id(2) + data_length(4) = 39 bytes.
inline constexpr size_t wal_record_header_size = 39;

/// Overhead per record: record_length(4) + header(39) + crc(4) = 47 bytes.
inline constexpr size_t wal_record_overhead = 47;

// -- Serialization -----------------------------------------------------------

/// Serialize a WAL record to a byte vector.
/// The output includes the record_length prefix, all header fields,
/// the variable-length data, and a trailing CRC32C.
[[nodiscard]] std::vector<uint8_t> serialize_wal_record(const WalRecord& record);

/// Deserialize a WAL record from a byte span.
/// The input must include the record_length prefix through the CRC.
/// Returns an error if the buffer is too short or the CRC is invalid.
[[nodiscard]] Result<WalRecord> deserialize_wal_record(std::span<const uint8_t> buf);

/// Compute the total serialized size of a WAL record.
[[nodiscard]] inline size_t serialized_wal_record_size(const WalRecord& record) {
    return wal_record_overhead + record.data.size();
}

} // namespace sixseven
