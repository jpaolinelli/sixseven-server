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
#include "sixseven/graph/graph_engine_wal.h"
#include "sixseven/graph/harmonic_centrality.h"
#include "sixseven/graph/louvain.h"
#include "sixseven/graph/pagerank.h"
#include "sixseven/graph/strongly_connected_components.h"
#include "sixseven/graph/triangle_count.h"
#include "sixseven/server/auth.h"
#include "sixseven/server/promotion_manager.h"
#include "sixseven/server/server.h"
#include "sixseven/server/tcp_replication_connection.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/storage/clean_shutdown_marker.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/table/table_wal.h"
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
    sixseven::StorageManager storage(disk_manager, data_dir, sixseven::frames_from_config(config));
    storage.set_double_write_enabled(config.storage_double_write);
    sixseven::CatalogPersistence persistence(catalog, storage);
    sixseven::GraphEngine graph_engine(catalog, disk_manager, data_dir);
    sixseven::ProviderRegistry provider_registry(catalog);

    // Size the embedding worker pool from the machine's core count. At
    // >10M-row ingest, a 2-worker pool (the old default) cannot keep up
    // and the queue grows until we OOM. Use a quarter of the logical
    // cores (floor 2) so the embedders have headroom without starving the
    // SQL executor.
    sixseven::EmbeddingWorkerConfig embedding_config;
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0)
            hw = 4;
        embedding_config.num_workers = std::max(2u, hw / 4);
    }
    sixseven::EmbeddingWorkerPool embedding_pool(embedding_config);
    embedding_pool.register_provider("builtin/384",
                                     std::make_shared<sixseven::BuiltinProvider>(384));
    embedding_pool.set_provider_registry(&provider_registry);

    sixseven::BackfillManager backfill_manager(catalog, storage, embedding_pool);

    // Register all graph algorithms so FROM pagerank('follows') etc. work.
    sixseven::AlgorithmRegistry algorithm_registry;
    (void)algorithm_registry.register_algorithm(sixseven::make_pagerank_def(),
                                                sixseven::pagerank_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_connected_components_def(),
                                                sixseven::connected_components_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_betweenness_centrality_def(),
                                                sixseven::betweenness_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_closeness_centrality_def(),
                                                sixseven::closeness_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_degree_centrality_def(),
                                                sixseven::degree_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_eigenvector_centrality_def(),
                                                sixseven::eigenvector_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_harmonic_centrality_def(),
                                                sixseven::harmonic_centrality_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_clustering_coefficient_def(),
                                                sixseven::clustering_coefficient_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_community_detect_def(),
                                                sixseven::community_detect_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_triangle_count_def(),
                                                sixseven::triangle_count_execute);
    (void)algorithm_registry.register_algorithm(sixseven::make_strongly_connected_components_def(),
                                                sixseven::strongly_connected_components_execute);

    sixseven::QueryEngine engine(catalog, storage, &graph_engine);
    engine.set_provider_registry(&provider_registry);
    engine.set_catalog_persistence(&persistence);
    engine.set_embedding_worker_pool(&embedding_pool);
    engine.set_backfill_manager(&backfill_manager);
    engine.set_algorithm_registry(&algorithm_registry);

    // Detect first run before bootstrap so we can seed demo data afterwards.
    bool first_run = !sixseven::SystemBootstrap::is_bootstrapped(data_dir);

    // Bootstrap system database (creates/loads system tables and catalog).
    auto boot = sixseven::SystemBootstrap::bootstrap(
        engine, catalog, storage, persistence, config, data_dir);
    if (!boot) {
        SIXSEVEN_LOG_ERROR("bootstrap failed: {}", boot.error().message);
        return 1;
    }

    // -- WAL crash recovery -------------------------------------------------------
    //
    // The clean-shutdown marker (<data_dir>/clean_shutdown) is written AFTER every
    // graceful shutdown and DELETED at the start of a clean-path restart.
    //
    //   marker PRESENT  =>  previous run shut down cleanly; skip recovery.
    //   marker ABSENT   =>  crash / kill -9 detected; run WAL recovery.
    //
    // Recovery MUST run BEFORE the server accepts any connections so that
    // committed data is visible and uncommitted writes are rolled back.
    // A recovery failure is FATAL — we must never serve stale state.
    //
    // recovered_max_txn_id survives the block below (GDB-1247): it is the
    // highest transaction id observed anywhere in the WAL during recovery
    // (0 if recovery was skipped or no such id was ever written), and is used
    // after the WAL writer opens to seed TransactionManager's next_txn_id_
    // strictly above every id a prior process could have handed out --
    // otherwise a live transaction in this process could reuse an id already
    // stamped into xmin/xmax on some persisted tuple, breaking visibility.
    sixseven::txn_id_t recovered_max_txn_id = 0;
    {
        std::filesystem::path wal_dir = data_dir / "wal";
        sixseven::CleanShutdownMarker marker(data_dir);

        if (marker.exists()) {
            // Previous run was clean — delete the marker and skip recovery.
            SIXSEVEN_LOG_INFO("clean shutdown marker found — skipping WAL recovery");
            auto rm = marker.remove();
            if (!rm) {
                SIXSEVEN_LOG_WARN("failed to remove clean-shutdown marker: {}", rm.error().message);
            }
        } else {
            // No marker — assume crash; run ARIES recovery.
            SIXSEVEN_LOG_INFO("no clean shutdown marker — running WAL recovery (wal_dir={})",
                              wal_dir.string());

            // Load edge data from disk first so EdgeTables are populated and
            // can be registered with the graph recovery handler (GDB-1067).
            // This is the same work the background jthread does after recovery;
            // the flag below prevents the jthread from repeating it.
            auto edge_preload = graph_engine.load_edges();
            if (!edge_preload) {
                SIXSEVEN_LOG_WARN("edge pre-load before WAL recovery failed (non-fatal): {}",
                                  edge_preload.error().message);
            }

            sixseven::TableHeapRecoveryHandler recovery_handler;
            storage.for_each_table_heap(
                [&recovery_handler](sixseven::table_id_t tid, sixseven::TableHeap* heap) {
                    recovery_handler.register_table(tid, heap);
                });

            // Register all edge types with the graph recovery handler so
            // EDGE_INSERT/EDGE_DELETE WAL records are replayed (GDB-1067).
            sixseven::GraphEngineRecoveryHandler graph_recovery_handler;
            {
                auto all_edge_types = catalog.list_all_edge_types();
                for (const auto& et : all_edge_types) {
                    auto et_result = graph_engine.get_edge_table(et.database_id, et.name);
                    if (!et_result) {
                        SIXSEVEN_LOG_WARN("WAL recovery: could not find EdgeTable for '{}': {}",
                                          et.name,
                                          et_result.error().message);
                        continue;
                    }
                    sixseven::EdgeTable* edge_table = *et_result;
                    const auto& cfg = edge_table->config();
                    std::vector<sixseven::TypeId> prop_types;
                    prop_types.reserve(cfg.property_columns.size());
                    for (const auto& col : cfg.property_columns) {
                        prop_types.push_back(col.type);
                    }
                    graph_recovery_handler.register_edge_table(
                        et.database_id, et.name, edge_table, std::move(prop_types));
                }
            }

            sixseven::CompositeRecoveryHandler composite_handler(recovery_handler,
                                                                 graph_recovery_handler);
            sixseven::WalRecovery wal_recovery(wal_dir, composite_handler);
            auto recover_result = wal_recovery.recover();
            if (!recover_result) {
                SIXSEVEN_LOG_ERROR("WAL recovery failed — cannot start server: {}",
                                   recover_result.error().message);
                return 1;
            }
            const auto& stats = *recover_result;
            SIXSEVEN_LOG_INFO("WAL recovery complete: scanned={} redone={} undone={} "
                              "committed_txns={} aborted_txns={} max_lsn={} max_txn_id_seen={}",
                              stats.records_scanned,
                              stats.records_redone,
                              stats.records_undone,
                              stats.committed_txns,
                              stats.aborted_txns,
                              stats.max_lsn,
                              stats.max_txn_id_seen);
            recovered_max_txn_id = stats.max_txn_id_seen;
        }
    }

    // -- WAL writer ---------------------------------------------------------------
    //
    // Open the WAL writer AFTER crash recovery has finished reading the existing
    // WAL segments.  Both recovery and the writer use the same canonical path
    // (data_dir/"wal") — this is the single source of truth for the WAL directory.
    //
    // Startup ordering:
    //   1. WalRecovery reads existing segments (above).
    //   2. WalWriter::open() resumes from the last valid record in the latest
    //      existing segment — it does NOT truncate or destroy prior segments.
    //   3. All subsequent DML via QueryEngine emits real WAL records.
    //   4. On shutdown: flush + close WAL, THEN write the clean-shutdown marker
    //      so the marker only appears once all data is durably persisted.
    //
    // Lifetime: WalWriter is declared at main-function scope so it outlives the
    // Server, QueryEngine, and StorageManager (all of which hold a raw pointer
    // obtained via set_wal_writer).  Destructor runs last.
    std::filesystem::path wal_dir = data_dir / "wal";
    std::filesystem::create_directories(wal_dir);
    sixseven::WalWriter wal_writer(wal_dir);
    {
        auto wal_open = wal_writer.open();
        if (!wal_open) {
            SIXSEVEN_LOG_ERROR("WAL writer open failed — cannot start server: {}",
                               wal_open.error().message);
            return 1;
        }
    }
    SIXSEVEN_LOG_INFO("WAL writer opened (wal_dir={})", wal_dir.string());

    // -- Standby / read-only mode -------------------------------------------------
    //
    // When config.standby_mode is set, this process is a hot standby that
    // replicates from a primary server.  Wire the read-only enforcement flag
    // into the query engine FIRST so that any connection arriving before the
    // WAL receiver connects is still rejected for DML.
    //
    // Construction order (mirrors PostgreSQL startup):
    //   1. Mark engine standby -> reject DML immediately.
    //   2. Construct WalReceiver (no network activity yet).
    //   3. Construct PromotionManager (no network activity).
    //   4. Register the on-promoted callback so promotion toggles the engine
    //      back to read-write mode.
    //   5. Start the WAL receiver background thread (begins connecting).
    //
    // The WalReceiver, PromotionManager, and TableHeapRecoveryHandler are
    // declared here and outlive the Server (main-function scope).  On
    // shutdown the destructor order is: Server -> receiver.stop() ->
    // receiver destroyed, which is safe because stop() joins the thread.
    //
    // NOTE on live replication (AC1): The WalReceiver connects via a
    // ReplicationConnection to the primary.  The ConnectionFactory below
    // constructs a TcpReplicationConnection.  On Windows the CRT fd-assert
    // crash described in the ticket makes the live two-node path
    // unverifiable locally; all socket-free construction/read-only/promotion
    // tests pass.  The live round-trip is verified in CI (Linux) only.

    std::unique_ptr<sixseven::TableHeapRecoveryHandler> standby_recovery_handler;
    std::unique_ptr<sixseven::WalReceiver> wal_receiver;
    std::unique_ptr<sixseven::PromotionManager> promotion_manager;

    if (config.standby_mode) {
        // 1. Enable read-only / recovery mode immediately.
        engine.set_standby_mode(true);
        SIXSEVEN_LOG_INFO("standby: read-only mode enabled");

        // 2. Build the recovery handler so the receiver can replay WAL.
        standby_recovery_handler = std::make_unique<sixseven::TableHeapRecoveryHandler>();
        storage.for_each_table_heap(
            [&rh = *standby_recovery_handler](sixseven::table_id_t tid, sixseven::TableHeap* heap) {
                rh.register_table(tid, heap);
            });

        // 3. Construct the WalReceiver with retry/backoff from config.
        sixseven::WalReceiverOptions recv_opts;
        recv_opts.retry_interval = std::chrono::milliseconds(config.replication_retry_interval_ms);
        recv_opts.max_retry_interval =
            std::chrono::milliseconds(config.replication_max_retry_interval_ms);

        // ConnectionFactory: returns an error when host is empty (no primary
        // configured); otherwise opens a real blocking TCP socket to the
        // primary via TcpReplicationConnection.  The read-only enforcement
        // is independent and already active (step 1 above).
        sixseven::ConnectionFactory conn_factory = [](const std::string& host, uint16_t port)
            -> sixseven::Result<std::unique_ptr<sixseven::ReplicationConnection>> {
            if (host.empty()) {
                return sixseven::make_error(sixseven::StatusCode::INVALID_ARGUMENT,
                                            "standby: primary_host is not configured; "
                                            "WAL receiver will retry after backoff");
            }
            return sixseven::make_tcp_replication_connection(host, port);
        };

        wal_receiver = std::make_unique<sixseven::WalReceiver>(
            std::move(conn_factory), wal_dir, *standby_recovery_handler, recv_opts);

        // 4. Construct the PromotionManager.
        promotion_manager = std::make_unique<sixseven::PromotionManager>(
            config, wal_receiver.get(), wal_writer, disk_manager, wal_dir);

        // Load persisted timeline if present.
        {
            auto tl = promotion_manager->load_timeline();
            if (!tl) {
                SIXSEVEN_LOG_WARN("standby: failed to load timeline: {}", tl.error().message);
            }
        }

        // 4a. Register the on-promoted callback: toggle engine out of standby.
        promotion_manager->set_on_promoted([&engine]() {
            engine.set_standby_mode(false);
            SIXSEVEN_LOG_INFO("standby: promoted to primary -- writes now accepted");
        });

        // Wire the receiver into the engine for SHOW STANDBY STATUS /
        // pg_last_wal_replay_lsn().
        engine.set_wal_receiver(wal_receiver.get());

        // 5. Start the WAL receiver background thread.
        if (!config.replication_primary_host.empty()) {
            auto start_result = wal_receiver->start(config.replication_primary_host,
                                                    config.replication_primary_port);
            if (!start_result) {
                SIXSEVEN_LOG_WARN("standby: WAL receiver failed to start: {}",
                                  start_result.error().message);
                // Non-fatal: server runs as a read-only standby with no live
                // replication.  Operator can fix primary address and restart.
            } else {
                SIXSEVEN_LOG_INFO("standby: WAL receiver started (primary={}:{})",
                                  config.replication_primary_host,
                                  config.replication_primary_port);
            }
        } else {
            SIXSEVEN_LOG_WARN(
                "standby: replication_primary_host is empty -- "
                "WAL receiver not started; server is read-only with no live replication");
        }
    }

    // -- WAL archive manager -------------------------------------------------------
    //
    // Constructed AFTER the WalWriter (so its destructor runs first on scope exit,
    // ensuring stop() is called before the writer is closed - no dangling callback).
    // When archive_enabled=false (the default), start() is a no-op and no thread is
    // spawned: zero change to the default code path.
    //
    // Lifetime: archive_manager_ is an optional so we can skip construction entirely
    // when disabled.  stop() is called explicitly before wal_writer.close() at
    // shutdown to ensure the callback is deregistered before the writer goes away.
    std::unique_ptr<sixseven::WalArchiveManager> wal_archive_manager;
    if (config.archive_enabled) {
        std::filesystem::path archive_dir = data_dir / "wal_archive";
        sixseven::WalArchiveOptions archive_opts;
        archive_opts.enabled = true;
        archive_opts.cleanup_policy = sixseven::parse_cleanup_policy(config.archive_cleanup_policy);
        archive_opts.keep_last_n = config.archive_keep_last_n;

        wal_archive_manager =
            std::make_unique<sixseven::WalArchiveManager>(wal_dir, archive_dir, archive_opts);

        auto archive_start = wal_archive_manager->start();
        if (!archive_start) {
            SIXSEVEN_LOG_WARN("WAL archive manager start failed (non-fatal): {}",
                              archive_start.error().message);
            wal_archive_manager.reset(); // Disable archiving on failure.
        } else {
            // Register the segment-rotated callback so completed WAL segments
            // are enqueued for archival automatically.
            // OnSegmentRotated = std::function<void(uint64_t segment_id, lsn_t last_lsn)>
            wal_writer.set_on_segment_rotated(
                [&mgr = *wal_archive_manager](uint64_t segment_id, sixseven::lsn_t last_lsn) {
                    mgr.enqueue_segment(segment_id, last_lsn);
                });
            SIXSEVEN_LOG_INFO("WAL archive manager running: archive_dir={}", archive_dir.string());
        }
    }

    // Propagate writer to every existing TableHeap (already open via bootstrap)
    // and to every heap created afterwards (CREATE TABLE during this session).
    storage.set_wal_writer(&wal_writer);

    // Wire into QueryEngine so pg_current_wal_lsn() reflects real WAL position.
    engine.set_wal_writer(&wal_writer);

    // GDB-1247: wire the WAL writer into TransactionManager so it can persist
    // the txn-id high-water mark, and seed next_txn_id_ strictly above every
    // id observed during recovery (0 if recovery was skipped -- clean
    // shutdown -- or this is a fresh data directory, in which case
    // TransactionManager keeps its default start-at-1 behavior). Must happen
    // AFTER WalWriter::open() (a valid writer is required to persist
    // batches) and is safe to do before any connection is accepted, since no
    // transaction can have begun yet.
    engine.transaction_manager().set_wal_writer(&wal_writer);
    engine.transaction_manager().init_next_txn_id(recovered_max_txn_id + 1);

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

    // Set up authentication. Resolve the configured method before `config` is
    // moved into the Server below. Users are persisted in sys_users so they
    // survive restart; on first run (or empty store) we seed the default admin
    // 'demo'/'demo' (see UserManager::ensure_default_admin). ensure_users_table()
    // runs after bootstrap so the
    // table-id collision guard can see any loaded user tables.
    sixseven::AuthMethod auth_method = sixseven::AuthMethod::SCRAM_SHA_256;
    if (auto m = sixseven::parse_auth_method(config.auth_method)) {
        auth_method = *m;
    } else {
        SIXSEVEN_LOG_WARN("invalid auth_method '{}', defaulting to scram-sha-256: {}",
                          config.auth_method,
                          m.error().message);
    }

    sixseven::UserManager user_manager;
    auto users_table = sixseven::SystemBootstrap::ensure_users_table(persistence, catalog, storage);
    if (!users_table) {
        SIXSEVEN_LOG_ERROR("sys_users setup failed: {}", users_table.error().message);
        return 1;
    }
    if (*users_table) {
        user_manager.set_persistence(
            [&persistence](const sixseven::UserRecord& u) { return persistence.persist_user(u); },
            [&persistence](const std::string& name) { return persistence.remove_user(name); });
        auto loaded = persistence.load_users();
        if (loaded) {
            user_manager.load(std::move(*loaded));
        } else {
            SIXSEVEN_LOG_WARN("failed to load persisted users: {}", loaded.error().message);
        }
    }
    if (user_manager.empty()) {
        user_manager.ensure_default_admin(auth_method);
    }
    engine.set_user_manager(&user_manager);
    engine.set_auth_method(auth_method);

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
                if (!r || r->rows.empty() || r->rows[0].empty())
                    return -1;
                return r->rows[0][0].as_int64();
            };
            (void)engine.execute("BACKFILL EMBEDDINGS ON books");
            (void)engine.execute("BACKFILL EMBEDDINGS ON reviews");
            SIXSEVEN_LOG_INFO("demo: generating embeddings (one-time, on first start)...");

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
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
            if (prev_remaining != 0 &&
                count_nulls("SELECT COUNT(*) FROM books WHERE description_vec IS NULL") != 0) {
                SIXSEVEN_LOG_WARN(
                    "demo: embedding generation did not fully complete within timeout");
            }

            // Build secondary indexes only now — after embedding write-backs have
            // moved rows to their final heap slots — so the indexes point at live
            // tuples instead of deleted ones.
            if (auto r = sixseven::create_demo_indexes(engine); !r) {
                SIXSEVEN_LOG_WARN("demo index creation failed (non-fatal): {}", r.error().message);
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

    // Wire the user store so the server can authenticate incoming connections.
    server.set_user_manager(&user_manager);

    // Wire query executor: route SQL to the shared QueryEngine.
    // Resolves the client's startup database name to a database_id.
    server.set_query_executor(
        [&engine,
         &catalog](const std::string& sql,
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
        [&engine, &catalog](const std::string& sql, const std::string& database)
            -> sixseven::Result<std::vector<sixseven::ColumnDescription>> {
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

    // Flush all table heaps before writing the clean-shutdown marker so the
    // marker only appears once the data is durably on disk.
    {
        auto flush = storage.flush_all();
        if (!flush) {
            SIXSEVEN_LOG_WARN("pre-marker storage flush failed: {}", flush.error().message);
        }
    }

    // Stop the WAL archive manager BEFORE detaching/closing the writer.
    // This deregisters the segment-rotated callback and drains any pending
    // queue, ensuring no enqueue_segment call races with writer teardown.
    if (wal_archive_manager) {
        // Clear the callback first to prevent any in-flight rotation from
        // enqueuing after we call stop() (which drains the queue).
        wal_writer.set_on_segment_rotated(nullptr);
        auto archive_stop = wal_archive_manager->stop();
        if (!archive_stop) {
            SIXSEVEN_LOG_WARN("WAL archive manager stop failed: {}", archive_stop.error().message);
        }
    }

    // Stop the WAL receiver (standby) before detaching the WAL writer so the
    // receiver thread cannot write WAL records after the writer is closed.
    if (wal_receiver) {
        wal_receiver->stop();
        SIXSEVEN_LOG_INFO("standby: WAL receiver stopped");
    }

    // Detach WAL writer from all heaps and engine before closing it so no
    // DML races with the close.  Then flush+close to ensure every WAL record
    // written during this session is durably on disk before the marker appears.
    storage.set_wal_writer(nullptr);
    engine.set_wal_writer(nullptr);
    {
        auto wal_flush = wal_writer.flush();
        if (!wal_flush) {
            SIXSEVEN_LOG_WARN("WAL flush failed on shutdown: {}", wal_flush.error().message);
        }
        auto wal_close = wal_writer.close();
        if (!wal_close) {
            SIXSEVEN_LOG_WARN("WAL close failed on shutdown: {}", wal_close.error().message);
        }
    }

    // Write the clean-shutdown marker so the next startup can safely skip
    // WAL recovery. This is written LAST — after all data is flushed — so
    // a crash between flush and marker write is safe (the missing marker
    // triggers an idempotent recovery that finds nothing to redo/undo).
    {
        sixseven::CleanShutdownMarker marker(data_dir);
        auto write_result = marker.write();
        if (!write_result) {
            SIXSEVEN_LOG_WARN("failed to write clean-shutdown marker (non-fatal): {}",
                              write_result.error().message);
        } else {
            SIXSEVEN_LOG_INFO("clean-shutdown marker written: {}", marker.path().string());
        }
    }

    SIXSEVEN_LOG_INFO("SixSevenDB Server stopped cleanly");
    return 0;
}
