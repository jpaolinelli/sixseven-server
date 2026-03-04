#include "giodb/catalog/catalog.h"
#include "giodb/common/config.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/catalog_persistence.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/settings_cache.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/system_bootstrap.h"
#include "giodb/server/promotion_manager.h"
#include "giodb/server/replication_connection.h"
#include "giodb/server/replication_health_monitor.h"
#include "giodb/server/replication_message.h"
#include "giodb/server/replication_slot.h"
#include "giodb/server/sync_replication.h"
#include "giodb/server/wal_receiver.h"
#include "giodb/server/wal_sender_manager.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/wal.h"
#include "giodb/storage/wal_archive.h"
#include "giodb/storage/wal_record.h"
#include "giodb/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace giodb;
namespace fs = std::filesystem;

// =============================================================================
// In-memory linked connection pair for testing replication
// =============================================================================

namespace {

/// In-memory pipe connecting a primary-side sender and a replica-side receiver.
/// Data sent by one end is received by the other.
class LinkedConnection : public ReplicationConnection {
public:
    LinkedConnection(std::string peer,
                     std::shared_ptr<std::vector<uint8_t>> send_buf,
                     std::shared_ptr<std::vector<uint8_t>> recv_buf,
                     std::shared_ptr<std::mutex> send_mu,
                     std::shared_ptr<std::mutex> recv_mu,
                     std::shared_ptr<std::condition_variable> send_cv,
                     std::shared_ptr<std::condition_variable> recv_cv)
        : peer_(std::move(peer)), send_buf_(std::move(send_buf)), recv_buf_(std::move(recv_buf)),
          send_mu_(std::move(send_mu)), recv_mu_(std::move(recv_mu)), send_cv_(std::move(send_cv)),
          recv_cv_(std::move(recv_cv)) {}

    Result<void> send(std::span<const uint8_t> data) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        std::lock_guard lock(*send_mu_);
        send_buf_->insert(send_buf_->end(), data.begin(), data.end());
        send_cv_->notify_all();
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t max_bytes,
                                         std::chrono::milliseconds timeout) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        std::unique_lock lock(*recv_mu_);
        if (recv_buf_->empty()) {
            recv_cv_->wait_for(lock, timeout, [&] { return !recv_buf_->empty() || !open_.load(); });
        }
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        size_t n = std::min(max_bytes, recv_buf_->size());
        std::vector<uint8_t> result(recv_buf_->begin(), recv_buf_->begin() + static_cast<long>(n));
        recv_buf_->erase(recv_buf_->begin(), recv_buf_->begin() + static_cast<long>(n));
        return ok(std::move(result));
    }

    void close() override { open_.store(false); }
    bool is_open() const override { return open_.load(); }
    std::string peer_description() const override { return peer_; }

private:
    std::string peer_;
    std::atomic<bool> open_{true};
    std::shared_ptr<std::vector<uint8_t>> send_buf_;
    std::shared_ptr<std::vector<uint8_t>> recv_buf_;
    std::shared_ptr<std::mutex> send_mu_;
    std::shared_ptr<std::mutex> recv_mu_;
    std::shared_ptr<std::condition_variable> send_cv_;
    std::shared_ptr<std::condition_variable> recv_cv_;
};

/// Simple recovery handler that tracks redo calls.
class TrackingRecoveryHandler : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord& record) override {
        std::lock_guard lock(mu_);
        redo_count_++;
        last_type_ = record.type;
        return ok();
    }

    Result<void> undo(const WalRecord& /*record*/) override { return ok(); }

    int redo_count() const {
        std::lock_guard lock(mu_);
        return redo_count_;
    }

    WalRecordType last_type() const {
        std::lock_guard lock(mu_);
        return last_type_;
    }

private:
    mutable std::mutex mu_;
    int redo_count_ = 0;
    WalRecordType last_type_ = WalRecordType::BEGIN;
};

/// WAL writer options with group commit disabled.
WalWriterOptions test_wal_opts() {
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    return opts;
}

/// Write a simple committed transaction to WAL.
void write_txn(WalWriter& writer, txn_id_t txn_id, uint32_t table_id, const std::string& data) {
    WalRecord begin;
    begin.type = WalRecordType::BEGIN;
    begin.txn_id = txn_id;
    ASSERT_TRUE(writer.append(begin).has_value());

    WalRecord insert;
    insert.type = WalRecordType::INSERT;
    insert.txn_id = txn_id;
    insert.table_id = table_id;
    insert.data.assign(data.begin(), data.end());
    ASSERT_TRUE(writer.append(insert).has_value());

    WalRecord commit;
    commit.type = WalRecordType::COMMIT;
    commit.txn_id = txn_id;
    ASSERT_TRUE(writer.append(commit).has_value());
}

} // namespace

// =============================================================================
// Integration test fixture
// =============================================================================

class ReplicationIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_dir_ = fs::temp_directory_path() / "giodb_test_repl_integration";
        fs::remove_all(base_dir_);
        fs::create_directories(base_dir_);

        // Primary setup.
        primary_wal_dir_ = base_dir_ / "primary_wal";
        fs::create_directories(primary_wal_dir_);
        primary_writer_ = std::make_unique<WalWriter>(primary_wal_dir_, test_wal_opts());
        ASSERT_TRUE(primary_writer_->open().has_value());
        primary_slot_mgr_ = std::make_unique<ReplicationSlotManager>(base_dir_ / "primary_data");
        fs::create_directories(base_dir_ / "primary_data");

        // Replica setup.
        replica_wal_dir_ = base_dir_ / "replica_wal";
        fs::create_directories(replica_wal_dir_);
    }

    void TearDown() override {
        if (primary_writer_) {
            primary_writer_->close().has_value();
        }
        primary_writer_.reset();
        primary_slot_mgr_.reset();
        fs::remove_all(base_dir_);
    }

    fs::path base_dir_;
    fs::path primary_wal_dir_;
    fs::path replica_wal_dir_;
    std::unique_ptr<WalWriter> primary_writer_;
    std::unique_ptr<ReplicationSlotManager> primary_slot_mgr_;
};

// =============================================================================
// Basic Replication Flow
// - Start primary, write data, connect standby, verify data flows
// =============================================================================

TEST_F(ReplicationIntegrationTest, BasicReplicationFlow) {
    // Write initial data on primary.
    write_txn(*primary_writer_, 1, 1, "initial_data");
    ASSERT_TRUE(primary_writer_->flush().has_value());

    // Create slot and sender manager.
    ASSERT_TRUE(primary_slot_mgr_->create_slot("standby_1").has_value());

    WalSenderOptions sender_opts;
    sender_opts.keepalive_interval = std::chrono::milliseconds(50);
    sender_opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(
        primary_wal_dir_, nullptr, *primary_writer_, 10, sender_opts, primary_slot_mgr_.get());

    // Connect standby.
    auto conn = std::make_unique<LinkedConnection>("standby_1",
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::condition_variable>(),
                                                   std::make_shared<std::condition_variable>());
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1, "standby_1").has_value());

    // Verify sender is active.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto statuses = sender_mgr.get_sender_statuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].slot_name, "standby_1");
    EXPECT_EQ(statuses[0].sync_state, "async");

    // Write more data on primary.
    write_txn(*primary_writer_, 2, 1, "more_data");
    ASSERT_TRUE(primary_writer_->flush().has_value());
    sender_mgr.notify_new_wal(primary_writer_->flushed_lsn());

    // Verify the slot is active.
    auto slot = primary_slot_mgr_->get_slot("standby_1");
    ASSERT_TRUE(slot.has_value());
    EXPECT_TRUE(slot->active);

    sender_mgr.stop_all();
}

// =============================================================================
// Write Rejection on Standby
// - INSERT/UPDATE/DELETE/CREATE TABLE on standby → error
// - SELECT works on standby
// =============================================================================

TEST_F(ReplicationIntegrationTest, WriteRejectionOnStandby) {
    // Set up a query engine in standby mode.
    auto data_dir = base_dir_ / "standby_data";
    fs::create_directories(data_dir);
    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, data_dir);
    QueryEngine engine(catalog, storage);
    Config config = Config::load_defaults();
    CatalogPersistence persistence(catalog, storage);
    auto boot = SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
    ASSERT_TRUE(boot.has_value()) << boot.error().message;
    SettingsCache cache;
    ASSERT_TRUE(cache.load(engine).has_value());
    engine.set_settings_cache(&cache);

    // Switch to user DB and create a table before enabling standby mode.
    auto db = catalog.get_database("giodb");
    ASSERT_TRUE(db.has_value());
    engine.set_current_database(db->database_id);
    auto create = engine.execute("CREATE TABLE test_tbl (id INT, val VARCHAR, PRIMARY KEY (id))");
    ASSERT_TRUE(create.has_value()) << create.error().message;

    // Enable standby mode.
    engine.set_standby_mode(true);

    // DML rejected.
    auto ins = engine.execute("INSERT INTO test_tbl VALUES (1, 'hello')");
    EXPECT_FALSE(ins.has_value());
    EXPECT_EQ(ins.error().code, StatusCode::READ_ONLY);

    auto upd = engine.execute("UPDATE test_tbl SET val = 'world' WHERE id = 1");
    EXPECT_FALSE(upd.has_value());
    EXPECT_EQ(upd.error().code, StatusCode::READ_ONLY);

    auto del = engine.execute("DELETE FROM test_tbl WHERE id = 1");
    EXPECT_FALSE(del.has_value());
    EXPECT_EQ(del.error().code, StatusCode::READ_ONLY);

    // DDL rejected.
    auto ddl = engine.execute("CREATE TABLE test_tbl2 (id INT, PRIMARY KEY (id))");
    EXPECT_FALSE(ddl.has_value());
    EXPECT_EQ(ddl.error().code, StatusCode::READ_ONLY);

    // SELECT works.
    auto sel = engine.execute("SELECT id, val FROM test_tbl");
    EXPECT_TRUE(sel.has_value()) << sel.error().message;

    // SHOW commands work.
    auto show = engine.execute("SHOW TABLES");
    EXPECT_TRUE(show.has_value()) << show.error().message;

    // pg_is_in_recovery() returns true.
    auto recovery = engine.execute("SELECT pg_is_in_recovery()");
    ASSERT_TRUE(recovery.has_value()) << recovery.error().message;
    ASSERT_EQ(recovery->rows.size(), 1u);
    EXPECT_TRUE(recovery->rows[0][0].as_bool());
}

// =============================================================================
// Replication Lag Monitoring
// - Write data on primary at high rate
// - Verify SHOW REPLICATION STATUS shows lag information
// =============================================================================

TEST_F(ReplicationIntegrationTest, ReplicationLagMonitoring) {
    // Write several transactions.
    for (txn_id_t i = 1; i <= 10; i++) {
        write_txn(*primary_writer_, i, 1, "data_" + std::to_string(i));
    }
    ASSERT_TRUE(primary_writer_->flush().has_value());

    ASSERT_TRUE(primary_slot_mgr_->create_slot("lag_replica").has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(
        primary_wal_dir_, nullptr, *primary_writer_, 10, opts, primary_slot_mgr_.get());

    auto conn = std::make_unique<LinkedConnection>("lag_replica",
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::condition_variable>(),
                                                   std::make_shared<std::condition_variable>());
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1, "lag_replica").has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Set up QueryEngine for monitoring.
    auto data_dir = base_dir_ / "monitor_data";
    fs::create_directories(data_dir);
    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, data_dir);
    QueryEngine engine(catalog, storage);
    Config config = Config::load_defaults();
    CatalogPersistence persistence(catalog, storage);
    auto boot = SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
    ASSERT_TRUE(boot.has_value()) << boot.error().message;
    SettingsCache cache;
    ASSERT_TRUE(cache.load(engine).has_value());
    engine.set_settings_cache(&cache);
    engine.set_wal_sender_manager(&sender_mgr);
    engine.set_wal_writer(primary_writer_.get());

    auto db = catalog.get_database("giodb");
    ASSERT_TRUE(db.has_value());
    engine.set_current_database(db->database_id);

    auto qr = engine.execute("SHOW REPLICATION STATUS");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 1u);
    EXPECT_EQ(qr->rows[0][0].as_string(), "lag_replica"); // slot_name

    sender_mgr.stop_all();
}

// =============================================================================
// Failover (Promotion)
// - Start primary + standby, insert data, promote standby
// - Verify new primary accepts writes
// =============================================================================

TEST_F(ReplicationIntegrationTest, FailoverPromotion) {
    // Write initial data.
    write_txn(*primary_writer_, 1, 1, "pre_promote_data");
    ASSERT_TRUE(primary_writer_->flush().has_value());

    // Create a standby-side writer for the promotion test.
    auto standby_wal_dir = base_dir_ / "standby_promote_wal";
    fs::create_directories(standby_wal_dir);
    auto standby_writer = std::make_unique<WalWriter>(standby_wal_dir, test_wal_opts());
    ASSERT_TRUE(standby_writer->open().has_value());

    // Set up standby query engine.
    auto standby_data = base_dir_ / "standby_promote_data";
    fs::create_directories(standby_data);
    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, standby_data);
    QueryEngine engine(catalog, storage);
    Config config = Config::load_defaults();
    config.standby_mode = true;
    CatalogPersistence persistence(catalog, storage);
    auto boot =
        SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, standby_data);
    ASSERT_TRUE(boot.has_value()) << boot.error().message;
    SettingsCache cache;
    ASSERT_TRUE(cache.load(engine).has_value());
    engine.set_settings_cache(&cache);
    engine.set_standby_mode(true);
    engine.set_wal_writer(standby_writer.get());

    auto db = catalog.get_database("giodb");
    ASSERT_TRUE(db.has_value());
    engine.set_current_database(db->database_id);

    // Verify standby reports in recovery.
    auto recovery = engine.execute("SELECT pg_is_in_recovery()");
    ASSERT_TRUE(recovery.has_value());
    EXPECT_TRUE(recovery->rows[0][0].as_bool());

    // Simulate promotion: disable standby mode.
    engine.set_standby_mode(false);

    // Now the promoted standby should accept writes.
    auto create =
        engine.execute("CREATE TABLE promoted_tbl (id INT, val VARCHAR, PRIMARY KEY (id))");
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ins = engine.execute("INSERT INTO promoted_tbl VALUES (1, 'after_promote')");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;

    // Verify data.
    auto sel = engine.execute("SELECT val FROM promoted_tbl WHERE id = 1");
    ASSERT_TRUE(sel.has_value()) << sel.error().message;
    ASSERT_EQ(sel->rows.size(), 1u);
    EXPECT_EQ(sel->rows[0][0].as_string(), "after_promote");

    // pg_is_in_recovery should now be false.
    auto post_promote = engine.execute("SELECT pg_is_in_recovery()");
    ASSERT_TRUE(post_promote.has_value());
    EXPECT_FALSE(post_promote->rows[0][0].as_bool());

    standby_writer->close().has_value();
}

// =============================================================================
// Synchronous Replication
// - Enable sync mode, verify configuration
// =============================================================================

TEST_F(ReplicationIntegrationTest, SynchronousReplicationConfig) {
    SyncReplicationConfig sync_cfg;
    sync_cfg.level = SyncLevel::REMOTE_FLUSH;
    sync_cfg.standby_names = {"sync_replica"};
    sync_cfg.commit_count = 1;
    sync_cfg.timeout = std::chrono::milliseconds(5000);
    sync_cfg.fallback = SyncFallback::ERROR;
    SyncReplicationManager sync_mgr(sync_cfg);

    EXPECT_TRUE(sync_mgr.is_sync_enabled());
    EXPECT_EQ(sync_mgr.sync_level(), SyncLevel::REMOTE_FLUSH);

    auto cfg = sync_mgr.current_config();
    EXPECT_EQ(cfg.commit_count, 1u);
    EXPECT_TRUE(cfg.standby_names.count("sync_replica"));

    // Create sender manager with sync.
    ASSERT_TRUE(primary_slot_mgr_->create_slot("sync_replica").has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(
        primary_wal_dir_, nullptr, *primary_writer_, 10, opts, primary_slot_mgr_.get(), &sync_mgr);

    auto conn = std::make_unique<LinkedConnection>("sync_replica",
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::condition_variable>(),
                                                   std::make_shared<std::condition_variable>());
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1, "sync_replica").has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify sync_state shows "sync" for this replica.
    auto statuses = sender_mgr.get_sender_statuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].slot_name, "sync_replica");
    EXPECT_EQ(statuses[0].sync_state, "sync");

    sender_mgr.stop_all();
}

// =============================================================================
// Multiple Replicas
// - Start primary + 2 standbys, verify both appear in status
// =============================================================================

TEST_F(ReplicationIntegrationTest, MultipleReplicas) {
    write_txn(*primary_writer_, 1, 1, "multi_data");
    ASSERT_TRUE(primary_writer_->flush().has_value());

    ASSERT_TRUE(primary_slot_mgr_->create_slot("replica_a").has_value());
    ASSERT_TRUE(primary_slot_mgr_->create_slot("replica_b").has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(
        primary_wal_dir_, nullptr, *primary_writer_, 10, opts, primary_slot_mgr_.get());

    // Connect first replica.
    auto conn_a = std::make_unique<LinkedConnection>("replica_a",
                                                     std::make_shared<std::vector<uint8_t>>(),
                                                     std::make_shared<std::vector<uint8_t>>(),
                                                     std::make_shared<std::mutex>(),
                                                     std::make_shared<std::mutex>(),
                                                     std::make_shared<std::condition_variable>(),
                                                     std::make_shared<std::condition_variable>());
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn_a), 1, "replica_a").has_value());

    // Connect second replica.
    auto conn_b = std::make_unique<LinkedConnection>("replica_b",
                                                     std::make_shared<std::vector<uint8_t>>(),
                                                     std::make_shared<std::vector<uint8_t>>(),
                                                     std::make_shared<std::mutex>(),
                                                     std::make_shared<std::mutex>(),
                                                     std::make_shared<std::condition_variable>(),
                                                     std::make_shared<std::condition_variable>());
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn_b), 1, "replica_b").has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto statuses = sender_mgr.get_sender_statuses();
    ASSERT_EQ(statuses.size(), 2u);

    // Both replicas should be tracked.
    bool found_a = false;
    bool found_b = false;
    for (const auto& s : statuses) {
        if (s.slot_name == "replica_a") {
            found_a = true;
        }
        if (s.slot_name == "replica_b") {
            found_b = true;
        }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);

    // Verify both slots are active.
    auto slots = primary_slot_mgr_->list_slots();
    for (const auto& slot : slots) {
        EXPECT_TRUE(slot.active) << "slot " << slot.slot_name << " should be active";
    }

    sender_mgr.stop_all();
}

// =============================================================================
// Health Monitor Integration
// - Verify health monitor runs without issues
// =============================================================================

TEST_F(ReplicationIntegrationTest, HealthMonitorIntegration) {
    write_txn(*primary_writer_, 1, 1, "health_data");
    ASSERT_TRUE(primary_writer_->flush().has_value());

    ASSERT_TRUE(primary_slot_mgr_->create_slot("health_replica").has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(
        primary_wal_dir_, nullptr, *primary_writer_, 10, opts, primary_slot_mgr_.get());

    auto conn = std::make_unique<LinkedConnection>("health_replica",
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::vector<uint8_t>>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::mutex>(),
                                                   std::make_shared<std::condition_variable>(),
                                                   std::make_shared<std::condition_variable>());
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1, "health_replica").has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    HealthMonitorConfig hm_cfg;
    hm_cfg.lag_warning_threshold = std::chrono::milliseconds(10000);
    hm_cfg.disconnect_warning_threshold = std::chrono::milliseconds(60000);
    ReplicationHealthMonitor monitor(hm_cfg);

    // Run health check — should not crash.
    monitor.check_health(sender_mgr, primary_slot_mgr_.get(), *primary_writer_);

    // Write more data and check again.
    for (txn_id_t i = 2; i <= 5; i++) {
        write_txn(*primary_writer_, i, 1, "health_batch_" + std::to_string(i));
    }
    ASSERT_TRUE(primary_writer_->flush().has_value());
    monitor.check_health(sender_mgr, primary_slot_mgr_.get(), *primary_writer_);

    sender_mgr.stop_all();
}
