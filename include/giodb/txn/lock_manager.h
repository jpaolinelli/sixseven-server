#pragma once

#include "giodb/catalog/schema.h"
#include "giodb/common/result.h"
#include "giodb/index/rid.h"
#include "giodb/storage/wal_record.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace giodb {

// -- Lock modes ---------------------------------------------------------------

/// Lock modes supported by the lock manager.
/// Ordered by strength: IS < IX < S < X.
enum class LockMode : uint8_t {
    IS, ///< Intent Shared: table-level intent for row-level S locks.
    IX, ///< Intent Exclusive: table-level intent for row-level X locks.
    S,  ///< Shared: multiple readers can hold concurrently.
    X,  ///< Exclusive: single writer, blocks all other locks.
};

/// Return a human-readable name for a LockMode.
inline const char* lock_mode_name(LockMode mode) {
    switch (mode) {
    case LockMode::IS:
        return "IS";
    case LockMode::IX:
        return "IX";
    case LockMode::S:
        return "S";
    case LockMode::X:
        return "X";
    }
    return "UNKNOWN";
}

/// Check whether two lock modes are compatible (can be held simultaneously).
///
/// Compatibility matrix:
///       IS   IX   S    X
///  IS   OK   OK   OK   BLOCK
///  IX   OK   OK   BLOCK BLOCK
///  S    OK   BLOCK OK   BLOCK
///  X    BLOCK BLOCK BLOCK BLOCK
inline bool locks_compatible(LockMode held, LockMode requested) {
    // clang-format off
    static constexpr bool matrix[4][4] = {
        /*          IS     IX     S      X    */
        /* IS */ {  true,  true,  true,  false },
        /* IX */ {  true,  true,  false, false },
        /* S  */ {  true,  false, true,  false },
        /* X  */ {  false, false, false, false },
    };
    // clang-format on
    return matrix[static_cast<uint8_t>(held)][static_cast<uint8_t>(requested)];
}

// -- Lock keys ----------------------------------------------------------------

/// A table-level lock key (table_id only).
struct TableLockKey {
    table_id_t table_id = 0;

    bool operator==(const TableLockKey&) const = default;
};

/// A row-level lock key (table_id + page_id + slot_id).
struct RowLockKey {
    table_id_t table_id = 0;
    PageId page_id = 0;
    SlotId slot_id = 0;

    bool operator==(const RowLockKey&) const = default;
};

/// A lock key identifies the resource being locked.
using LockKey = std::variant<TableLockKey, RowLockKey>;

} // namespace giodb

// -- Hash support for lock keys -----------------------------------------------

template <>
struct std::hash<giodb::TableLockKey> {
    std::size_t operator()(const giodb::TableLockKey& k) const noexcept {
        return std::hash<giodb::table_id_t>{}(k.table_id);
    }
};

template <>
struct std::hash<giodb::RowLockKey> {
    std::size_t operator()(const giodb::RowLockKey& k) const noexcept {
        auto h1 = std::hash<giodb::table_id_t>{}(k.table_id);
        auto h2 = std::hash<giodb::PageId>{}(k.page_id);
        auto h3 = std::hash<giodb::SlotId>{}(k.slot_id);
        // Combine hashes.
        h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
        h1 ^= h3 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
        return h1;
    }
};

template <>
struct std::hash<giodb::LockKey> {
    std::size_t operator()(const giodb::LockKey& k) const noexcept {
        return std::visit(
            [](const auto& key) -> std::size_t {
                return std::hash<std::decay_t<decltype(key)>>{}(key);
            },
            k);
    }
};

namespace giodb {

// -- Lock request / queue -----------------------------------------------------

/// A single lock request from a transaction.
struct LockRequest {
    txn_id_t txn_id = invalid_txn_id;
    LockMode mode = LockMode::S;
    bool granted = false;
    bool aborted = false; ///< Set by deadlock detector to abort this request.
};

/// A lock queue for a single resource.
/// Contains granted locks and waiting requests in FIFO order.
struct LockQueue {
    std::list<LockRequest> requests;
    std::condition_variable cv; ///< Signaled when a lock is released.
};

// -- Lock manager -------------------------------------------------------------

/// Manages row-level and table-level locks with deadlock detection.
///
/// Thread-safe: all public methods are protected by a mutex.
///
/// Usage:
/// ```
///   LockManager lock_mgr;
///   // Acquire intent lock on table, then row lock.
///   lock_mgr.lock_table(txn_id, table_id, LockMode::IX);
///   lock_mgr.lock_row(txn_id, table_id, page_id, slot_id, LockMode::X);
///   // On commit/abort:
///   lock_mgr.release_all(txn_id);
/// ```
class LockManager {
public:
    /// Default lock wait timeout.
    static constexpr auto default_lock_timeout = std::chrono::seconds(30);

    explicit LockManager(std::chrono::milliseconds lock_timeout = default_lock_timeout);

    /// Acquire a table-level lock.
    /// Blocks until the lock is granted, timeout expires, or deadlock is detected.
    [[nodiscard]] Result<void> lock_table(txn_id_t txn_id, table_id_t table_id, LockMode mode);

    /// Acquire a row-level lock.
    /// Automatically acquires the appropriate intent lock on the table first.
    /// Blocks until the lock is granted, timeout expires, or deadlock is detected.
    [[nodiscard]] Result<void>
    lock_row(txn_id_t txn_id, table_id_t table_id, PageId page_id, SlotId slot_id, LockMode mode);

    /// Release all locks held by a transaction (called on COMMIT or ROLLBACK).
    void release_all(txn_id_t txn_id);

    /// Set the lock wait timeout.
    void set_lock_timeout(std::chrono::milliseconds timeout);

    /// Get the lock wait timeout.
    [[nodiscard]] std::chrono::milliseconds lock_timeout() const;

private:
    mutable std::mutex mu_;
    std::chrono::milliseconds lock_timeout_;

    /// Map from lock key to its lock queue.
    std::unordered_map<LockKey, LockQueue> lock_table_;

    /// Acquire a lock on the given key. Blocks until granted, timeout, or deadlock.
    /// Caller must NOT hold mu_.
    [[nodiscard]] Result<void> acquire(txn_id_t txn_id, const LockKey& key, LockMode mode);

    /// Try to grant waiting requests in a queue after a release.
    /// Caller must hold mu_.
    void grant_waiting_locked(LockQueue& queue);

    /// Check if a lock request is compatible with all currently granted locks
    /// in the queue (excluding locks held by the same transaction).
    /// Caller must hold mu_.
    [[nodiscard]] bool
    is_compatible_locked(const LockQueue& queue, txn_id_t txn_id, LockMode mode) const;

    /// Detect deadlock involving the given transaction.
    /// Returns the txn_id of the youngest transaction in the cycle (victim),
    /// or invalid_txn_id if no deadlock is found.
    /// Caller must hold mu_.
    [[nodiscard]] txn_id_t detect_deadlock_locked(txn_id_t txn_id) const;

    /// Mark the victim's waiting requests as aborted and notify all queues.
    /// Caller must hold mu_.
    void abort_victim_locked(txn_id_t victim);

    /// Remove a non-granted request for the given transaction from a queue.
    /// Caller must hold mu_.
    void remove_waiting_request_locked(LockQueue& queue, txn_id_t txn_id);
};

} // namespace giodb
