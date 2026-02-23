#pragma once

#include "giodb/common/config.h"
#include "giodb/common/result.h"

#include <filesystem>

namespace giodb {

// Forward declarations.
class Catalog;
class QueryEngine;
class StorageManager;

/// Bootstraps the system database and sys_settings table on server startup.
///
/// On first initialization (no bootstrap flag file), creates the giodb_system
/// database storage, the sys_settings table, and seeds it with default settings.
///
/// On subsequent starts, recreates the catalog-level metadata (since the catalog
/// is in-memory) but skips re-seeding if the bootstrap flag is present.
class SystemBootstrap {
public:
    /// Run bootstrap: create system database storage, sys_settings table,
    /// and seed default settings on first run.
    ///
    /// @param engine    QueryEngine (used to create tables and insert rows).
    /// @param catalog   System catalog.
    /// @param storage   StorageManager for physical storage.
    /// @param config    Current Config (used for seeding default values).
    /// @param data_dir  Root data directory.
    [[nodiscard]] static Result<void> bootstrap(QueryEngine& engine,
                                                Catalog& catalog,
                                                StorageManager& storage,
                                                const Config& config,
                                                const std::filesystem::path& data_dir);

    /// Load settings from the sys_settings table and apply them to the config.
    /// Implements the config priority chain: defaults < file < sys_settings.
    ///
    /// @param engine    QueryEngine (used to query sys_settings).
    /// @param catalog   System catalog.
    /// @param config    Config to apply overrides to (modified in place).
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
