#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/rid.h"
#include "sixseven/storage/wal_record.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace sixseven {

// -- Transaction status -------------------------------------------------------

/// Status of a transaction in the commit log.
enum class TransactionStatus : uint8_t {
    ACTIVE,
    COMMITTED,
    ABORTED,
};

/// Return a human-readable name for a TransactionStatus.
inline const char* transaction_status_name(TransactionStatus status) {
    switch (status) {
    case TransactionStatus::ACTIVE:
        return "ACTIVE";
    case TransactionStatus::COMMITTED:
        return "COMMITTED";
    case TransactionStatus::ABORTED:
        return "ABORTED";
    }
    return "UNKNOWN";
}

// -- Isolation level ----------------------------------------------------------

/// SQL isolation levels supported by the MVCC engine.
enum class IsolationLevel : uint8_t {
    READ_COMMITTED,     ///< New snapshot per statement (default).
    SNAPSHOT_ISOLATION, ///< Snapshot at BEGIN, write-write conflict detection.
    SERIALIZABLE,       ///< SSI: snapshot + rw-dependency tracking.
};

/// Return a human-readable name for an IsolationLevel.
inline const char* isolation_level_name(IsolationLevel level) {
    switch (level) {
    case IsolationLevel::READ_COMMITTED:
        return "READ COMMITTED";
    case IsolationLevel::SNAPSHOT_ISOLATION:
        return "REPEATABLE READ";
    case IsolationLevel::SERIALIZABLE:
        return "SERIALIZABLE";
    }
    return "UNKNOWN";
}

// -- Snapshot -----------------------------------------------------------------

/// A snapshot of the database state at a point in time.
/// Used by MVCC to determine which tuple versions are visible.
struct Snapshot {
    txn_id_t xmin = 0;                    ///< Smallest active txn ID at snapshot time.
    txn_id_t xmax = 0;                    ///< Next txn ID to be assigned.
    std::vector<txn_id_t> active_txn_ids; ///< In-progress txn IDs at snapshot time.

    /// Check whether a transaction was in-progress when this snapshot was taken.
    [[nodiscard]] bool is_active(txn_id_t txn_id) const {
        return std::find(active_txn_ids.begin(), active_txn_ids.end(), txn_id) !=
               active_txn_ids.end();
    }
};

// -- Savepoint ----------------------------------------------------------------

/// A savepoint within a transaction.
/// Captures the state of write_set and read_set at the time of creation,
/// allowing partial rollback within a transaction (SAVEPOINT / ROLLBACK TO).
struct Savepoint {
    std::string name;              ///< Savepoint name.
    std::set<RID> saved_write_set; ///< Write set snapshot at savepoint creation.
    std::set<RID> saved_read_set;  ///< Read set snapshot at savepoint creation.
};

// -- Transaction --------------------------------------------------------------

/// A transaction in the MVCC system.
struct Transaction {
    txn_id_t txn_id = invalid_txn_id;
    IsolationLevel isolation_level = IsolationLevel::READ_COMMITTED;
    TransactionStatus status = TransactionStatus::ACTIVE;
    Snapshot snapshot; ///< Transaction-level snapshot (for SI/SSI).

    /// Write set: RIDs modified by this transaction (for conflict detection).
    std::set<RID> write_set;

    /// Read set: RIDs read by this transaction (for SSI rw-dependency tracking).
    std::set<RID> read_set;

    /// Savepoint stack (newest at back). Supports SAVEPOINT / ROLLBACK TO / RELEASE.
    std::vector<Savepoint> savepoints;
};

} // namespace sixseven
