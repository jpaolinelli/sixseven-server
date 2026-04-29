#include "sixseven/executor/system_bootstrap.h"

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/logging.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"

#include <fstream>
#include <string>

namespace sixseven {

Result<void> SystemBootstrap::bootstrap(QueryEngine& engine,
                                        Catalog& catalog,
                                        StorageManager& storage,
                                        CatalogPersistence& persistence,
                                        const Config& config,
                                        const std::filesystem::path& data_dir) {
    // 1. Create system database storage directory (idempotent).
    auto dir_result = storage.create_database_storage(system_database_id);
    if (!dir_result) {
        return make_error(dir_result.error().code,
                          "failed to create system database storage: " +
                              dir_result.error().message);
    }

    bool first_run = !is_bootstrapped(data_dir);

    if (first_run) {
        // -----------------------------------------------------------------
        // First run: create all system tables from scratch.
        // -----------------------------------------------------------------
        SIXSEVEN_LOG_INFO("system bootstrap: first run — creating system tables");

        // Create sys_settings and sys_providers with reserved IDs.
        auto r1 = persistence.create_sys_table_public(sys_settings_schema());
        if (!r1) {
            return make_error(r1.error().code,
                              "failed to create sys_settings: " + r1.error().message);
        }

        auto r2 = persistence.create_sys_table_public(sys_providers_schema());
        if (!r2) {
            return make_error(r2.error().code,
                              "failed to create sys_providers: " + r2.error().message);
        }

        // Create the 5 system catalog tables.
        auto r3 = persistence.create_system_catalog_tables();
        if (!r3) {
            return make_error(r3.error().code,
                              "failed to create system catalog tables: " + r3.error().message);
        }

        // Create sys_embedding_jobs for durable embedding job queue.
        auto r_ej = persistence.create_embedding_jobs_table();
        if (!r_ej) {
            return make_error(r_ej.error().code,
                              "failed to create sys_embedding_jobs: " + r_ej.error().message);
        }

        // Create the default 'sixseven' database and persist it.
        auto existing_db = catalog.get_database("sixseven");
        if (!existing_db) {
            auto r_db = catalog.restore_database(default_database_id, "sixseven");
            if (!r_db) {
                return make_error(r_db.error().code,
                                  "failed to create sixseven database: " +
                                      r_db.error().message);
            }
        }
        auto dir_db = storage.create_database_storage(default_database_id);
        if (!dir_db) {
            return make_error(dir_db.error().code,
                              "failed to create sixseven database storage: " +
                                  dir_db.error().message);
        }
        auto p_db = persistence.persist_database(default_database_id, "sixseven");
        if (!p_db) {
            return make_error(p_db.error().code,
                              "failed to persist sixseven database: " +
                                  p_db.error().message);
        }

        // Ensure user table IDs start after all system tables.
        if (catalog.next_table_id() < first_user_table_id) {
            catalog.set_next_table_id(first_user_table_id);
        }

        // Seed default settings via SQL INSERT.
        auto prev_db = engine.current_database_id();
        engine.set_current_database(system_database_id);

        auto seed_result = seed_default_settings(engine, config);
        if (!seed_result) {
            engine.set_current_database(prev_db);
            return make_error(seed_result.error().code,
                              "failed to seed default settings: " + seed_result.error().message);
        }

        engine.set_current_database(prev_db);

        mark_bootstrapped(data_dir);
        SIXSEVEN_LOG_INFO("system bootstrap: first run complete");
    } else {
        // -----------------------------------------------------------------
        // Subsequent run: load persisted catalog from disk.
        // -----------------------------------------------------------------
        SIXSEVEN_LOG_INFO("system bootstrap: loading persisted catalog");

        // Open sys_settings and sys_providers.
        auto r1 = persistence.open_sys_table_public(sys_settings_schema());
        if (!r1) {
            return make_error(r1.error().code,
                              "failed to open sys_settings: " + r1.error().message);
        }

        auto r2 = persistence.open_sys_table_public(sys_providers_schema());
        if (!r2) {
            return make_error(r2.error().code,
                              "failed to open sys_providers: " + r2.error().message);
        }

        // Open sys_databases before load_catalog so databases are available.
        // Migration: if the file doesn't exist (old deployment), create and
        // back-fill from sys_tables.
        auto r_sdb = persistence.open_sys_table_public(sys_databases_schema());
        if (!r_sdb) {
            // Old deployment without sys_databases — migrate.
            SIXSEVEN_LOG_INFO("system bootstrap: migrating — creating sys_databases");
            auto create_sdb = persistence.create_sys_table_public(sys_databases_schema());
            if (!create_sdb) {
                return make_error(create_sdb.error().code,
                                  "failed to create sys_databases during migration: " +
                                      create_sdb.error().message);
            }
            auto migrate = persistence.migrate_databases_from_sys_tables();
            if (!migrate) {
                return make_error(migrate.error().code,
                                  "failed to migrate databases: " +
                                      migrate.error().message);
            }
        }

        // Load the full catalog (sys_tables, sys_columns, etc.).
        auto load = persistence.load_catalog();
        if (!load) {
            return make_error(load.error().code, "failed to load catalog: " + load.error().message);
        }

        // Open sys_embedding_jobs for durable embedding job queue.
        auto r_ej = persistence.open_embedding_jobs_table();
        if (!r_ej) {
            return make_error(r_ej.error().code,
                              "failed to open sys_embedding_jobs: " + r_ej.error().message);
        }

        // Ensure user table IDs start after all system tables.
        if (catalog.next_table_id() < first_user_table_id) {
            catalog.set_next_table_id(first_user_table_id);
        }

        // Migration: ensure every EMBEDDING column has a corresponding HNSW
        // IndexDef in sys_indexes.  Old deployments created the embedding
        // metadata but never persisted the companion index entry.
        {
            auto all_emb = catalog.list_all_embedding_columns();
            int migrated = 0;
            for (const auto& emb : all_emb) {
                auto table = catalog.get_table_by_id(emb.table_id);
                if (!table) {
                    continue;
                }

                // Find the column name from the ordinal.
                std::string col_name;
                for (const auto& col : table->columns) {
                    if (col.ordinal == emb.column_id) {
                        col_name = col.name;
                        break;
                    }
                }
                if (col_name.empty()) {
                    continue;
                }

                std::string index_name = "hnsw_" + table->name + "_" + col_name;

                // Check if this table already has an HNSW index for this column.
                auto table_indexes = catalog.list_indexes(emb.table_id);
                bool already_has_hnsw = false;
                for (const auto& idx : table_indexes) {
                    if (idx.index_type == "hnsw" && idx.columns == col_name) {
                        already_has_hnsw = true;
                        break;
                    }
                }
                if (already_has_hnsw) {
                    continue; // Already present for this table, nothing to do.
                }

                // Create the missing HNSW IndexDef.
                IndexDef def;
                def.table_id = emb.table_id;
                def.name = index_name;
                def.index_type = "hnsw";
                def.columns = col_name;
                def.is_unique = false;

                auto idx_id = catalog.create_index(def);
                if (!idx_id) {
                    SIXSEVEN_LOG_WARN("system bootstrap: failed to create missing HNSW index '{}': {}",
                                      index_name, idx_id.error().message);
                    continue;
                }

                // Persist to sys_indexes so it survives the next restart.
                def.index_id = *idx_id;
                auto pr = persistence.persist_index(def);
                if (!pr) {
                    SIXSEVEN_LOG_WARN("system bootstrap: failed to persist HNSW index '{}': {}",
                                      index_name, pr.error().message);
                    continue;
                }

                ++migrated;
                SIXSEVEN_LOG_INFO("system bootstrap: created missing HNSW index '{}'", index_name);
            }
            if (migrated > 0) {
                SIXSEVEN_LOG_INFO("system bootstrap: migrated {} HNSW index entries", migrated);
            }
        }

        SIXSEVEN_LOG_INFO("system bootstrap: catalog loaded from disk");
    }

    // Register pg_catalog virtual tables (always, regardless of first run).
    register_virtual_catalog_tables(catalog);

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

void SystemBootstrap::register_virtual_catalog_tables(Catalog& catalog) {
    // pg_database — lists all databases.
    VirtualTableDef pg_database;
    pg_database.name = "pg_database";
    pg_database.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "datname", TypeId::STRING, false, ""},
    };
    pg_database.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& db : catalog.list_databases()) {
            rows.push_back({std::to_string(db.database_id), db.name});
        }
        return rows;
    };
    catalog.register_virtual_table(std::move(pg_database));

    SIXSEVEN_LOG_INFO("system bootstrap: registered {} pg_catalog virtual tables",
                      catalog.list_virtual_tables().size());
}

void SystemBootstrap::mark_bootstrapped(const std::filesystem::path& data_dir) {
    std::filesystem::create_directories(data_dir);
    std::ofstream flag(data_dir / bootstrap_flag_file);
    flag << "bootstrapped\n";
}

} // namespace sixseven
