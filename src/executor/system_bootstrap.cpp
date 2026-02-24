#include "giodb/executor/system_bootstrap.h"

#include "giodb/catalog/catalog.h"
#include "giodb/common/logging.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"

#include <fstream>
#include <string>

namespace giodb {

Result<void> SystemBootstrap::bootstrap(QueryEngine& engine,
                                        Catalog& /*catalog*/,
                                        StorageManager& storage,
                                        const Config& config,
                                        const std::filesystem::path& data_dir) {
    // 1. Create system database storage directory.
    auto dir_result = storage.create_database_storage(system_database_id);
    if (!dir_result) {
        return make_error(dir_result.error().code,
                          "failed to create system database storage: " +
                              dir_result.error().message);
    }

    // 2. Switch QueryEngine to the system database.
    auto prev_db = engine.current_database_id();
    engine.set_current_database(system_database_id);

    // 3. Create sys_settings table.
    auto create_result = engine.execute("CREATE TABLE sys_settings ("
                                        "key VARCHAR, "
                                        "value VARCHAR, "
                                        "category VARCHAR, "
                                        "description VARCHAR, "
                                        "is_runtime_mutable BOOLEAN, "
                                        "PRIMARY KEY (key)"
                                        ")");
    if (!create_result) {
        engine.set_current_database(prev_db);
        return make_error(create_result.error().code,
                          "failed to create sys_settings table: " + create_result.error().message);
    }

    GIODB_LOG_INFO("system bootstrap: sys_settings table created");

    // 3b. Create sys_providers table.
    auto create_providers = engine.execute("CREATE TABLE sys_providers ("
                                           "provider_id INT, "
                                           "name VARCHAR, "
                                           "type VARCHAR, "
                                           "endpoint VARCHAR, "
                                           "model VARCHAR, "
                                           "api_key_encrypted VARCHAR, "
                                           "is_default BOOLEAN, "
                                           "created_at TIMESTAMP, "
                                           "PRIMARY KEY (provider_id)"
                                           ")");
    if (!create_providers) {
        engine.set_current_database(prev_db);
        return make_error(create_providers.error().code,
                          "failed to create sys_providers table: " +
                              create_providers.error().message);
    }

    GIODB_LOG_INFO("system bootstrap: sys_providers table created");

    // 4. Seed default settings on first run.
    if (!is_bootstrapped(data_dir)) {
        auto seed_result = seed_default_settings(engine, config);
        if (!seed_result) {
            engine.set_current_database(prev_db);
            return make_error(seed_result.error().code,
                              "failed to seed default settings: " + seed_result.error().message);
        }

        mark_bootstrapped(data_dir);
        GIODB_LOG_INFO("system bootstrap: default settings seeded (first run)");
    } else {
        GIODB_LOG_INFO("system bootstrap: skipping seed (already bootstrapped)");
    }

    // 5. Restore previous database context.
    engine.set_current_database(prev_db);
    return ok();
}

Result<void>
SystemBootstrap::load_settings(QueryEngine& engine, Catalog& /*catalog*/, Config& config) {
    // Switch to system database to query sys_settings.
    auto prev_db = engine.current_database_id();
    engine.set_current_database(system_database_id);

    auto result = engine.execute("SELECT key, value FROM sys_settings");
    engine.set_current_database(prev_db);

    if (!result) {
        return make_error(result.error().code,
                          "failed to load sys_settings: " + result.error().message);
    }

    // Apply each setting to the config.
    for (const auto& row : result->rows) {
        if (row.size() < 2 || row[0].is_null() || row[1].is_null()) {
            continue;
        }
        config.apply_setting(row[0].as_string(), row[1].as_string());
    }

    return ok();
}

bool SystemBootstrap::is_bootstrapped(const std::filesystem::path& data_dir) {
    return std::filesystem::exists(data_dir / bootstrap_flag_file);
}

Result<void> SystemBootstrap::seed_default_settings(QueryEngine& engine, const Config& config) {
    // Default settings as defined in the ticket.
    struct Setting {
        const char* key;
        std::string value;
        const char* category;
        const char* description;
        bool is_runtime_mutable;
    };

    std::vector<Setting> defaults = {
        {"server.port", std::to_string(config.port), "server", "TCP listen port", false},
        {"server.max_connections",
         std::to_string(config.max_connections),
         "server",
         "Max concurrent connections",
         false},
        {"storage.data_dir", config.data_dir, "storage", "Data directory path", false},
        {"storage.buffer_pool_size_mb",
         std::to_string(config.buffer_pool_size_mb),
         "storage",
         "Buffer pool size in MB",
         false},
        {"storage.wal_segment_size_mb",
         std::to_string(config.wal_segment_size_mb),
         "storage",
         "WAL segment size in MB",
         false},
        {"logging.level",
         config.log_level,
         "logging",
         "Log level (trace/debug/info/warn/error)",
         true},
        {"replication.archive_enabled",
         config.archive_enabled ? "true" : "false",
         "replication",
         "Enable WAL segment archiving",
         false},
        {"replication.archive_cleanup_policy",
         config.archive_cleanup_policy,
         "replication",
         "Archive cleanup policy (keep_all/keep_last_n/keep_since_lsn)",
         false},
        {"replication.max_wal_senders",
         std::to_string(config.replication_max_wal_senders),
         "replication",
         "Max concurrent replication connections",
         false},
        {"replication.keepalive_interval_ms",
         std::to_string(config.replication_keepalive_interval_ms),
         "replication",
         "Keepalive interval in milliseconds",
         true},
        {"replication.sender_timeout_ms",
         std::to_string(config.replication_sender_timeout_ms),
         "replication",
         "Sender timeout in milliseconds",
         true},
        {"replication.synchronous_mode",
         config.replication_synchronous_mode,
         "replication",
         "Sync replication level (off/remote_write/remote_flush/remote_apply)",
         true},
        {"replication.synchronous_standby_names",
         config.replication_synchronous_standby_names,
         "replication",
         "Comma-separated slot names for synchronous replication",
         true},
        {"replication.synchronous_commit_count",
         std::to_string(config.replication_synchronous_commit_count),
         "replication",
         "Number of standbys that must acknowledge before commit succeeds",
         true},
        {"replication.synchronous_timeout_ms",
         std::to_string(config.replication_synchronous_timeout_ms),
         "replication",
         "Timeout in ms before sync replication fallback triggers",
         true},
        {"replication.synchronous_fallback",
         config.replication_synchronous_fallback,
         "replication",
         "Fallback on sync timeout (error/warn/block)",
         true},
        {"replication.lag_warning_threshold_ms",
         std::to_string(config.replication_lag_warning_threshold_ms),
         "replication",
         "Replication lag threshold in ms before warning is logged",
         true},
        {"replication.disconnect_warning_threshold_ms",
         std::to_string(config.replication_disconnect_warning_threshold_ms),
         "replication",
         "Replica disconnect threshold in ms before error is logged",
         true},
    };

    for (const auto& s : defaults) {
        // Escape single quotes in values by doubling them.
        std::string escaped_value = s.value;
        std::string escaped_desc = s.description;

        std::string sql = "INSERT INTO sys_settings VALUES ('" + std::string(s.key) + "', '" +
                          escaped_value + "', '" + std::string(s.category) + "', '" + escaped_desc +
                          "', " + (s.is_runtime_mutable ? "TRUE" : "FALSE") + ")";

        auto result = engine.execute(sql);
        if (!result) {
            return make_error(result.error().code,
                              "failed to insert setting '" + std::string(s.key) +
                                  "': " + result.error().message);
        }
    }

    return ok();
}

void SystemBootstrap::mark_bootstrapped(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir);
    std::ofstream flag(data_dir / bootstrap_flag_file);
    flag << "bootstrapped\n";
}

} // namespace giodb
