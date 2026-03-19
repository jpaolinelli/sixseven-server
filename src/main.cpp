#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/logging.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/server/server.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/provider_registry.h"

#include <csignal>
#include <filesystem>
#include <string>

namespace {

sixseven::Server* g_server = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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
    std::string config_path = "sixseven.json";
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
    auto config_result = sixseven::Config::load_from_file(config_path);
    if (!config_result) {
        sixseven::init_logging("error");
        SIXSEVEN_LOG_ERROR("Failed to load config: {}", config_result.error().message);
        return 1;
    }
    auto config = std::move(*config_result);

    // Command-line --standby overrides config file.
    if (standby_flag) {
        config.standby_mode = true;
    }

    sixseven::init_logging(config.log_level);
    SIXSEVEN_LOG_INFO("SixSevenDB Server v{} starting", sixseven::Server::VERSION);
    SIXSEVEN_LOG_INFO("  mode: {}", config.standby_mode ? "standby" : "primary");
    SIXSEVEN_LOG_INFO("  data_dir: {}", config.data_dir);
    SIXSEVEN_LOG_INFO("  port: {}", config.port);
    SIXSEVEN_LOG_INFO("  buffer_pool: {} MB", config.buffer_pool_size_mb);
    SIXSEVEN_LOG_INFO("  max_connections: {}", config.max_connections);
    SIXSEVEN_LOG_INFO("  shutdown_timeout: {}s", config.shutdown_timeout_s);

    if (config.standby_mode) {
        SIXSEVEN_LOG_INFO("  primary_host: {}", config.replication_primary_host);
        SIXSEVEN_LOG_INFO("  primary_port: {}", config.replication_primary_port);
    }

    // Initialize query execution infrastructure.
    std::filesystem::path data_dir(config.data_dir);
    std::filesystem::create_directories(data_dir);

    sixseven::DiskManager disk_manager;
    sixseven::Catalog catalog;
    sixseven::StorageManager storage(disk_manager, data_dir);
    sixseven::CatalogPersistence persistence(catalog, storage);
    sixseven::GraphEngine graph_engine(catalog, disk_manager, data_dir);
    sixseven::ProviderRegistry provider_registry(catalog);
    sixseven::EmbeddingWorkerPool embedding_pool;
    embedding_pool.register_provider("builtin/384", std::make_shared<sixseven::BuiltinProvider>(384));
    embedding_pool.set_provider_registry(&provider_registry);

    sixseven::QueryEngine engine(catalog, storage, &graph_engine);
    engine.set_provider_registry(&provider_registry);
    engine.set_catalog_persistence(&persistence);
    engine.set_embedding_worker_pool(&embedding_pool);

    // Bootstrap system database (creates/loads system tables and catalog).
    auto boot =
        sixseven::SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
    if (!boot) {
        SIXSEVEN_LOG_ERROR("bootstrap failed: {}", boot.error().message);
        return 1;
    }

    // Load persisted edge data from disk.
    auto edge_load = graph_engine.load_edges();
    if (!edge_load) {
        SIXSEVEN_LOG_ERROR("edge data load failed: {}", edge_load.error().message);
        return 1;
    }

    // Load settings cache and wire it to the engine.
    sixseven::SettingsCache settings_cache;
    auto load = settings_cache.load(engine);
    if (!load) {
        SIXSEVEN_LOG_ERROR("settings cache load failed: {}", load.error().message);
        return 1;
    }
    engine.set_settings_cache(&settings_cache);

    // Start embedding worker pool for async EMBEDDING column generation.
    auto pool_start = embedding_pool.start();
    if (!pool_start) {
        SIXSEVEN_LOG_WARN("embedding worker pool failed to start: {}", pool_start.error().message);
    }

    // Switch engine to default user database.
    engine.set_current_database(sixseven::default_database_id);

    install_signal_handlers();

    sixseven::Server server(std::move(config));
    g_server = &server;

    // Wire query executor: route SQL to the shared QueryEngine.
    // Resolves the client's startup database name to a database_id.
    server.set_query_executor(
        [&engine, &catalog](const std::string& sql,
                            const std::string& database) -> sixseven::Result<sixseven::QueryResult> {
            if (!database.empty()) {
                auto db = catalog.get_database(database);
                if (db) {
                    return engine.execute(sql, db->database_id);
                }
                // Unknown database name — fall through to default.
            }
            return engine.execute(sql);
        });

    // Wire query describer: route Describe to the shared QueryEngine.
    server.set_query_describer(
        [&engine, &catalog](
            const std::string& sql,
            const std::string& database) -> sixseven::Result<std::vector<sixseven::ColumnDescription>> {
            if (!database.empty()) {
                auto db = catalog.get_database(database);
                if (db) {
                    return engine.describe(sql, db->database_id);
                }
            }
            return engine.describe(sql);
        });

    auto result = server.start();
    g_server = nullptr;

    // Stop embedding worker pool before teardown.
    if (embedding_pool.is_running()) {
        auto pool_stop = embedding_pool.stop();
        if (!pool_stop) {
            SIXSEVEN_LOG_WARN("embedding worker pool stop failed: {}", pool_stop.error().message);
        }
    }

    if (!result) {
        SIXSEVEN_LOG_ERROR("server error: {}", result.error().message);
        return 1;
    }

    SIXSEVEN_LOG_INFO("SixSevenDB Server stopped cleanly");
    return 0;
}
