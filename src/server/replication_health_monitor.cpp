#include "sixseven/server/replication_health_monitor.h"

#include "sixseven/common/logging.h"
#include "sixseven/server/replication_slot.h"

namespace sixseven {

ReplicationHealthMonitor::ReplicationHealthMonitor(HealthMonitorConfig config) : config_(config) {}

void ReplicationHealthMonitor::configure(HealthMonitorConfig config) {
    std::lock_guard lock(mutex_);
    config_ = config;
}

void ReplicationHealthMonitor::check_health(const WalSenderManager& sender_mgr,
                                            const ReplicationSlotManager* slot_mgr,
                                            const WalWriter& writer) {
    std::lock_guard lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto statuses = sender_mgr.get_sender_statuses();
    lsn_t current_lsn = writer.current_lsn();

    // Reset per-call lag map; repopulate below.
    last_lag_bytes_.clear();

    // Track which replicas are currently active.
    std::unordered_map<std::string, bool> active_replicas;

    for (const auto& s : statuses) {
        std::string id = s.slot_name.empty() ? s.peer : s.slot_name;
        active_replicas[id] = true;
        last_seen_[id] = now;

        // Check replication lag (bytes behind as proxy for lag).
        if (s.applied_lsn != invalid_lsn && current_lsn > 0) {
            auto lag_bytes =
                static_cast<int64_t>(current_lsn) - static_cast<int64_t>(s.applied_lsn);
            last_lag_bytes_[id] = lag_bytes;
            if (lag_bytes > config_.lag_warning_threshold.count()) {
                ++warning_count_;
                SIXSEVEN_LOG_WARN(
                    "replication lag for {} is {} bytes (threshold: {} ms equivalent)",
                    id,
                    lag_bytes,
                    config_.lag_warning_threshold.count());
            }
        }
    }

    // Check for replicas that were previously seen but are now disconnected.
    for (auto it = last_seen_.begin(); it != last_seen_.end();) {
        if (active_replicas.count(it->first) == 0) {
            auto disconnected_duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
            if (disconnected_duration > config_.disconnect_warning_threshold) {
                SIXSEVEN_LOG_ERROR("replica {} disconnected for {} ms (threshold: {} ms)",
                                   it->first,
                                   disconnected_duration.count(),
                                   config_.disconnect_warning_threshold.count());
                // Remove from tracking to avoid repeated errors.
                it = last_seen_.erase(it);
                continue;
            }
        }
        ++it;
    }

    // Check WAL accumulation from slot manager.
    if (slot_mgr != nullptr) {
        slot_mgr->check_wal_accumulation(current_lsn);
    }
}

HealthReport ReplicationHealthMonitor::last_report() const {
    std::lock_guard lock(mutex_);
    HealthReport report;
    report.warning_count = warning_count_;
    report.last_lag_bytes = last_lag_bytes_;
    return report;
}

} // namespace sixseven