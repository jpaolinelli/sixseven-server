#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/server/replication_health_monitor.h"
#include "sixseven/server/replication_message.h"
#include "sixseven/server/replication_slot.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/server/wal_sender_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "test_catalog_helpers.h"
#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// Minimal ReplicationConnection for tests
// =============================================================================

namespace {

class TestReplicationConnection : public ReplicationConnection {
public:
    explicit TestReplicationConnection(std::string peer = "test-replica")
        : peer_(std::move(peer)) {}

    Result<void> send(std::span<const uint8_t> /*data*/) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t /*max_bytes*/,
                                         std::chrono::milliseconds /*timeout*/) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok(std::vector<uint8_t>{});
    }

    void close() override { open_.store(false); }
    bool is_open() const override { return open_.load(); }
    std::string peer_description() const override { return peer_; }

private:
    std::string peer_;
    std::atomic<bool> open_{true};
};

/// A ReplicationConnection that delivers one pre-baked StandbyStatusMessage on
/// the first receive() call and then returns empty buffers.  Used to drive an
/// applied_lsn into WalSenderManager so check_health() can evaluate the lag
/// comparison against a known threshold.
class StatusReportingConnection : public ReplicationConnection {
public:
    explicit StatusReportingConnection(std::string peer, lsn_t applied_lsn)
        : peer_(std::move(peer)) {
        StandbyStatusMessage msg;
        msg.received_lsn = applied_lsn;
        msg.flushed_lsn = applied_lsn;
        msg.applied_lsn = applied_lsn;
        msg.timestamp_us = 0;
        status_bytes_ = serialize_standby_status(msg);
    }

    Result<void> send(std::span<const uint8_t> /*data*/) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t /*max_bytes*/,
                                         std::chrono::milliseconds /*timeout*/) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        // Return the status message once, then empty.
        if (!delivered_.exchange(true)) {
            return ok(status_bytes_);
        }
        return ok(std::vector<uint8_t>{});
    }

    void close() override { open_.store(false); }
    bool is_open() const override { return open_.load(); }
    std::string peer_description() const override { return peer_; }

private:
    std::string peer_;
    std::vector<uint8_t> status_bytes_;
    std::atomic<bool> open_{true};
    std::atomic<bool> delivered_{false};
};

/// Null recovery handler for WalReceiver tests.
class NullRecoveryHandler : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord& /*record*/) override { return ok(); }
    Result<void> undo(const WalRecord& /*record*/) override { return ok(); }
};

} // namespace

// =============================================================================
// Test fixture with QueryEngine + replication infrastructure
// =============================================================================

class ReplicationMonitoringTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_repl_monitor";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        wal_dir_ = data_dir_ / "wal";
        std::filesystem::create_directories(wal_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();
        cache_ = std::make_unique<SettingsCache>();

        // Bootstrap system database.
        auto boot = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;
        auto load = cache_->load(*engine_);
        ASSERT_TRUE(load.has_value()) << load.error().message;
        engine_->set_settings_cache(cache_.get());

        // Set up WAL writer.
        writer_ = std::make_unique<WalWriter>(wal_dir_, test_wal_opts());
        ASSERT_TRUE(writer_->open().has_value());

        // Set up slot manager.
        slot_mgr_ = std::make_unique<ReplicationSlotManager>(data_dir_);

        // Wire up the engine.
        engine_->set_slot_manager(slot_mgr_.get());
        engine_->set_wal_writer(writer_.get());

        // Switch to user database.
        use_database("demo");
    }

    void TearDown() override {
        engine_.reset();
        cache_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        if (writer_) {
            writer_->close().has_value();
        }
        writer_.reset();
        slot_mgr_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    void use_database(const std::string& name) {
        auto db = catalog_->get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    std::filesystem::path data_dir_;
    std::filesystem::path wal_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<SettingsCache> cache_;
    std::unique_ptr<WalWriter> writer_;
    std::unique_ptr<ReplicationSlotManager> slot_mgr_;
    Config config_;
};

// =============================================================================
// SHOW REPLICATION STATUS
// =============================================================================

TEST_F(ReplicationMonitoringTest, ShowReplicationStatusNoManager) {
    // Without a sender manager, should return error.
    exec_error("SHOW REPLICATION STATUS", StatusCode::INTERNAL_ERROR);
}

TEST_F(ReplicationMonitoringTest, ShowReplicationStatusEmpty) {
    // Set up sender manager with no connected replicas.
    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(wal_dir_, nullptr, *writer_, 10, opts, slot_mgr_.get());
    engine_->set_wal_sender_manager(&sender_mgr);

    auto qr = exec_ok("SHOW REPLICATION STATUS");
    EXPECT_EQ(qr.column_names.size(), 11u);
    EXPECT_EQ(qr.column_names[0], "slot_name");
    EXPECT_EQ(qr.column_names[1], "client_addr");
    EXPECT_EQ(qr.column_names[2], "state");
    EXPECT_EQ(qr.column_names[3], "sent_lsn");
    EXPECT_EQ(qr.column_names[4], "write_lsn");
    EXPECT_EQ(qr.column_names[5], "flush_lsn");
    EXPECT_EQ(qr.column_names[6], "replay_lsn");
    EXPECT_EQ(qr.column_names[7], "write_lag_ms");
    EXPECT_EQ(qr.column_names[8], "flush_lag_ms");
    EXPECT_EQ(qr.column_names[9], "replay_lag_ms");
    EXPECT_EQ(qr.column_names[10], "sync_state");
    EXPECT_TRUE(qr.rows.empty());

    sender_mgr.stop_all();
}

TEST_F(ReplicationMonitoringTest, ShowReplicationStatusWithReplica) {
    // Write some WAL data first so there's a non-zero current_lsn.
    write_committed_txn(*writer_, 1, 1, "hello");
    ASSERT_TRUE(writer_->flush().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(wal_dir_, nullptr, *writer_, 10, opts, slot_mgr_.get());
    engine_->set_wal_sender_manager(&sender_mgr);

    // Create a slot and accept a connection.
    ASSERT_TRUE(slot_mgr_->create_slot("replica_1").has_value());
    auto conn = std::make_unique<TestReplicationConnection>("192.168.1.10:6767");
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1, "replica_1").has_value());

    // Give the sender thread a moment to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto qr = exec_ok("SHOW REPLICATION STATUS");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "replica_1");         // slot_name
    EXPECT_EQ(qr.rows[0][1].as_string(), "192.168.1.10:6767"); // client_addr
    // State should be catchup or streaming.
    std::string state = qr.rows[0][2].as_string();
    EXPECT_TRUE(state == "catchup" || state == "streaming" || state == "startup")
        << "got: " << state;
    EXPECT_EQ(qr.rows[0][10].as_string(), "async"); // sync_state

    sender_mgr.stop_all();
}

// =============================================================================
// SHOW STANDBY STATUS
// =============================================================================

TEST_F(ReplicationMonitoringTest, ShowStandbyStatusNoReceiver) {
    // Without a WAL receiver, should return error.
    exec_error("SHOW STANDBY STATUS", StatusCode::INTERNAL_ERROR);
}

TEST_F(ReplicationMonitoringTest, ShowStandbyStatusInitial) {
    // Create a WAL receiver that won't actually connect.
    NullRecoveryHandler handler;
    auto factory = [](const std::string& /*host*/,
                      uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        return make_error(StatusCode::NETWORK_ERROR, "test");
    };

    WalReceiverOptions recv_opts;
    recv_opts.retry_interval = std::chrono::milliseconds(50);
    recv_opts.receive_timeout = std::chrono::milliseconds(100);
    WalReceiver receiver(factory, wal_dir_, handler, recv_opts);
    engine_->set_wal_receiver(&receiver);

    auto qr = exec_ok("SHOW STANDBY STATUS");
    EXPECT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "received_lsn");
    EXPECT_EQ(qr.column_names[1], "applied_lsn");
    EXPECT_EQ(qr.column_names[2], "flushed_lsn");
    EXPECT_EQ(qr.column_names[3], "replication_lag_ms");
    EXPECT_EQ(qr.column_names[4], "is_streaming");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Initial state: not streaming, no LSNs.
    EXPECT_EQ(qr.rows[0][4].as_bool(), false); // is_streaming
}

// =============================================================================
// pg_current_wal_lsn()
// =============================================================================

TEST_F(ReplicationMonitoringTest, PgCurrentWalLsn) {
    // Write some WAL so we have a non-zero LSN.
    write_committed_txn(*writer_, 1, 1, "test");
    ASSERT_TRUE(writer_->flush().has_value());
    lsn_t expected_lsn = writer_->current_lsn();
    ASSERT_GT(expected_lsn, 0u);

    auto qr = exec_ok("SELECT pg_current_wal_lsn()");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), static_cast<int64_t>(expected_lsn));
}

// =============================================================================
// pg_is_in_recovery()
// =============================================================================

TEST_F(ReplicationMonitoringTest, PgIsInRecoveryFalseOnPrimary) {
    // Default is not standby mode.
    auto qr = exec_ok("SELECT pg_is_in_recovery()");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_bool(), false);
}

TEST_F(ReplicationMonitoringTest, PgIsInRecoveryTrueOnStandby) {
    engine_->set_standby_mode(true);
    auto qr = exec_ok("SELECT pg_is_in_recovery()");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_bool(), true);
    engine_->set_standby_mode(false);
}

// =============================================================================
// pg_last_wal_replay_lsn()
// =============================================================================

TEST_F(ReplicationMonitoringTest, PgLastWalReplayLsnNullWithoutReceiver) {
    // Without a WAL receiver, should return NULL.
    auto qr = exec_ok("SELECT pg_last_wal_replay_lsn()");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

// =============================================================================
// Parser tests for new SHOW variants
// =============================================================================

TEST_F(ReplicationMonitoringTest, ParseShowReplicationStatus) {
    // Verify the parser handles "SHOW REPLICATION STATUS" correctly.
    WalSenderOptions opts;
    WalSenderManager sender_mgr(wal_dir_, nullptr, *writer_, 10, opts);
    engine_->set_wal_sender_manager(&sender_mgr);

    auto qr = exec_ok("SHOW REPLICATION STATUS");
    EXPECT_EQ(qr.column_names[0], "slot_name");

    sender_mgr.stop_all();
}

TEST_F(ReplicationMonitoringTest, ParseShowStandbyStatus) {
    NullRecoveryHandler handler;
    auto factory = [](const std::string& /*host*/,
                      uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
        return make_error(StatusCode::NETWORK_ERROR, "test");
    };
    WalReceiver receiver(factory, wal_dir_, handler);
    engine_->set_wal_receiver(&receiver);

    auto qr = exec_ok("SHOW STANDBY STATUS");
    EXPECT_EQ(qr.column_names[0], "received_lsn");
}

// =============================================================================
// Health check config in settings
// =============================================================================

TEST_F(ReplicationMonitoringTest, HealthCheckSettingsExistInShowAll) {
    use_database(system_database_name);
    auto qr = exec_ok("SHOW ALL");

    bool found_lag = false;
    bool found_disconnect = false;
    for (const auto& row : qr.rows) {
        auto key = row[0].as_string();
        if (key == "replication.lag_warning_threshold_bytes") {
            found_lag = true;
            EXPECT_EQ(row[1].as_string(), "10000");
        }
        if (key == "replication.disconnect_warning_threshold_ms") {
            found_disconnect = true;
            EXPECT_EQ(row[1].as_string(), "60000");
        }
    }
    EXPECT_TRUE(found_lag);
    EXPECT_TRUE(found_disconnect);
}

// =============================================================================
// ReplicationHealthMonitor unit tests
// =============================================================================

TEST(ReplicationHealthMonitor, CheckHealthNoReplicas) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager sender_mgr(dir.path(), nullptr, writer, 10);
    ReplicationSlotManager slot_mgr(dir.path());

    HealthMonitorConfig cfg;
    cfg.lag_warning_threshold_bytes = 1000;
    cfg.disconnect_warning_threshold = std::chrono::milliseconds(500);

    ReplicationHealthMonitor monitor(cfg);
    // Should not crash with no replicas.
    monitor.check_health(sender_mgr, &slot_mgr, writer);

    sender_mgr.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

TEST(ReplicationHealthMonitor, ConfigureUpdatesThresholds) {
    HealthMonitorConfig cfg;
    cfg.lag_warning_threshold_bytes = 5000;
    cfg.disconnect_warning_threshold = std::chrono::milliseconds(30000);

    ReplicationHealthMonitor monitor(cfg);

    HealthMonitorConfig new_cfg;
    new_cfg.lag_warning_threshold_bytes = 2000;
    new_cfg.disconnect_warning_threshold = std::chrono::milliseconds(10000);
    monitor.configure(new_cfg);

    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold_bytes, 2000);
    EXPECT_EQ(got.disconnect_warning_threshold, std::chrono::milliseconds(10000));
}

TEST(ReplicationHealthMonitor, CheckHealthWithActiveReplica) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Write some WAL.
    WalRecord rec;
    rec.type = WalRecordType::BEGIN;
    rec.txn_id = 1;
    ASSERT_TRUE(writer.append(rec).has_value());
    ASSERT_TRUE(writer.flush().has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(100);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr(dir.path(), nullptr, writer, 10, opts);

    auto conn = std::make_unique<TestReplicationConnection>("test-replica");
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1).has_value());

    // Give sender thread time to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Threshold set very high so no lag warning fires for this active replica.
    HealthMonitorConfig cfg;
    cfg.lag_warning_threshold_bytes = INT64_MAX;
    cfg.disconnect_warning_threshold = std::chrono::milliseconds(60000);
    ReplicationHealthMonitor monitor(cfg);

    monitor.check_health(sender_mgr, nullptr, writer);
    // With an extremely high threshold no warning should fire.
    auto report = monitor.last_report();
    EXPECT_EQ(report.warning_count, 0u) << "unexpected lag warning with INT64_MAX threshold";

    sender_mgr.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// =============================================================================
// GDB-918: lag_warning_threshold_bytes boundary tests
//
// Mutation-grade: the warn decision (lag_bytes > threshold_bytes) is observable
// through HealthReport::warning_count.  These tests drive applied_lsn to a
// known value, set the threshold just above and just below the resulting lag,
// and assert that warning_count changes accordingly.
// =============================================================================

/// Helper: wait until the sender_mgr reports applied_lsn == target for
/// the first sender, or until the deadline.
static bool wait_for_applied_lsn(const WalSenderManager& mgr, lsn_t target) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto statuses = mgr.get_sender_statuses();
        if (!statuses.empty() && statuses[0].applied_lsn == target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// GDB-918: threshold just ABOVE the lag -- no warning fires.
TEST(ReplicationHealthMonitor, GDB918_LagBelowThresholdNoWarning) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Write several WAL records so current_lsn > 0.
    for (int i = 0; i < 5; ++i) {
        WalRecord rec;
        rec.type = WalRecordType::BEGIN;
        rec.txn_id = static_cast<txn_id_t>(i + 1);
        ASSERT_TRUE(writer.append(rec).has_value());
    }
    ASSERT_TRUE(writer.flush().has_value());

    lsn_t primary_lsn = writer.current_lsn();
    ASSERT_GT(primary_lsn, 0u);

    // Replica reports applied_lsn = 1 (behind the primary).
    lsn_t applied = 1;
    int64_t lag_bytes = static_cast<int64_t>(primary_lsn) - static_cast<int64_t>(applied);
    ASSERT_GT(lag_bytes, 0);

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr918a(dir.path(), nullptr, writer, 10, opts);

    auto conn = std::make_unique<StatusReportingConnection>("lag-test-replica", applied);
    ASSERT_TRUE(sender_mgr918a.accept_connection(std::move(conn), 1).has_value());

    // Wait for the sender to process the status message.
    ASSERT_TRUE(wait_for_applied_lsn(sender_mgr918a, applied))
        << "sender never processed applied_lsn=" << applied;

    // Threshold = lag_bytes + 1: lag is BELOW threshold, no warning.
    HealthMonitorConfig cfg_above;
    cfg_above.lag_warning_threshold_bytes = lag_bytes + 1;
    cfg_above.disconnect_warning_threshold = std::chrono::milliseconds(60000);
    ReplicationHealthMonitor monitor_above(cfg_above);

    monitor_above.check_health(sender_mgr918a, nullptr, writer);

    auto report_above = monitor_above.last_report();
    EXPECT_EQ(report_above.warning_count, 0u)
        << "no warning expected: lag=" << lag_bytes << " threshold=" << (lag_bytes + 1);

    sender_mgr918a.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

/// GDB-918: threshold just BELOW the lag -- warning fires exactly once.
TEST(ReplicationHealthMonitor, GDB918_LagAboveThresholdWarningFires) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Write several WAL records so current_lsn > 0.
    for (int i = 0; i < 5; ++i) {
        WalRecord rec;
        rec.type = WalRecordType::BEGIN;
        rec.txn_id = static_cast<txn_id_t>(i + 1);
        ASSERT_TRUE(writer.append(rec).has_value());
    }
    ASSERT_TRUE(writer.flush().has_value());

    lsn_t primary_lsn = writer.current_lsn();
    ASSERT_GT(primary_lsn, 0u);

    // Replica reports applied_lsn = 1 (behind the primary).
    lsn_t applied = 1;
    int64_t lag_bytes = static_cast<int64_t>(primary_lsn) - static_cast<int64_t>(applied);
    ASSERT_GT(lag_bytes, 0);

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    opts.sender_timeout = std::chrono::milliseconds(5000);
    WalSenderManager sender_mgr918b(dir.path(), nullptr, writer, 10, opts);

    auto conn = std::make_unique<StatusReportingConnection>("lag-test-replica", applied);
    ASSERT_TRUE(sender_mgr918b.accept_connection(std::move(conn), 1).has_value());

    // Wait for the sender to process the status message.
    ASSERT_TRUE(wait_for_applied_lsn(sender_mgr918b, applied))
        << "sender never processed applied_lsn=" << applied;

    // Threshold = lag_bytes - 1: lag is ABOVE threshold, warning fires.
    HealthMonitorConfig cfg_below;
    cfg_below.lag_warning_threshold_bytes = lag_bytes - 1;
    cfg_below.disconnect_warning_threshold = std::chrono::milliseconds(60000);
    ReplicationHealthMonitor monitor_below(cfg_below);

    monitor_below.check_health(sender_mgr918b, nullptr, writer);

    auto report_below = monitor_below.last_report();
    EXPECT_GT(report_below.warning_count, 0u)
        << "warning expected: lag=" << lag_bytes << " threshold=" << (lag_bytes - 1);
    // Verify the lag entry is present and matches.
    ASSERT_EQ(report_below.last_lag_bytes.count("lag-test-replica"), 1u)
        << "lag-test-replica absent from lag map";
    EXPECT_EQ(report_below.last_lag_bytes.at("lag-test-replica"), lag_bytes);

    sender_mgr918b.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}
