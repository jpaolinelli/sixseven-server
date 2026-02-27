#pragma once

#include "giodb/common/config.h"
#include "giodb/common/result.h"

#include <filesystem>

namespace giodb {

// Forward declarations.
class Catalog;
class CatalogPersistence;
class QueryEngine;
class StorageManager;

/// Bootstraps the system database and system tables on server startup.
///
/// On first initialization (no bootstrap flag file), creates all system tables
/// (sys_settings, sys_providers, sys_tables, sys_columns, sys_indexes,
/// sys_edge_types, sys_embedding_columns) and seeds default settings.
///
/// On subsequent starts, loads the persisted catalog (user tables, indexes,
/// edge types, embedding columns) from the system catalog tables, then opens
/// sys_settings and sys_providers for query access.
class SystemBootstrap {
public:
    /// Run bootstrap: create or restore system tables and catalog state.
    ///
    /// @param engine      QueryEngine (used for SQL INSERTs during seeding).
    /// @param catalog     System catalog.
    /// @param storage     StorageManager for physical storage.
    /// @param persistence CatalogPersistence for system table management.
    /// @param config      Current Config (used for seeding default values).
    /// @param data_dir    Root data directory.
    [[nodiscard]] static Result<void> bootstrap(QueryEngine& engine,
                                                Catalog& catalog,
                                                StorageManager& storage,
                                                CatalogPersistence& persistence,
                                                const Config& config,
                                                const std::filesystem::path& data_dir);

    /// Load settings from the sys_settings table and apply them to the config.
    [[nodiscard]] static Result<void>
    load_settings(QueryEngine& engine, Catalog& catalog, Config& config);

    /// Check whether the data directory has been bootstrapped.
    [[nodiscard]] static bool is_bootstrapped(const std::filesystem::path& data_dir);

private:
    /// Seed the sys_settings table with default settings.
    [[nodiscard]] static Result<void> seed_default_settings(QueryEngine& engine,
                                                            const Config& config);

    /// Write the bootstrap flag file to the data directory.
    static void mark_bootstrapped(const std::filesystem::path& data_dir);

    /// Bootstrap flag filename.
    static constexpr const char* bootstrap_flag_file = ".bootstrapped";
};

} // namespace giodb
