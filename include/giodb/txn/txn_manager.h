#pragma once

#include "giodb/common/result.h"
#include "giodb/index/rid.h"
#include "giodb/txn/transaction.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace giodb {

/// Manages transaction lifecycles, snapshots, and status tracking.
///
/// Thread-safe: all public methods are protected by a mutex.
///
/// Usage:
/// ```
///   TransactionManager txn_mgr;
///   auto* txn = txn_mgr.begin().value();
///   // ... perform operations ...
///   txn_mgr.commit(txn->txn_id);
/// ```
class TransactionManager {
public:
    TransactionManager() = default;

    /// Begin a new transaction with the given isolation level.
    /// If no level is specified, uses the default isolation level.
    /// Allocates a new txn_id and takes a snapshot.
    [[nodiscard]] Result<Transaction*> begin(IsolationLevel level = IsolationLevel::READ_COMMITTED);

    /// Commit a transaction. Updates its status to COMMITTED.
    /// Under Snapshot Isolation, checks for write-write conflicts first.
    /// Under Serializable, also checks for rw-dependency cycles (SSI).
    [[nodiscard]] Result<void> commit(txn_id_t txn_id);

    /// Abort a transaction. Updates its status to ABORTED.
    [[nodiscard]] Result<void> abort(txn_id_t txn_id);

    /// Take a fresh snapshot of the current transaction state.
    /// Used by Read Committed for per-statement snapshots.
    [[nodiscard]] Snapshot take_snapshot() const;

    /// Refresh the snapshot for a Read Committed transaction.
    /// For SI/SSI transactions, this is a no-op (they keep their BEGIN snapshot).
    /// Returns the snapshot that should be used for the current statement.
    [[nodiscard]] Snapshot get_statement_snapshot(txn_id_t txn_id);

    /// Get the status of a transaction.
    [[nodiscard]] TransactionStatus get_status(txn_id_t txn_id) const;

    /// Get a transaction by ID (returns nullptr if not found).
    [[nodiscard]] Transaction* get_transaction(txn_id_t txn_id) const;

    /// Get the next transaction ID that will be assigned (useful for testing).
    [[nodiscard]] txn_id_t next_txn_id() const;

    /// Record that a transaction wrote to a specific RID (for conflict detection).
    void record_write(txn_id_t txn_id, RID rid);

    /// Record that a transaction read a specific RID (for SSI tracking).
    void record_read(txn_id_t txn_id, RID rid);

    /// Get the xmin horizon: the smallest xmin across all active snapshots.
    /// Tuples with xmax committed below this value are safe to vacuum.
    [[nodiscard]] txn_id_t xmin_horizon() const;

    /// Set the default isolation level for new transactions started without
    /// an explicit level. Maps to SET default_transaction_isolation.
    void set_default_isolation_level(IsolationLevel level);

    /// Get the current default isolation level.
    [[nodiscard]] IsolationLevel default_isolation_level() const;

private:
    mutable std::mutex mu_;
    txn_id_t next_txn_id_ = 1;
    IsolationLevel default_isolation_level_ = IsolationLevel::READ_COMMITTED;

    /// All transactions (active and recently completed).
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> transactions_;

    /// Take a snapshot while holding the lock.
    [[nodiscard]] Snapshot take_snapshot_locked() const;

    /// Check for write-write conflicts under Snapshot Isolation / SSI.
    [[nodiscard]] Result<void> check_write_conflicts(const Transaction& txn) const;

    /// Check for rw-dependency cycles under Serializable (SSI).
    /// Detects "dangerous structures" where concurrent rw-dependencies form cycles.
    [[nodiscard]] Result<void> check_serialization_conflicts(const Transaction& txn) const;
};

} // namespace giodb
