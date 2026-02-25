#include "giodb/catalog/catalog.h"
#include "giodb/common/config.h"
#include "giodb/common/logging.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/settings_cache.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/system_bootstrap.h"
#include "giodb/server/server.h"
#include "giodb/storage/disk_manager.h"

#include <csignal>
#include <filesystem>
#include <string>

namespace {

giodb::Server* g_server = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void signal_handler(int /*signo*/) {
    // Signal-safe: only touches an atomic flag. No logging, no mutex.
    if (g_server != nullptr) {
        g_server->request_shutdown();
    }
}

void install_signal_handlers() {
    // Ignore SIGPIPE — broken pipe from disconnected clients.
    std::signal(SIGPIPE, SIG_IGN);

    // Graceful shutdown on SIGINT (Ctrl+C) and SIGTERM.
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    // Parse command-line arguments.
    std::string config_path = "giodb.json";
    bool standby_flag = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--standby") {
            standby_flag = true;
        } else {
            config_path = arg;
        }
    }

    // Load config from file if provided, otherwise use defaults.
    auto config_result = giodb::Config::load_from_file(config_path);
    if (!config_result) {
        giodb::init_logging("error");
        GIODB_LOG_ERROR("Failed to load config: {}", config_result.error().message);
        return 1;
    }
    auto config = std::move(*config_result);

    // Command-line --standby overrides config file.
    if (standby_flag) {
        config.standby_mode = true;
    }

    giodb::init_logging(config.log_level);
    GIODB_LOG_INFO("GioDB Server v{} starting", giodb::Server::VERSION);
    GIODB_LOG_INFO("  mode: {}", config.standby_mode ? "standby" : "primary");
    GIODB_LOG_INFO("  data_dir: {}", config.data_dir);
    GIODB_LOG_INFO("  port: {}", config.port);
    GIODB_LOG_INFO("  buffer_pool: {} MB", config.buffer_pool_size_mb);
    GIODB_LOG_INFO("  max_connections: {}", config.max_connections);
    GIODB_LOG_INFO("  shutdown_timeout: {}s", config.shutdown_timeout_s);

    if (config.standby_mode) {
        GIODB_LOG_INFO("  primary_host: {}", config.replication_primary_host);
        GIODB_LOG_INFO("  primary_port: {}", config.replication_primary_port);
    }

    // Initialize query execution infrastructure.
    std::filesystem::path data_dir(config.data_dir);
    std::filesystem::create_directories(data_dir);

    giodb::DiskManager disk_manager;
    giodb::Catalog catalog;
    giodb::StorageManager storage(disk_manager, data_dir);
    giodb::QueryEngine engine(catalog, storage);

    // Bootstrap system database (creates sys_settings, sys_providers, seeds defaults).
    auto boot = giodb::SystemBootstrap::bootstrap(engine, catalog, storage, config, data_dir);
    if (!boot) {
        GIODB_LOG_ERROR("bootstrap failed: {}", boot.error().message);
        return 1;
    }

    // Load settings cache and wire it to the engine.
    giodb::SettingsCache settings_cache;
    auto load = settings_cache.load(engine);
    if (!load) {
        GIODB_LOG_ERROR("settings cache load failed: {}", load.error().message);
        return 1;
    }
    engine.set_settings_cache(&settings_cache);

    // Switch engine to default user database.
    engine.set_current_database(giodb::default_database_id);

    install_signal_handlers();

    giodb::Server server(std::move(config));
    g_server = &server;

    // Wire query executor: route SQL to the shared QueryEngine.
    server.set_query_executor(
        [&engine](const std::string& sql) -> giodb::Result<giodb::QueryResult> {
            return engine.execute(sql);
        });

    auto result = server.start();
    g_server = nullptr;

    if (!result) {
        GIODB_LOG_ERROR("server error: {}", result.error().message);
        return 1;
    }

    GIODB_LOG_INFO("GioDB Server stopped cleanly");
    return 0;
}
