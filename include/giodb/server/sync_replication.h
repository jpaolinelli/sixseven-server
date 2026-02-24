#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/wal_record.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giodb {

/// Synchronous replication level.
///
/// Controls how much progress a replica must report before the primary
/// considers a COMMIT acknowledged.
enum class SyncLevel {
    OFF,          ///< Fully async — primary never waits (default).
    REMOTE_WRITE, ///< Wait until replicas receive WAL in memory.
    REMOTE_FLUSH, ///< Wait until replicas flush WAL to disk.
    REMOTE_APPLY, ///< Wait until replicas apply WAL to storage.
};

/// Parse a SyncLevel from a string.
/// Accepted values: "off", "remote_write", "remote_flush", "remote_apply".
[[nodiscard]] Result<SyncLevel> parse_sync_level(const std::string& s);

/// Convert a SyncLevel to its string representation.
[[nodiscard]] const char* sync_level_name(SyncLevel level);

/// Fallback behavior when synchronous replication timeout expires.
enum class SyncFallback {
    ERROR, ///< COMMIT fails with an error (default).
    WARN,  ///< COMMIT succeeds with a warning, falls back to async.
    BLOCK, ///< COMMIT blocks indefinitely until a replica acknowledges.
};

/// Parse a SyncFallback from a string.
/// Accepted values: "error", "warn", "block".
[[nodiscard]] Result<SyncFallback> parse_sync_fallback(const std::string& s);

/// Convert a SyncFallback to its string representation.
[[nodiscard]] const char* sync_fallback_name(SyncFallback fb);

/// Configuration for the SyncReplicationManager.
struct SyncReplicationConfig {
    SyncLevel level = SyncLevel::OFF;
    std::unordered_set<std::string> standby_names;
    uint32_t commit_count = 1;
    std::chrono::milliseconds timeout{30000};
    SyncFallback fallback = SyncFallback::ERROR;
};

/// Result of waiting for synchronous replication acknowledgement.
enum class SyncWaitResult {
    SYNCED,          ///< Required replicas acknowledged within time.
    TIMED_OUT_ERROR, ///< Timeout with fallback=error (commit should fail).
    TIMED_OUT_WARN,  ///< Timeout with fallback=warn (commit succeeds, warning).
    NOT_ENABLED,     ///< Sync replication is off.
};

/// Progress reported by a single replica.
struct ReplicaProgress {
    lsn_t received_lsn = invalid_lsn;
    lsn_t flushed_lsn = invalid_lsn;
    lsn_t applied_lsn = invalid_lsn;
};

/// Manages synchronous replication on the primary.
///
/// After a commit is flushed to the primary's WAL, the caller invokes
/// wait_for_sync() with the commit LSN. The method blocks until the
/// required number of replicas acknowledge the LSN at the configured
/// sync level, or the timeout fires.
///
/// WalSenders call report_replica_progress() whenever they receive a
/// StandbyStatus from their replica. This wakes any waiting commits.
///
/// Thread safety: all public methods are safe to call from any thread.
class SyncReplicationManager {
public:
    SyncReplicationManager() = default;
    explicit SyncReplicationManager(SyncReplicationConfig config);

    SyncReplicationManager(const SyncReplicationManager&) = delete;
    SyncReplicationManager& operator=(const SyncReplicationManager&) = delete;

    /// Update the configuration at runtime (e.g., from SET commands).
    void configure(SyncReplicationConfig config);

    /// Return true if synchronous mode is not OFF.
    [[nodiscard]] bool is_sync_enabled() const;

    /// Return the current sync level.
    [[nodiscard]] SyncLevel sync_level() const;

    /// Block until N replicas acknowledge commit_lsn at the required level.
    /// Returns SYNCED on success, or a timeout result based on fallback config.
    /// Returns NOT_ENABLED if sync is off (no blocking).
    [[nodiscard]] SyncWaitResult wait_for_sync(lsn_t commit_lsn);

    /// Report progress from a replica identified by slot_name.
    /// Called by WalSender when it receives StandbyStatus.
    void report_replica_progress(const std::string& slot_name,
                                 lsn_t received_lsn,
                                 lsn_t flushed_lsn,
                                 lsn_t applied_lsn);

    /// Remove a replica from tracking (e.g., when it disconnects).
    void remove_replica(const std::string& slot_name);

    /// Return the current configuration (snapshot).
    [[nodiscard]] SyncReplicationConfig current_config() const;

    /// Return progress for all tracked replicas.
    [[nodiscard]] std::unordered_map<std::string, ReplicaProgress> replica_progress() const;

    /// Count how many sync standbys have acknowledged the given LSN
    /// at the current sync level. Useful for monitoring.
    [[nodiscard]] uint32_t count_synced_replicas(lsn_t lsn) const;

private:
    /// Check if the required number of replicas have acknowledged lsn.
    [[nodiscard]] bool is_lsn_synced(lsn_t lsn) const;

    /// Get the relevant LSN from a replica's progress based on sync level.
    [[nodiscard]] lsn_t relevant_lsn(const ReplicaProgress& progress) const;

    mutable std::mutex mutex_;
    std::condition_variable sync_cv_;
    SyncReplicationConfig config_;
    std::unordered_map<std::string, ReplicaProgress> replicas_;
};

} // namespace giodb
