#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/rid.h"
#include "sixseven/txn/lock_manager.h"
#include "sixseven/txn/transaction.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

// Forward declaration -- avoids a hard dependency from txn/ on storage/ in
// the header; only a pointer is stored (GDB-1247).
class WalWriter;

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

    /// Get the status of a transaction. This is the SINGLE authoritative
    /// resolver for transaction status (GDB-1242) -- every layer that needs
    /// to interpret an xid (table_heap's visibility check, mvcc.cpp's
    /// snapshot-based is_visible/is_dead, vacuum) MUST delegate to this
    /// rather than re-deriving status from get_transaction()/pruned sets.
    ///
    /// Convention (documented here as the one shared source of truth):
    ///   - xid == frozen_txn_id                    -> COMMITTED (GDB-714).
    ///   - xid live in transactions_                -> its current status.
    ///   - xid in pruned_committed_ (GC'd, was COMMITTED) -> COMMITTED.
    ///   - xid in pruned_aborted_   (GC'd, was ABORTED)   -> ABORTED.
    ///   - genuinely unknown xid (never registered; e.g. a row persisted by
    ///     a prior process with no commit log) -> COMMITTED. This preserves
    ///     the no-clog cross-restart compromise: existing data stays
    ///     readable. It does NOT apply to GC'd aborted transactions, which
    ///     are definitively ABORTED via pruned_aborted_ above.
    [[nodiscard]] TransactionStatus get_status(txn_id_t txn_id) const;

    /// True iff this xid is definitely known to the manager: either a live
    /// transaction, or a GC'd transaction remembered in pruned_committed_ /
    /// pruned_aborted_. False means the xid was never registered (e.g. a row
    /// persisted by a prior server process). Use this -- not get_status's
    /// return value -- to ask "is this xid unknown/unregistered"; get_status
    /// deliberately returns COMMITTED for unregistered ids (see its doc
    /// comment), so it cannot be used as an unknown-xid proxy.
    [[nodiscard]] bool is_registered(txn_id_t txn_id) const;

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

    /// Access the shared lock manager (GDB-930).
    /// DML operators acquire row/table locks through this manager.
    [[nodiscard]] LockManager& lock_manager() { return lock_mgr_; }

    /// Wire in a durable store for persisting the txn-id high-water mark
    /// (GDB-1247). Optional: if never called (in-memory/test configs), the
    /// manager falls back to the pre-existing behavior -- start at 1, never
    /// persist a watermark -- WITHOUT crashing. That fallback is not
    /// crash-safe: a process using it can reuse ids after a restart. Must be
    /// called before any begin() whose durability matters; safe to call at
    /// any time otherwise (affects only subsequently reserved batches).
    void set_wal_writer(WalWriter* writer);

    /// Initialize next_txn_id_ from a durably recovered watermark (GDB-1247).
    /// Callers (main.cpp, after WAL recovery) should pass
    /// RecoveryStats::max_txn_id_seen + 1, or skip the call entirely when no
    /// recovery ran / no watermark was ever observed (next_txn_id_ keeps its
    /// default of 1). Only raises the counter: never lowers it below its
    /// current value, so calling it more than once, or after begin() has
    /// already been used, is safe (though callers should call it once at
    /// startup before any real transaction begins).
    void init_next_txn_id(txn_id_t min_next_id);

    /// Batch size for reserving a block of txn ids per persisted watermark
    /// write (GDB-1247), mirroring PostgreSQL's XID allocation: persisting
    /// on every begin() would add a WAL round-trip to every transaction
    /// start, so instead we persist the CEILING of a batch of this many ids
    /// up front and hand out ids from that batch without further I/O until
    /// it is exhausted. A crash mid-batch merely skips the unused tail on
    /// restart -- acceptable, since ids are cheap and unbounded (64-bit).
    static constexpr txn_id_t txn_id_batch_size = 1024;

private:
    mutable std::mutex mu_;
    txn_id_t next_txn_id_ = 1;
    IsolationLevel default_isolation_level_ = IsolationLevel::READ_COMMITTED;

    /// Durable store for the txn-id watermark (GDB-1247). Null when no
    /// durable store is configured (in-memory/test configs); guarded by
    /// null checks everywhere it is used -- see set_wal_writer() doc.
    WalWriter* wal_writer_ = nullptr;

    /// The highest txn id that may be handed out without persisting a new
    /// watermark first (GDB-1247): next_txn_id_ may advance up to and
    /// including this value using only in-memory increments. When
    /// next_txn_id_ would exceed it, a new ceiling (reserved_ceiling_ +
    /// txn_id_batch_size) is persisted BEFORE any id past the old ceiling is
    /// handed out. 0 means no batch has been reserved yet (forces a persist
    /// on the very first begin() when wal_writer_ is configured).
    txn_id_t reserved_ceiling_ = 0;

    /// Reserve (persist) a new batch ceiling so next_txn_id_ can advance past
    /// reserved_ceiling_. Caller holds mu_. No-op (returns ok()) if
    /// wal_writer_ is null -- the in-memory/test fallback. On success,
    /// reserved_ceiling_ is updated to the new ceiling BEFORE returning, so
    /// the caller may then safely hand out any id up to and including it.
    [[nodiscard]] Result<void> ensure_batch_reserved_locked();

    /// Shared lock manager for row/table locking (GDB-930).
    LockManager lock_mgr_;

    /// All transactions (active and recently completed).
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> transactions_;

    /// Committed txn_ids that have been garbage-collected from transactions_.
    /// Needed so get_status() can still return COMMITTED for pruned transactions.
    std::unordered_set<txn_id_t> pruned_committed_;

    /// Aborted txn_ids that have been garbage-collected from transactions_
    /// (GDB-1242). Needed so get_status() can still return ABORTED for GC'd
    /// aborted transactions -- without this, their tuples would resurrect
    /// (deletes reverted, inserts made visible) the moment GC/auto-vacuum
    /// runs, because an untracked xid falls back to the unknown-xid
    /// convention (COMMITTED). Same memory-growth characteristics as
    /// pruned_committed_ above: both are unbounded-until-restart sets keyed
    /// by txn_id; neither currently has an eviction policy. This mirrors the
    /// existing (pre-GDB-1242) behavior of pruned_committed_ rather than
    /// introducing a new bound -- follow-up if unconstrained growth becomes
    /// an issue in practice.
    std::unordered_set<txn_id_t> pruned_aborted_;

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

} // namespace sixseven
