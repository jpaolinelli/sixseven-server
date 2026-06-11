#include "sixseven/common/config.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace sixseven {

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

// ---------------------------------------------------------------------------
// Helpers: exception-free numeric parsing via std::from_chars.
// Trailing junk (e.g. "123abc") is rejected — from_chars sets ptr to the
// character after the last consumed digit, and we require ptr == end.
// ---------------------------------------------------------------------------

namespace {

/// Parse an unsigned 64-bit integer.  Returns error on empty, non-numeric, or
/// trailing-junk input.
Result<uint64_t> parse_u64(const std::string& key, const std::string& value) {
    if (value.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': empty string");
    }
    uint64_t out = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': '" + value +
                              "' is not a valid non-negative integer");
    }
    return ok(out);
}

/// Parse a signed 32-bit integer.
Result<int32_t> parse_i32(const std::string& key, const std::string& value) {
    if (value.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': empty string");
    }
    int32_t out = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': '" + value +
                              "' is not a valid integer");
    }
    return ok(out);
}

/// Parse a signed 64-bit integer.
Result<int64_t> parse_i64(const std::string& key, const std::string& value) {
    if (value.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': empty string");
    }
    int64_t out = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': '" + value +
                              "' is not a valid integer");
    }
    return ok(out);
}

/// Parse a uint16_t port value (0-65535).
Result<uint16_t> parse_port(const std::string& key, const std::string& value) {
    auto r = parse_u64(key, value);
    if (!r) {
        return make_error(r.error().code, r.error().message);
    }
    if (*r > 65535) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid value for '" + key + "': " + value + " is out of range 0-65535");
    }
    return ok(static_cast<uint16_t>(*r));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// validate_setting — parse-only, no mutation.
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-branch-clone)
Result<void> Config::validate_setting(const std::string& key, const std::string& value) {
    if (key == "server.port") {
        auto r = parse_port(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "server.max_connections") {
        auto r = parse_u64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "storage.buffer_pool_size_mb") {
        auto r = parse_u64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "storage.wal_segment_size_mb") {
        auto r = parse_u64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.max_wal_senders") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.keepalive_interval_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.sender_timeout_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.primary_port") {
        auto r = parse_port(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.retry_interval_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.max_retry_interval_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.synchronous_commit_count") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.synchronous_timeout_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.promote_max_lag_bytes") {
        auto r = parse_i64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.lag_warning_threshold_ms") {
        auto r = parse_i64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "replication.disconnect_warning_threshold_ms") {
        auto r = parse_i64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    } else if (key == "server.shutdown_timeout_s") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
    }
    // String/bool keys and unknown keys accept any value — ok().
    return ok();
}

// ---------------------------------------------------------------------------
// apply_setting — validate then mutate.
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-branch-clone)
Result<void> Config::apply_setting(const std::string& key, const std::string& value) {
    if (key == "server.port") {
        auto r = parse_port(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        port = *r;
    } else if (key == "server.max_connections") {
        auto r = parse_u64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        max_connections = static_cast<size_t>(*r);
    } else if (key == "storage.data_dir") {
        data_dir = value;
    } else if (key == "storage.buffer_pool_size_mb") {
        auto r = parse_u64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        buffer_pool_size_mb = static_cast<size_t>(*r);
    } else if (key == "storage.wal_segment_size_mb") {
        auto r = parse_u64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        wal_segment_size_mb = static_cast<size_t>(*r);
    } else if (key == "logging.level") {
        log_level = value;
    } else if (key == "replication.archive_enabled") {
        archive_enabled = (value == "TRUE" || value == "true" || value == "1");
    } else if (key == "replication.archive_cleanup_policy") {
        archive_cleanup_policy = value;
    } else if (key == "replication.max_wal_senders") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_max_wal_senders = *r;
    } else if (key == "replication.keepalive_interval_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_keepalive_interval_ms = *r;
    } else if (key == "replication.sender_timeout_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_sender_timeout_ms = *r;
    } else if (key == "server.mode") {
        standby_mode = (value == "standby");
    } else if (key == "replication.primary_host") {
        replication_primary_host = value;
    } else if (key == "replication.primary_port") {
        auto r = parse_port(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_primary_port = *r;
    } else if (key == "replication.retry_interval_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_retry_interval_ms = *r;
    } else if (key == "replication.max_retry_interval_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_max_retry_interval_ms = *r;
    } else if (key == "replication.synchronous_mode") {
        replication_synchronous_mode = value;
    } else if (key == "replication.synchronous_standby_names") {
        replication_synchronous_standby_names = value;
    } else if (key == "replication.synchronous_commit_count") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_synchronous_commit_count = *r;
    } else if (key == "replication.synchronous_timeout_ms") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_synchronous_timeout_ms = *r;
    } else if (key == "replication.synchronous_fallback") {
        replication_synchronous_fallback = value;
    } else if (key == "replication.promote_max_lag_bytes") {
        auto r = parse_i64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_promote_max_lag_bytes = *r;
    } else if (key == "replication.lag_warning_threshold_ms") {
        auto r = parse_i64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_lag_warning_threshold_ms = *r;
    } else if (key == "replication.disconnect_warning_threshold_ms") {
        auto r = parse_i64(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        replication_disconnect_warning_threshold_ms = *r;
    } else if (key == "server.shutdown_timeout_s") {
        auto r = parse_i32(key, value);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        shutdown_timeout_s = *r;
    } else if (key == "server.auth_method") {
        auth_method = value;
    }
    // Unknown keys are silently ignored.
    return ok();
}

} // namespace sixseven