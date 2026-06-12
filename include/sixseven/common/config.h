#pragma once

#include "sixseven/common/result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sixseven {

struct Config {
    std::string data_dir = "./data";
    uint16_t port = 6767;
    std::string log_level = "info";
    size_t buffer_pool_size_mb = 256;
    size_t wal_segment_size_mb = 16;
    size_t max_connections = 100;
    std::string master_key_path; // defaults to <data_dir>/master.key
    bool archive_enabled = false;
    std::string archive_cleanup_policy = "keep_all";
    int32_t replication_max_wal_senders = 10;
    int32_t replication_keepalive_interval_ms = 10000;
    int32_t replication_sender_timeout_ms = 60000;

    // Standby mode settings.
    bool standby_mode = false;                    ///< True when server runs as read-only standby.
    std::string replication_primary_host;         ///< Hostname/IP of the primary.
    uint16_t replication_primary_port = 6767;     ///< Port of the primary.
    int32_t replication_retry_interval_ms = 5000; ///< Initial retry interval for reconnection.
    int32_t replication_max_retry_interval_ms = 60000; ///< Maximum retry interval (backoff cap).

    // Promotion settings.
    int64_t replication_promote_max_lag_bytes = 0; ///< Max replay lag for promotion (0 = any lag).

    // Synchronous replication settings.
    std::string replication_synchronous_mode =
        "off"; ///< off/remote_write/remote_flush/remote_apply.
    std::string replication_synchronous_standby_names;  ///< Comma-separated slot names for sync.
    int32_t replication_synchronous_commit_count = 1;   ///< How many standbys must ack.
    int32_t replication_synchronous_timeout_ms = 30000; ///< Timeout before fallback.
    std::string replication_synchronous_fallback = "error"; ///< error/warn/block.

    // Authentication settings.
    std::string auth_method = "scram-sha-256"; ///< trust, md5, or scram-sha-256.

    // Server lifecycle settings.
    int32_t shutdown_timeout_s = 30; ///< Seconds to wait for active queries on shutdown.

    // Health monitoring thresholds.
    int64_t replication_lag_warning_threshold_ms = 10000;        ///< Lag threshold for warnings.
    int64_t replication_disconnect_warning_threshold_ms = 60000; ///< Disconnect threshold.

    /// Create a Config with all default values.
    static Config load_defaults();

    /// Load config from a JSON file. Missing fields use defaults.
    /// Returns defaults if the file does not exist.
    /// Returns an error if the file exists but contains malformed JSON.
    static Result<Config> load_from_file(const std::string& path);

    /// Apply a single setting from sys_settings.
    /// Maps dotted setting keys (e.g., "server.port") to Config fields.
    /// Unknown keys are silently ignored.
    /// Returns INVALID_ARGUMENT if the value cannot be parsed for a numeric key.
    [[nodiscard]] Result<void> apply_setting(const std::string& key, const std::string& value);

    /// Validate that a value is acceptable for the given key without applying it.
    /// Returns ok() for string/bool keys (any value accepted) and for unknown keys.
    /// Returns INVALID_ARGUMENT if the value fails numeric parsing or range checks.
    [[nodiscard]] static Result<void> validate_setting(const std::string& key,
                                                       const std::string& value);
};

/// Convert a Config's buffer_pool_size_mb into a StorageManager frame count.
/// This is the single source of the MB->frames conversion: buffer_pool_size_mb * 128.
/// Used by main.cpp when constructing StorageManager and by tests that need to verify
/// the shared conversion logic.
/// Callers must ensure buffer_pool_size_mb does not exceed UINT32_MAX/128
/// (enforced by Config::load_from_file).
[[nodiscard]] uint32_t frames_from_config(const Config& cfg);

} // namespace sixseven