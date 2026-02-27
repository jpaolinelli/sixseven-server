#include "giodb/common/config.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace giodb {

Config Config::load_defaults() {
    Config config;
    // Default master_key_path derived from data_dir.
    config.master_key_path = config.data_dir + "/master.key";
    return config;
}

Result<Config> Config::load_from_file(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return ok(load_defaults());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error(StatusCode::IO_ERROR, "failed to open config file: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          std::string("malformed JSON in config file: ") + e.what());
    }

    Config config = load_defaults();

    if (j.contains("data_dir") && j["data_dir"].is_string()) {
        config.data_dir = j["data_dir"].get<std::string>();
    }
    if (j.contains("port") && j["port"].is_number_unsigned()) {
        auto port_value = j["port"].get<uint64_t>();
        if (port_value > 65535) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "port must be in range 0-65535, got: " + std::to_string(port_value));
        }
        config.port = static_cast<uint16_t>(port_value);
    }
    if (j.contains("log_level") && j["log_level"].is_string()) {
        config.log_level = j["log_level"].get<std::string>();
    }
    if (j.contains("buffer_pool_size_mb") && j["buffer_pool_size_mb"].is_number_unsigned()) {
        config.buffer_pool_size_mb = j["buffer_pool_size_mb"].get<size_t>();
    }
    if (j.contains("wal_segment_size_mb") && j["wal_segment_size_mb"].is_number_unsigned()) {
        config.wal_segment_size_mb = j["wal_segment_size_mb"].get<size_t>();
    }
    if (j.contains("max_connections") && j["max_connections"].is_number_unsigned()) {
        config.max_connections = j["max_connections"].get<size_t>();
    }
    if (j.contains("shutdown_timeout_s") && j["shutdown_timeout_s"].is_number()) {
        config.shutdown_timeout_s = j["shutdown_timeout_s"].get<int32_t>();
    }
    if (j.contains("auth_method") && j["auth_method"].is_string()) {
        config.auth_method = j["auth_method"].get<std::string>();
    }
    if (j.contains("master_key_path") && j["master_key_path"].is_string()) {
        config.master_key_path = j["master_key_path"].get<std::string>();
    } else {
        // Derive master_key_path from data_dir (which may have been overridden above).
        config.master_key_path = config.data_dir + "/master.key";
    }

    // Server mode: "primary" (default) or "standby".
    if (j.contains("server") && j["server"].is_object()) {
        const auto& server = j["server"];
        if (server.contains("mode") && server["mode"].is_string()) {
            config.standby_mode = (server["mode"].get<std::string>() == "standby");
        }
    }

    // Replication settings (standby-specific).
    if (j.contains("replication") && j["replication"].is_object()) {
        const auto& repl = j["replication"];
        if (repl.contains("primary_host") && repl["primary_host"].is_string()) {
            config.replication_primary_host = repl["primary_host"].get<std::string>();
        }
        if (repl.contains("primary_port") && repl["primary_port"].is_number_unsigned()) {
            auto port_value = repl["primary_port"].get<uint64_t>();
            if (port_value <= 65535) {
                config.replication_primary_port = static_cast<uint16_t>(port_value);
            }
        }
        if (repl.contains("retry_interval_ms") && repl["retry_interval_ms"].is_number()) {
            config.replication_retry_interval_ms = repl["retry_interval_ms"].get<int32_t>();
        }
        if (repl.contains("max_retry_interval_ms") && repl["max_retry_interval_ms"].is_number()) {
            config.replication_max_retry_interval_ms = repl["max_retry_interval_ms"].get<int32_t>();
        }
        if (repl.contains("promote_max_lag_bytes") && repl["promote_max_lag_bytes"].is_number()) {
            config.replication_promote_max_lag_bytes = repl["promote_max_lag_bytes"].get<int64_t>();
        }
        if (repl.contains("synchronous_mode") && repl["synchronous_mode"].is_string()) {
            config.replication_synchronous_mode = repl["synchronous_mode"].get<std::string>();
        }
        if (repl.contains("synchronous_standby_names") &&
            repl["synchronous_standby_names"].is_string()) {
            config.replication_synchronous_standby_names =
                repl["synchronous_standby_names"].get<std::string>();
        }
        if (repl.contains("synchronous_commit_count") &&
            repl["synchronous_commit_count"].is_number()) {
            config.replication_synchronous_commit_count =
                repl["synchronous_commit_count"].get<int32_t>();
        }
        if (repl.contains("synchronous_timeout_ms") && repl["synchronous_timeout_ms"].is_number()) {
            config.replication_synchronous_timeout_ms =
                repl["synchronous_timeout_ms"].get<int32_t>();
        }
        if (repl.contains("synchronous_fallback") && repl["synchronous_fallback"].is_string()) {
            config.replication_synchronous_fallback =
                repl["synchronous_fallback"].get<std::string>();
        }
        if (repl.contains("lag_warning_threshold_ms") &&
            repl["lag_warning_threshold_ms"].is_number()) {
            config.replication_lag_warning_threshold_ms =
                repl["lag_warning_threshold_ms"].get<int64_t>();
        }
        if (repl.contains("disconnect_warning_threshold_ms") &&
            repl["disconnect_warning_threshold_ms"].is_number()) {
            config.replication_disconnect_warning_threshold_ms =
                repl["disconnect_warning_threshold_ms"].get<int64_t>();
        }
    }

    return ok(std::move(config));
}

// NOLINTNEXTLINE(bugprone-branch-clone)
void Config::apply_setting(const std::string& key, const std::string& value) {
    if (key == "server.port") {
        auto v = std::stoul(value);
        if (v <= 65535) {
            port = static_cast<uint16_t>(v);
        }
    } else if (key == "server.max_connections") {
        max_connections = std::stoull(value);
    } else if (key == "storage.data_dir") {
        data_dir = value;
    } else if (key == "storage.buffer_pool_size_mb") {
        buffer_pool_size_mb = std::stoull(value);
    } else if (key == "storage.wal_segment_size_mb") {
        wal_segment_size_mb = std::stoull(value);
    } else if (key == "logging.level") {
        log_level = value;
    } else if (key == "replication.archive_enabled") {
        archive_enabled = (value == "TRUE" || value == "true" || value == "1");
    } else if (key == "replication.archive_cleanup_policy") {
        archive_cleanup_policy = value;
    } else if (key == "replication.max_wal_senders") {
        replication_max_wal_senders = std::stoi(value);
    } else if (key == "replication.keepalive_interval_ms") {
        replication_keepalive_interval_ms = std::stoi(value);
    } else if (key == "replication.sender_timeout_ms") {
        replication_sender_timeout_ms = std::stoi(value);
    } else if (key == "server.mode") {
        standby_mode = (value == "standby");
    } else if (key == "replication.primary_host") {
        replication_primary_host = value;
    } else if (key == "replication.primary_port") {
        auto v = std::stoul(value);
        if (v <= 65535) {
            replication_primary_port = static_cast<uint16_t>(v);
        }
    } else if (key == "replication.retry_interval_ms") {
        replication_retry_interval_ms = std::stoi(value);
    } else if (key == "replication.max_retry_interval_ms") {
        replication_max_retry_interval_ms = std::stoi(value);
    } else if (key == "replication.synchronous_mode") {
        replication_synchronous_mode = value;
    } else if (key == "replication.synchronous_standby_names") {
        replication_synchronous_standby_names = value;
    } else if (key == "replication.synchronous_commit_count") {
        replication_synchronous_commit_count = std::stoi(value);
    } else if (key == "replication.synchronous_timeout_ms") {
        replication_synchronous_timeout_ms = std::stoi(value);
    } else if (key == "replication.synchronous_fallback") {
        replication_synchronous_fallback = value;
    } else if (key == "replication.promote_max_lag_bytes") {
        replication_promote_max_lag_bytes = std::stoll(value);
    } else if (key == "replication.lag_warning_threshold_ms") {
        replication_lag_warning_threshold_ms = std::stoll(value);
    } else if (key == "replication.disconnect_warning_threshold_ms") {
        replication_disconnect_warning_threshold_ms = std::stoll(value);
    } else if (key == "server.shutdown_timeout_s") {
        shutdown_timeout_s = std::stoi(value);
    } else if (key == "server.auth_method") {
        auth_method = value;
    }
    // Unknown keys are silently ignored.
}

} // namespace giodb
