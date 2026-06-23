#include "sixseven/storage/wal_recovery.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/wal.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace sixseven {

// -- Checkpoint data helpers --------------------------------------------------

std::vector<uint8_t> serialize_checkpoint_data(const std::vector<txn_id_t>& active_txns) {
    size_t data_size = sizeof(uint32_t) + active_txns.size() * sizeof(uint64_t);
    std::vector<uint8_t> data(data_size);
    size_t offset = 0;

    auto count = static_cast<uint32_t>(active_txns.size());
    std::memcpy(data.data() + offset, &count, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    for (auto txn_id : active_txns) {
        std::memcpy(data.data() + offset, &txn_id, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    return data;
}

Result<std::vector<txn_id_t>> deserialize_checkpoint_data(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(uint32_t)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "checkpoint data too small");
    }

    uint32_t count = 0;
    std::memcpy(&count, data.data(), sizeof(uint32_t));

    size_t expected_size = sizeof(uint32_t) + static_cast<size_t>(count) * sizeof(uint64_t);
    if (data.size() < expected_size) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "checkpoint data truncated: expected " + std::to_string(expected_size) +
                              " bytes, got " + std::to_string(data.size()));
    }

    std::vector<txn_id_t> txns;
    txns.reserve(count);
    size_t offset = sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        txn_id_t txn_id = 0;
        std::memcpy(&txn_id, data.data() + offset, sizeof(uint64_t));
        txns.push_back(txn_id);
        offset += sizeof(uint64_t);
    }

    return ok(std::move(txns));
}

// -- WalRecovery --------------------------------------------------------------

WalRecovery::WalRecovery(std::filesystem::path wal_dir, RecoveryHandler& handler)
    : wal_dir_(std::move(wal_dir)), handler_(handler) {}

bool WalRecovery::is_data_record(WalRecordType type) {
    switch (type) {
    case WalRecordType::INSERT:
    case WalRecordType::UPDATE:
    case WalRecordType::DELETE:
    case WalRecordType::PAGE_SPLIT:
    case WalRecordType::CREATE_TABLE:
    case WalRecordType::DROP_TABLE:
        return true;
    case WalRecordType::BEGIN:
    case WalRecordType::COMMIT:
    case WalRecordType::ABORT:
    case WalRecordType::CHECKPOINT:
    case WalRecordType::PROMOTE:
        return false;
    }
    return false; // Unreachable — silences compiler warning.
}

Result<RecoveryStats> WalRecovery::recover() {
    RecoveryStats stats;

    // ---- Single-pass analysis + collection ----------------------------------
    // Stream through the WAL once.  When a CHECKPOINT record is encountered
    // we discard previously buffered records and restart collection from the
    // new checkpoint. This means:
    //   - We never read the WAL more than once (halves I/O).
    //   - We only buffer data records from the *last* checkpoint onward,
    //     keeping memory proportional to the post-checkpoint window.
    //   - Transaction state is rebuilt from the last checkpoint.

    WalReader reader(wal_dir_);
    auto open_result = reader.open();
    if (!open_result) {
        return tl::unexpected(open_result.error());
    }

    lsn_t last_checkpoint_lsn = 0;

    // Buffered data records from the last checkpoint onward (only data types).
    std::vector<WalRecord> post_checkpoint_records;

    // Transaction state (rebuilt from the last checkpoint).
    std::set<txn_id_t> committed_txns;
    std::set<txn_id_t> aborted_txns;
    std::set<txn_id_t> active_txns;

    while (true) {
        auto record = reader.next();
        if (!record) {
            if (record.error().code == StatusCode::NOT_FOUND) {
                break; // End of WAL.
            }
            return tl::unexpected(record.error());
        }
        ++stats.records_scanned;

        if (record->lsn > stats.max_lsn) {
            stats.max_lsn = record->lsn;
        }

        // --- Handle CHECKPOINT: reset collection state -----------------------
        if (record->type == WalRecordType::CHECKPOINT) {
            last_checkpoint_lsn = record->lsn;

            // Discard everything buffered before this checkpoint.
            post_checkpoint_records.clear();
            committed_txns.clear();
            aborted_txns.clear();
            active_txns.clear();

            // Seed active_txns with the transactions that were in-flight at
            // the time of the checkpoint (decoded from its data payload).
            if (!record->data.empty()) {
                auto active = deserialize_checkpoint_data(record->data);
                if (active) {
                    for (auto txn_id : *active) {
                        active_txns.insert(txn_id);
                    }
                } else {
                    SIXSEVEN_LOG_WARN("WAL recovery: failed to decode checkpoint data: {}",
                                      active.error().message);
                }
            }
            continue; // Don't buffer the checkpoint record itself.
        }

        // --- Track transaction state -----------------------------------------
        // Frozen records (GDB-714) represent autocommit operations: they are
        // committed by definition and never tracked as in-flight.
        if (record->txn_id != 0 && record->txn_id != frozen_txn_id) {
            switch (record->type) {
            case WalRecordType::BEGIN:
                active_txns.insert(record->txn_id);
                break;
            case WalRecordType::COMMIT:
                active_txns.erase(record->txn_id);
                committed_txns.insert(record->txn_id);
                break;
            case WalRecordType::ABORT:
                active_txns.erase(record->txn_id);
                aborted_txns.insert(record->txn_id);
                break;
            default:
                // Data records — if we haven't seen a BEGIN for this txn,
                // treat it as implicitly started (may happen if checkpoint
                // was after the BEGIN).
                if (committed_txns.find(record->txn_id) == committed_txns.end() &&
                    aborted_txns.find(record->txn_id) == aborted_txns.end()) {
                    active_txns.insert(record->txn_id);
                }
                break;
            }
        }

        // --- Buffer only data records (BEGIN/COMMIT/ABORT are not needed) ----
        // Note: invalid_txn_id (0) is the invalid sentinel; frozen_txn_id (~0)
        // is the separate autocommit marker (handled in redo/undo phases).
        // Legitimate DML never writes a data record with txn_id=0, so treat
        // any such record as malformed: emit a WARN and skip it rather than
        // silently dropping it with no observable signal.
        if (is_data_record(record->type)) {
            if (record->txn_id == invalid_txn_id) {
                SIXSEVEN_LOG_WARN(
                    "WAL recovery: data record with invalid txn_id (sentinel 0) "
                    "at lsn={} table_id={} -- record is malformed and will be skipped",
                    record->lsn,
                    record->table_id);
            } else {
                post_checkpoint_records.push_back(std::move(*record));
            }
        }
    }

    auto close_result = reader.close();
    if (!close_result) {
        return tl::unexpected(close_result.error());
    }

    stats.checkpoint_lsn = last_checkpoint_lsn;

    // In-progress transactions at crash time are treated as aborted.
    for (auto txn_id : active_txns) {
        aborted_txns.insert(txn_id);
    }

    stats.committed_txns = committed_txns.size();
    stats.aborted_txns = aborted_txns.size();
    stats.committed_txn_ids = committed_txns;
    stats.aborted_txn_ids = aborted_txns;

    SIXSEVEN_LOG_INFO("WAL recovery analysis: {} records scanned, checkpoint_lsn={}, max_lsn={}, "
                      "{} committed, {} aborted/in-progress",
                      stats.records_scanned,
                      last_checkpoint_lsn,
                      stats.max_lsn,
                      stats.committed_txns,
                      stats.aborted_txns);

    // ---- Phase 2: Redo ------------------------------------------------------
    // Replay data records of committed transactions in forward order.

    for (const auto& rec : post_checkpoint_records) {
        // Frozen records (GDB-714) are autocommit operations: always redone.
        if (rec.txn_id != frozen_txn_id &&
            committed_txns.find(rec.txn_id) == committed_txns.end()) {
            continue; // Not a committed transaction.
        }

        auto result = handler_.redo(rec);
        if (!result) {
            return tl::unexpected(result.error());
        }
        ++stats.records_redone;
    }

    SIXSEVEN_LOG_INFO("WAL recovery redo: {} records replayed", stats.records_redone);

    // ---- Phase 3: Undo ------------------------------------------------------
    // Reverse data records of aborted/in-progress transactions in reverse
    // (LSN) order.

    for (auto it = post_checkpoint_records.rbegin(); it != post_checkpoint_records.rend(); ++it) {
        if (aborted_txns.find(it->txn_id) == aborted_txns.end()) {
            continue; // Not an aborted transaction.
        }

        auto result = handler_.undo(*it);
        if (!result) {
            return tl::unexpected(result.error());
        }
        ++stats.records_undone;
    }

    SIXSEVEN_LOG_INFO("WAL recovery undo: {} records reversed", stats.records_undone);

    return ok(stats);
}

} // namespace sixseven
