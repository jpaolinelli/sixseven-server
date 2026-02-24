#include "giodb/server/sync_replication.h"

#include "giodb/common/logging.h"

#include <algorithm>

namespace giodb {

// -- Free functions -----------------------------------------------------------

Result<SyncLevel> parse_sync_level(const std::string& s) {
    if (s == "off") {
        return ok(SyncLevel::OFF);
    }
    if (s == "remote_write") {
        return ok(SyncLevel::REMOTE_WRITE);
    }
    if (s == "remote_flush") {
        return ok(SyncLevel::REMOTE_FLUSH);
    }
    if (s == "remote_apply") {
        return ok(SyncLevel::REMOTE_APPLY);
    }
    return make_error(StatusCode::INVALID_ARGUMENT,
                      "unknown sync level: '" + s +
                          "' (expected off/remote_write/remote_flush/remote_apply)");
}

const char* sync_level_name(SyncLevel level) {
    switch (level) {
    case SyncLevel::OFF:
        return "off";
    case SyncLevel::REMOTE_WRITE:
        return "remote_write";
    case SyncLevel::REMOTE_FLUSH:
        return "remote_flush";
    case SyncLevel::REMOTE_APPLY:
        return "remote_apply";
    }
    return "unknown";
}

Result<SyncFallback> parse_sync_fallback(const std::string& s) {
    if (s == "error") {
        return ok(SyncFallback::ERROR);
    }
    if (s == "warn") {
        return ok(SyncFallback::WARN);
    }
    if (s == "block") {
        return ok(SyncFallback::BLOCK);
    }
    return make_error(StatusCode::INVALID_ARGUMENT,
                      "unknown sync fallback: '" + s + "' (expected error/warn/block)");
}

const char* sync_fallback_name(SyncFallback fb) {
    switch (fb) {
    case SyncFallback::ERROR:
        return "error";
    case SyncFallback::WARN:
        return "warn";
    case SyncFallback::BLOCK:
        return "block";
    }
    return "unknown";
}

// -- SyncReplicationManager ---------------------------------------------------

SyncReplicationManager::SyncReplicationManager(SyncReplicationConfig config)
    : config_(std::move(config)) {}

void SyncReplicationManager::configure(SyncReplicationConfig config) {
    std::lock_guard lock(mutex_);
    config_ = std::move(config);
    // Wake all waiters so they re-evaluate with new config.
    sync_cv_.notify_all();

    GIODB_LOG_INFO(
        "sync replication configured: level={}, commit_count={}, timeout={}ms, fallback={}",
        sync_level_name(config_.level),
        config_.commit_count,
        config_.timeout.count(),
        sync_fallback_name(config_.fallback));
}

bool SyncReplicationManager::is_sync_enabled() const {
    std::lock_guard lock(mutex_);
    return config_.level != SyncLevel::OFF;
}

SyncLevel SyncReplicationManager::sync_level() const {
    std::lock_guard lock(mutex_);
    return config_.level;
}

SyncWaitResult SyncReplicationManager::wait_for_sync(lsn_t commit_lsn) {
    std::unique_lock lock(mutex_);

    if (config_.level == SyncLevel::OFF) {
        return SyncWaitResult::NOT_ENABLED;
    }

    if (config_.commit_count == 0) {
        return SyncWaitResult::SYNCED;
    }

    // Fast path: already synced.
    if (is_lsn_synced(commit_lsn)) {
        return SyncWaitResult::SYNCED;
    }

    if (config_.fallback == SyncFallback::BLOCK) {
        // Block indefinitely.
        sync_cv_.wait(lock,
                      [&] { return config_.level == SyncLevel::OFF || is_lsn_synced(commit_lsn); });

        if (config_.level == SyncLevel::OFF) {
            return SyncWaitResult::NOT_ENABLED;
        }

        return SyncWaitResult::SYNCED;
    }

    // Wait with timeout.
    auto deadline = config_.timeout;
    bool satisfied = sync_cv_.wait_for(lock, deadline, [&] {
        return config_.level == SyncLevel::OFF || is_lsn_synced(commit_lsn);
    });

    if (config_.level == SyncLevel::OFF) {
        return SyncWaitResult::NOT_ENABLED;
    }

    if (satisfied) {
        return SyncWaitResult::SYNCED;
    }

    // Timeout fired.
    if (config_.fallback == SyncFallback::WARN) {
        GIODB_LOG_WARN("sync replication timeout for LSN {} after {}ms — falling back to async",
                       commit_lsn,
                       config_.timeout.count());
        return SyncWaitResult::TIMED_OUT_WARN;
    }

    GIODB_LOG_ERROR("sync replication timeout for LSN {} after {}ms — commit failed",
                    commit_lsn,
                    config_.timeout.count());
    return SyncWaitResult::TIMED_OUT_ERROR;
}

void SyncReplicationManager::report_replica_progress(const std::string& slot_name,
                                                     lsn_t received_lsn,
                                                     lsn_t flushed_lsn,
                                                     lsn_t applied_lsn) {
    {
        std::lock_guard lock(mutex_);

        // Only track replicas that are in the configured standby set,
        // or track all if the set is empty (wildcard).
        if (!config_.standby_names.empty() &&
            config_.standby_names.find(slot_name) == config_.standby_names.end()) {
            return;
        }

        auto& progress = replicas_[slot_name];
        progress.received_lsn = received_lsn;
        progress.flushed_lsn = flushed_lsn;
        progress.applied_lsn = applied_lsn;
    }

    // Wake waiters — one of them might now be satisfied.
    sync_cv_.notify_all();
}

void SyncReplicationManager::remove_replica(const std::string& slot_name) {
    std::lock_guard lock(mutex_);
    replicas_.erase(slot_name);
}

SyncReplicationConfig SyncReplicationManager::current_config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

std::unordered_map<std::string, ReplicaProgress> SyncReplicationManager::replica_progress() const {
    std::lock_guard lock(mutex_);
    return replicas_;
}

uint32_t SyncReplicationManager::count_synced_replicas(lsn_t lsn) const {
    std::lock_guard lock(mutex_);
    uint32_t count = 0;
    for (const auto& [name, progress] : replicas_) {
        if (!config_.standby_names.empty() &&
            config_.standby_names.find(name) == config_.standby_names.end()) {
            continue;
        }
        if (relevant_lsn(progress) >= lsn && relevant_lsn(progress) != invalid_lsn) {
            ++count;
        }
    }
    return count;
}

// -- Private ------------------------------------------------------------------

bool SyncReplicationManager::is_lsn_synced(lsn_t lsn) const {
    // Caller must hold mutex_.
    uint32_t count = 0;
    for (const auto& [name, progress] : replicas_) {
        if (!config_.standby_names.empty() &&
            config_.standby_names.find(name) == config_.standby_names.end()) {
            continue;
        }
        lsn_t rlsn = relevant_lsn(progress);
        if (rlsn >= lsn && rlsn != invalid_lsn) {
            ++count;
            if (count >= config_.commit_count) {
                return true;
            }
        }
    }
    return false;
}

lsn_t SyncReplicationManager::relevant_lsn(const ReplicaProgress& progress) const {
    // Caller must hold mutex_.
    switch (config_.level) {
    case SyncLevel::OFF:
        return invalid_lsn;
    case SyncLevel::REMOTE_WRITE:
        return progress.received_lsn;
    case SyncLevel::REMOTE_FLUSH:
        return progress.flushed_lsn;
    case SyncLevel::REMOTE_APPLY:
        return progress.applied_lsn;
    }
    return invalid_lsn;
}

} // namespace giodb
