#pragma once

#include "giodb/common/result.h"
#include "giodb/index/rid.h"
#include "giodb/txn/transaction.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

    /// Create a named savepoint within an active transaction.
    /// Captures the current write_set and read_set so they can be restored later.
    [[nodiscard]] Result<void> savepoint(txn_id_t txn_id, const std::string& name);

    /// Roll back to a named savepoint, restoring write_set and read_set.
    /// Destroys all savepoints established after the target savepoint.
    /// The target savepoint itself is retained (per SQL standard).
    [[nodiscard]] Result<void> rollback_to_savepoint(txn_id_t txn_id, const std::string& name);

    /// Release (destroy) a named savepoint and all savepoints established after it.
    [[nodiscard]] Result<void> release_savepoint(txn_id_t txn_id, const std::string& name);

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

    /// Garbage-collect completed transactions whose txn_id < xmin_horizon.
    /// Frees memory from committed/aborted transactions that are no longer
    /// needed for visibility checks by any active transaction.
    /// Should be called periodically (e.g., by the auto-vacuum worker).
    void gc_completed_transactions();

private:
    mutable std::mutex mu_;
    txn_id_t next_txn_id_ = 1;
    IsolationLevel default_isolation_level_ = IsolationLevel::READ_COMMITTED;

    /// All transactions (active and recently completed).
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> transactions_;

    /// Committed txn_ids that have been garbage-collected from transactions_.
    /// Needed so get_status() can still return COMMITTED for pruned transactions.
    std::unordered_set<txn_id_t> pruned_committed_;

    /// Take a snapshot while holding the lock.
    [[nodiscard]] Snapshot take_snapshot_locked() const;

    /// Compute xmin_horizon while already holding the lock.
    [[nodiscard]] txn_id_t xmin_horizon_locked() const;

    /// Garbage-collect completed transactions (caller holds mu_).
    void gc_completed_transactions_locked();

    /// Check for write-write conflicts under Snapshot Isolation / SSI.
    [[nodiscard]] Result<void> check_write_conflicts(const Transaction& txn) const;

    /// Check for rw-dependency cycles under Serializable (SSI).
    /// Detects "dangerous structures" where concurrent rw-dependencies form cycles.
    [[nodiscard]] Result<void> check_serialization_conflicts(const Transaction& txn) const;
};

} // namespace giodb
