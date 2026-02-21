#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/wal_record.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <vector>

namespace giodb {

// -- Recovery Handler ---------------------------------------------------------

/// Abstract interface for redo/undo callbacks during WAL recovery.
///
/// The recovery process calls redo() for each data-modifying record of
/// committed transactions (in forward order) and undo() for each record of
/// aborted or in-progress transactions (in reverse order).
///
/// Implementors should apply or reverse the operation described by the
/// WalRecord to the database state (e.g., via the buffer pool).
///
/// @par Idempotency contract
/// redo() **must** be idempotent: applying the same record twice must produce
/// the same result as applying it once. Recovery may replay records for pages
/// that were already flushed to disk before the crash.
class RecoveryHandler {
public:
    virtual ~RecoveryHandler() = default;

    /// Apply the operation described by the WAL record (forward replay).
    /// Must be idempotent — see class-level documentation.
    [[nodiscard]] virtual Result<void> redo(const WalRecord& record) = 0;

    /// Reverse the operation described by the WAL record (backward rollback).
    [[nodiscard]] virtual Result<void> undo(const WalRecord& record) = 0;
};

// -- Recovery Statistics ------------------------------------------------------

/// Statistics collected during WAL recovery.
struct RecoveryStats {
    size_t records_scanned = 0; ///< Total records read from WAL.
    size_t records_redone = 0;  ///< Records replayed in redo phase.
    size_t records_undone = 0;  ///< Records reversed in undo phase.
    size_t committed_txns = 0;  ///< Transactions that committed.
    size_t aborted_txns = 0;    ///< Transactions that aborted or were in-progress.
    lsn_t checkpoint_lsn = 0;   ///< LSN of the last checkpoint found (0 if none).
    lsn_t max_lsn = 0;          ///< Highest LSN seen during recovery.

    /// Txn IDs that were committed (useful for debugging/diagnostics).
    std::set<txn_id_t> committed_txn_ids;
    /// Txn IDs that were aborted or in-progress at crash (diagnostics).
    std::set<txn_id_t> aborted_txn_ids;
};

// -- WAL Recovery -------------------------------------------------------------

/// Implements ARIES-style crash recovery using the Write-Ahead Log.
///
/// Recovery proceeds in three logical phases:
///
/// 1. **Analysis**: Scan the WAL forward from the last CHECKPOINT record
///    to determine which transactions committed, aborted, or were still
///    in-progress at the time of the crash.
///
/// 2. **Redo**: Replay all data-modifying records of committed transactions
///    in forward (LSN) order via RecoveryHandler::redo().
///
/// 3. **Undo**: Reverse all data-modifying records of aborted and in-progress
///    transactions in reverse (LSN) order via RecoveryHandler::undo().
///
/// Data-modifying record types: INSERT, UPDATE, DELETE, PAGE_SPLIT,
/// CREATE_TABLE, DROP_TABLE.
///
/// Usage:
/// ```
///   MyRecoveryHandler handler(buffer_pool);
///   WalRecovery recovery(wal_dir, handler);
///   auto stats = recovery.recover();
/// ```
class WalRecovery {
public:
    /// Construct a recovery instance.
    /// @param wal_dir Path to the WAL directory containing segment files.
    /// @param handler Callback handler for redo/undo operations.
    WalRecovery(std::filesystem::path wal_dir, RecoveryHandler& handler);

    /// Run the full recovery process: analysis → redo → undo.
    /// Returns recovery statistics on success.
    [[nodiscard]] Result<RecoveryStats> recover();

private:
    /// Return true if the record type represents a data-modifying operation
    /// that should be redone/undone.
    static bool is_data_record(WalRecordType type);

    std::filesystem::path wal_dir_;
    RecoveryHandler& handler_;
};

// -- Checkpoint helpers -------------------------------------------------------

/// Serialize a list of active transaction IDs into a CHECKPOINT record's
/// data payload. Format: [count: uint32][txn_id: uint64]...
[[nodiscard]] std::vector<uint8_t>
serialize_checkpoint_data(const std::vector<txn_id_t>& active_txns);

/// Deserialize a CHECKPOINT record's data payload into a list of active
/// transaction IDs.
[[nodiscard]] Result<std::vector<txn_id_t>>
deserialize_checkpoint_data(const std::vector<uint8_t>& data);

} // namespace giodb
