#pragma once

#include "sixseven/server/wal_sender_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sixseven {

class ReplicationSlotManager;

/// Configuration for replication health monitoring.
struct HealthMonitorConfig {
    /// Replication lag threshold before a warning is logged (milliseconds).
    std::chrono::milliseconds lag_warning_threshold{10000};

    /// Disconnect threshold before an error is logged (milliseconds).
    std::chrono::milliseconds disconnect_warning_threshold{60000};
};

/// Observable health state produced by the most recent check_health() call.
struct HealthReport {
    /// Number of lag warnings emitted across all check_health() calls so far.
    uint64_t warning_count{0};

    /// Last observed lag (bytes) for each replica, keyed by slot name (or peer
    /// address when no slot is assigned).  Only replicas that have reported
    /// their applied_lsn are present; replicas with an unknown position are
    /// absent from the map.
    std::unordered_map<std::string, int64_t> last_lag_bytes;
};

/// Monitors replication health on the primary and logs warnings/errors.
///
/// Checks:
///   - Replication lag exceeds threshold -> log warning.
///   - Replica disconnected longer than threshold -> log error.
///   - WAL accumulation growing due to slow/inactive replica -> log warning.
///
/// Usage:
///   Call check_health() periodically (e.g., from a timer or keepalive loop).
class ReplicationHealthMonitor {
public:
    ReplicationHealthMonitor() = default;
    explicit ReplicationHealthMonitor(HealthMonitorConfig config);

    /// Update the configuration at runtime.
    void configure(HealthMonitorConfig config);

    /// Run all health checks.
    /// @param sender_mgr  WAL sender manager (for replica statuses).
    /// @param slot_mgr    Replication slot manager (for WAL accumulation).
    /// @param writer      WAL writer (for current LSN).
    void check_health(const WalSenderManager& sender_mgr,
                      const ReplicationSlotManager* slot_mgr,
                      const WalWriter& writer);

    /// Return a snapshot of the health state from the most recent check_health()
    /// call.  Thread-safe (takes the internal mutex).
    [[nodiscard]] HealthReport last_report() const;

private:
    mutable std::mutex mutex_;
    HealthMonitorConfig config_;

    /// Tracks when each replica was last seen active (for disconnect detection).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_seen_;

    /// Cumulative count of lag warnings emitted.
    uint64_t warning_count_{0};

    /// Lag in bytes observed per replica during the last check_health() call.
    std::unordered_map<std::string, int64_t> last_lag_bytes_;
};

} // namespace sixseven
