#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/wal_record.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace giodb {

/// A replication slot tracks a replica's progress and prevents WAL cleanup
/// before the replica has consumed the data.
struct ReplicationSlot {
    /// Unique slot name (e.g., "replica_1").
    std::string slot_name;

    /// Slot type — "physical" only for v1.
    std::string slot_type = "physical";

    /// Whether a replica is currently connected using this slot.
    bool active = false;

    /// Oldest LSN the slot needs — WAL before this can be cleaned up.
    lsn_t restart_lsn = invalid_lsn;

    /// Last LSN confirmed as flushed by the replica.
    lsn_t confirmed_flush_lsn = invalid_lsn;

    /// Creation timestamp (microseconds since epoch).
    uint64_t created_at_us = 0;
};

/// WAL accumulation warning threshold (bytes). When WAL retained by an
/// inactive slot exceeds this size, a warning is logged.
inline constexpr size_t default_wal_retention_warning_bytes = 256 * 1024 * 1024; // 256 MB

/// Manages replication slots on the primary.
///
/// Thread-safe: all public methods are protected by a mutex.
///
/// Slots are persisted to a JSON file in the data directory so they
/// survive primary restarts.
class ReplicationSlotManager {
public:
    /// Construct a slot manager.
    /// @param data_dir  Data directory for slot persistence.
    /// @param wal_retention_warning_bytes  Threshold for WAL accumulation warning.
    explicit ReplicationSlotManager(
        std::filesystem::path data_dir,
        size_t wal_retention_warning_bytes = default_wal_retention_warning_bytes);

    /// Create a new replication slot.
    /// Returns ALREADY_EXISTS if a slot with this name exists.
    [[nodiscard]] Result<ReplicationSlot> create_slot(const std::string& name);

    /// Drop a replication slot by name.
    /// Returns NOT_FOUND if no slot with this name exists.
    /// Returns INVALID_ARGUMENT if the slot is currently active.
    [[nodiscard]] Result<void> drop_slot(const std::string& name);

    /// Get a slot by name.
    /// Returns NOT_FOUND if no slot with this name exists.
    [[nodiscard]] Result<ReplicationSlot> get_slot(const std::string& name) const;

    /// List all replication slots.
    [[nodiscard]] std::vector<ReplicationSlot> list_slots() const;

    /// Mark a slot as active (replica connected).
    /// Sets active = true and updates restart_lsn if provided.
    /// Returns NOT_FOUND if no slot with this name exists.
    [[nodiscard]] Result<void> activate_slot(const std::string& name, lsn_t restart_lsn);

    /// Mark a slot as inactive (replica disconnected).
    /// Returns NOT_FOUND if no slot with this name exists.
    [[nodiscard]] Result<void> deactivate_slot(const std::string& name);

    /// Update the confirmed flush LSN for a slot.
    /// Called by WalSender when a replica confirms progress.
    /// Also advances restart_lsn to match confirmed_flush_lsn.
    void update_confirmed_lsn(const std::string& slot_name, lsn_t lsn);

    /// Return the minimum restart_lsn across all slots.
    /// WAL segments before this LSN can be safely cleaned up.
    /// Returns invalid_lsn (0) if no slots exist.
    [[nodiscard]] lsn_t get_min_restart_lsn() const;

    /// Persist all slots to disk.
    [[nodiscard]] Result<void> save() const;

    /// Load slots from disk. Marks all slots as inactive on load
    /// (since no replicas are connected after a restart).
    [[nodiscard]] Result<void> load();

    /// Check WAL accumulation for inactive slots and log warnings.
    /// @param current_lsn  The primary's current WAL LSN.
    void check_wal_accumulation(lsn_t current_lsn) const;

private:
    /// Persist slots without locking (caller must hold mutex_).
    [[nodiscard]] Result<void> save_impl() const;

    std::filesystem::path persistence_path() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ReplicationSlot> slots_;
    std::filesystem::path data_dir_;
    size_t wal_retention_warning_bytes_;
};

} // namespace giodb
