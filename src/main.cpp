#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/crash_handler.h"
#include "sixseven/common/logging.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/demo_bootstrap.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/closeness_centrality.h"
#include "sixseven/graph/clustering_coefficient.h"
#include "sixseven/graph/connected_components.h"
#include "sixseven/graph/degree_centrality.h"
#include "sixseven/graph/eigenvector_centrality.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/harmonic_centrality.h"
#include "sixseven/graph/louvain.h"
#include "sixseven/graph/pagerank.h"
#include "sixseven/graph/strongly_connected_components.h"
#include "sixseven/graph/triangle_count.h"
#include "sixseven/server/server.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/backfill_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/provider_registry.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <thread>

namespace {

sixseven::Server* g_server = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void signal_handler(int /*signo*/) {
    // Signal-safe: only touches an atomic flag. No logging, no mutex.
    if (g_server != nullptr) {
        g_server->request_shutdown();
    }
}

void install_signal_handlers() {
#if !defined(_WIN32)
    // Ignore SIGPIPE — broken pipe from disconnected clients.
    // (Windows has no SIGPIPE; Winsock returns WSAECONNRESET instead.)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Graceful shutdown on SIGINT (Ctrl+C) and SIGTERM.
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    // Install fatal-signal + std::terminate handlers as early as possible
    // so any subsequent crash dumps a backtrace to stderr instead of dying
    // silently with `zsh: trace trap`.
    sixseven::install_crash_handlers();

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
    sixseven::StorageManager storage(disk_manager, data_dir,
                                     static_cast<uint32_t>(config.buffer_pool_size_mb * 128));
    sixseven::CatalogPersistence persistence(catalog, storage);
    sixseven::GraphEngine graph_engine(catalog, disk_manager, data_dir);
    sixseven::ProviderRegistry provider_registry(catalog);

    // Size the embedding worker pool from the machine's core count. At
    // >10M-row ingest, a 2-worker pool (the old default) cannot keep up
    // and the queue grows until we OOM. Use half the logical cores
    // (floor 4) so the embedders have headroom without starving the
    // SQL executor.
    sixseven::EmbeddingWorkerConfig embedding_config;
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        embedding_config.num_workers = std::max(2u, hw / 4);
    }
    sixseven::EmbeddingWorkerPool embedding_pool(embedding_config);
    embedding_pool.register_provider("builtin/384", std::make_shared<sixseven::BuiltinProvider>(384));
    embedding_pool.set_provider_registry(&provider_registry);

    sixseven::BackfillManager backfill_manager(catalog, storage, embedding_pool);

    // Register all graph algorithms so FROM pagerank('follows') etc. work.
    sixseven::AlgorithmRegistry algorithm_registry;
    (void)algorithm_registry.register_algorithm(sixseven::make_pagerank_def(), sixseven::pagerank_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_connected_components_def(), sixseven::connected_components_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_betweenness_centrality_def(), sixseven::betweenness_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_closeness_centrality_def(), sixseven::closeness_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_degree_centrality_def(), sixseven::degree_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_eigenvector_centrality_def(), sixseven::eigenvector_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_harmonic_centrality_def(), sixseven::harmonic_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_clustering_coefficient_def(), sixseven::clustering_coefficient_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_community_detect_def(), sixseven::community_detect_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_triangle_count_def(), sixseven::triangle_count_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_strongly_connected_components_def(), sixseven::strongly_connected_components_execute);

    sixseven::QueryEngine engine(catalog, storage, &graph_engine);
    engine.set_provider_registry(&provider_registry);
    engine.set_catalog_persistence(&persistence);
    engine.set_embedding_worker_pool(&embedding_pool);
    engine.set_backfill_manager(&backfill_manager);
    engine.set_algorithm_registry(&algorithm_registry);

    // Detect first run before bootstrap so we can seed demo data afterwards.
    bool first_run = !sixseven::SystemBootstrap::is_bootstrapped(data_dir);

    // Bootstrap system database (creates/loads system tables and catalog).
    auto boot =
        sixseven::SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
    if (!boot) {
        SIXSEVEN_LOG_ERROR("bootstrap failed: {}", boot.error().message);
        return 1;
    }

    // Load B+ tree and hash indexes asynchronously from persisted disk files.
    // Indexes are loaded in background threads; queries fall back to sequential
    // scan until ready. On first run (no index files), rebuilds from table data
    // and persists to disk for next startup.
    sixseven::IndexManager index_manager(catalog, storage);
    index_manager.set_catalog_persistence(&persistence);
    engine.set_index_manager(&index_manager);
    engine.set_hnsw_indexes(index_manager.hnsw_map());
    auto async_load = index_manager.start_async_load();
    if (!async_load) {
        SIXSEVEN_LOG_ERROR("index async load failed: {}", async_load.error().message);
        return 1;
    }

    // Load persisted edge data in a background thread so the server can
    // start accepting connections immediately.  GraphEngine::load_edges()
    // holds its internal mutex, so edge queries arriving before loading
    // finishes simply block until the data is ready — they won't crash.
    std::atomic<bool> edge_load_ok{true};
    std::jthread edge_load_thread([&graph_engine, &edge_load_ok]() {
        auto edge_load = graph_engine.load_edges();
        if (!edge_load) {
            SIXSEVEN_LOG_ERROR("edge data load failed: {}", edge_load.error().message);
            edge_load_ok.store(false);
        }
    });

    // Load settings cache and wire it to the engine.
    sixseven::SettingsCache settings_cache;
    auto load = settings_cache.load(engine);
    if (!load) {
        SIXSEVEN_LOG_ERROR("settings cache load failed: {}", load.error().message);
        return 1;
    }
    engine.set_settings_cache(&settings_cache);

    // NOTE: Embedding job persistence (sys_embedding_jobs) is available but
    // intentionally NOT wired here.  The INSERT hot path uses try_enqueue_batch
    // with 0ms timeout — pure in-memory, zero disk I/O.  Jobs that overflow
    // the queue are dropped; rows with NULL embeddings will be caught by a
    // future background scan.  Wiring persistence caused pool.start() to fail
    // when load() errored, silently killing all worker threads.
    //
    // TODO(GDB-627): Add a background NULL-embedding scanner that re-enqueues
    // jobs for rows missing their embeddings, making the system self-healing
    // without needing synchronous persistence on the INSERT path.

    // Start embedding worker pool for async EMBEDDING column generation.
    auto pool_start = embedding_pool.start();
    if (!pool_start) {
        SIXSEVEN_LOG_ERROR("EMBEDDING WORKERS NOT RUNNING — pool.start() failed: {}",
                           pool_start.error().message);
    } else {
        SIXSEVEN_LOG_INFO("embedding worker pool started successfully: {} workers",
                          embedding_config.num_workers);
    }

    // Switch engine to default user database.
    engine.set_current_database(sixseven::default_database_id);

    // On first run, populate the demo database so new users can immediately
    // explore relational, graph, and vector queries without any setup.
    if (first_run) {
        auto demo = sixseven::create_demo_database(engine);
        if (!demo) {
            SIXSEVEN_LOG_WARN("demo database creation failed (non-fatal): {}",
                              demo.error().message);
        } else {
            SIXSEVEN_LOG_INFO("demo database created successfully");

            // Generate every EMBEDDING vector synchronously before the server
            // starts serving. EMBEDDING columns are filled asynchronously by the
            // worker pool, and under the bootstrap insert flood some jobs are
            // dropped; BACKFILL re-enqueues any row still missing its vector.
            // Waiting here means NEAREST works immediately and the HNSW index can
            // be persisted populated (instead of empty).
            auto count_nulls = [&engine](const char* sql) -> int64_t {
                auto r = engine.execute(sql);
                if (!r || r->rows.empty() || r->rows[0].empty()) return -1;
                return r->rows[0][0].as_int64();
            };
            (void)engine.execute("BACKFILL EMBEDDINGS ON books");
            (void)engine.execute("BACKFILL EMBEDDINGS ON reviews");
            SIXSEVEN_LOG_INFO("demo: generating embeddings (one-time, on first start)...");

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(300);
            auto last_progress = std::chrono::steady_clock::now();
            int64_t prev_remaining = -1;
            while (std::chrono::steady_clock::now() < deadline) {
                int64_t books_null =
                    count_nulls("SELECT COUNT(*) FROM books WHERE description_vec IS NULL");
                int64_t reviews_null =
                    count_nulls("SELECT COUNT(*) FROM reviews WHERE review_vec IS NULL");
                if (books_null == 0 && reviews_null == 0) {
                    SIXSEVEN_LOG_INFO("demo: all embeddings generated");
                    break;
                }
                int64_t remaining =
                    std::max<int64_t>(0, books_null) + std::max<int64_t>(0, reviews_null);
                auto now = std::chrono::steady_clock::now();
                if (remaining != prev_remaining) {
                    SIXSEVEN_LOG_INFO("demo: embeddings remaining: {}", remaining);
                    prev_remaining = remaining;
                    last_progress = now;
                } else if (now - last_progress > std::chrono::seconds(20)) {
                    // No progress for a while — some jobs were dropped; re-issue.
                    (void)engine.execute("BACKFILL EMBEDDINGS ON books");
                    (void)engine.execute("BACKFILL EMBEDDINGS ON reviews");
                    last_progress = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (prev_remaining != 0 && count_nulls(
                    "SELECT COUNT(*) FROM books WHERE description_vec IS NULL") != 0) {
                SIXSEVEN_LOG_WARN(
                    "demo: embedding generation did not fully complete within timeout");
            }

            // Build secondary indexes only now — after embedding write-backs have
            // moved rows to their final heap slots — so the indexes point at live
            // tuples instead of deleted ones.
            if (auto r = sixseven::create_demo_indexes(engine); !r) {
                SIXSEVEN_LOG_WARN("demo index creation failed (non-fatal): {}",
                                  r.error().message);
            }

            // Make the whole demo dataset durable now, so it survives a hard kill
            // before any clean shutdown/checkpoint. INSERT does not maintain
            // secondary indexes, LINK keeps edges only in the (WAL-less) edge
            // heap buffer pool, and embedding write-backs populate the HNSW index
            // in memory — none of these are flushed until shutdown otherwise.
            if (auto r = index_manager.flush_all_indexes(); !r) {
                SIXSEVEN_LOG_WARN("post-demo index flush failed: {}", r.error().message);
            }
            if (auto r = graph_engine.flush_edges(); !r) {
                SIXSEVEN_LOG_WARN("post-demo edge flush failed: {}", r.error().message);
            }
        }
        // Flush all table heaps to disk (includes written-back embedding vectors)
        // so the demo data survives a crash or hard kill.
        auto flush = storage.flush_all();
        if (!flush) {
            SIXSEVEN_LOG_WARN("post-demo flush failed: {}", flush.error().message);
        }
    }

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

    // Wait for background edge loading to finish before teardown.
    if (edge_load_thread.joinable()) {
        edge_load_thread.join();
    }

    // Wait for any pending async index loading to complete.
    index_manager.wait_for_load_complete();

    // Persist all in-memory indexes to disk for fast startup next time.
    auto flush_indexes = index_manager.flush_all_indexes();
    if (!flush_indexes) {
        SIXSEVEN_LOG_WARN("index flush failed: {}", flush_indexes.error().message);
    }

    // Persist edge B+ tree indexes so they load directly on next startup.
    auto flush_edges = graph_engine.flush_edge_indexes();
    if (!flush_edges) {
        SIXSEVEN_LOG_WARN("edge index flush failed: {}", flush_edges.error().message);
    }

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
