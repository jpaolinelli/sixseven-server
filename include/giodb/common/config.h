#pragma once

#include "giodb/common/result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace giodb {

struct Config {
    std::string data_dir = "./data";
    uint16_t port = 5432;
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

    /// Create a Config with all default values.
    static Config load_defaults();

    /// Load config from a JSON file. Missing fields use defaults.
    /// Returns defaults if the file does not exist.
    /// Returns an error if the file exists but contains malformed JSON.
    static Result<Config> load_from_file(const std::string& path);

    /// Apply a single setting from sys_settings.
    /// Maps dotted setting keys (e.g., "server.port") to Config fields.
    /// Unknown keys are silently ignored.
    void apply_setting(const std::string& key, const std::string& value);
};

} // namespace giodb
