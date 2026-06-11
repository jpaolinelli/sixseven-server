#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/server/promotion_manager.h"
#include "sixseven/server/replication_connection.h"
#include "sixseven/server/replication_health_monitor.h"
#include "sixseven/server/replication_message.h"
#include "sixseven/server/replication_slot.h"
#include "sixseven/server/sync_replication.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/server/wal_sender_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sixseven;
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
        base_dir_ = fs::temp_directory_path() / "sixseven_test_repl_integration";
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
    auto db = catalog.get_database("demo");
    ASSERT_TRUE(db.has_value());
    engine.set_current_database(db->database_id);
    auto create = engine.execute("CREATE TABLE test_tbl (id INT, val VARCHAR, PRIMARY KEY (id))");
    ASSERT_TRUE(create.has_value()) << create.error().message;

    // Enable standby mode.
    engine.set_standby_mode(true);

    // DML rejected.
    auto ins = engine.execute("INSERT INTO test_tbl VALUES (1, 'hello')");
    ASSERT_FALSE(ins.has_value());
    EXPECT_EQ(ins.error().code, StatusCode::READ_ONLY);

    auto upd = engine.execute("UPDATE test_tbl SET val = 'world' WHERE id = 1");
    ASSERT_FALSE(upd.has_value());
    EXPECT_EQ(upd.error().code, StatusCode::READ_ONLY);

    auto del = engine.execute("DELETE FROM test_tbl WHERE id = 1");
    ASSERT_FALSE(del.has_value());
    EXPECT_EQ(del.error().code, StatusCode::READ_ONLY);

    // DDL rejected.
    auto ddl = engine.execute("CREATE TABLE test_tbl2 (id INT, PRIMARY KEY (id))");
    ASSERT_FALSE(ddl.has_value());
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
    // Write several transactions (3 WAL records each) to create lag.
    for (txn_id_t i = 1; i <= 10; i++) {
        write_txn(*primary_writer_, i, 1, "data_" + std::to_string(i));
    }
    ASSERT_TRUE(primary_writer_->flush().has_value());

    ASSERT_TRUE(primary_slot_mgr_->create_slot("lag_replica").has_value());

    WalSenderOptions opts;
    opts.keepalive_interval = std::chrono::milliseconds(50);
    // Generous timeout: the replica in this test only acknowledges when the
    // test injects a status message, so the sender must not disconnect it.
    opts.sender_timeout = std::chrono::milliseconds(60000);
    WalSenderManager sender_mgr(
        primary_wal_dir_, nullptr, *primary_writer_, 10, opts, primary_slot_mgr_.get());

    // Keep handles to the replica->primary direction so the test can inject
    // standby status messages later.
    auto to_replica_buf = std::make_shared<std::vector<uint8_t>>();
    auto to_primary_buf = std::make_shared<std::vector<uint8_t>>();
    auto to_replica_mu = std::make_shared<std::mutex>();
    auto to_primary_mu = std::make_shared<std::mutex>();
    auto to_replica_cv = std::make_shared<std::condition_variable>();
    auto to_primary_cv = std::make_shared<std::condition_variable>();
    auto conn = std::make_unique<LinkedConnection>("lag_replica",
                                                   to_replica_buf,
                                                   to_primary_buf,
                                                   to_replica_mu,
                                                   to_primary_mu,
                                                   to_replica_cv,
                                                   to_primary_cv);
    ASSERT_TRUE(sender_mgr.accept_connection(std::move(conn), 1, "lag_replica").has_value());

    // Bounded wait helper — avoids fixed-sleep timing assumptions.
    auto wait_until = [](auto&& pred) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    };

    // Wait until the sender has caught up and switched to real-time streaming.
    ASSERT_TRUE(wait_until([&] {
        auto statuses = sender_mgr.get_sender_statuses();
        return statuses.size() == 1 && statuses[0].state == WalSender::State::STREAMING;
    })) << "sender never reached streaming state";

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

    auto db = catalog.get_database("demo");
    ASSERT_TRUE(db.has_value());
    engine.set_current_database(db->database_id);

    auto qr = engine.execute("SHOW REPLICATION STATUS");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;

    // The lag/LSN reporting columns must all be present.
    const std::vector<std::string> expected_columns = {"slot_name",
                                                       "client_addr",
                                                       "state",
                                                       "sent_lsn",
                                                       "write_lsn",
                                                       "flush_lsn",
                                                       "replay_lsn",
                                                       "write_lag_ms",
                                                       "flush_lag_ms",
                                                       "replay_lag_ms",
                                                       "sync_state"};
    EXPECT_EQ(qr->column_names, expected_columns);

    ASSERT_EQ(qr->rows.size(), 1u);
    const auto& row = qr->rows[0];
    ASSERT_EQ(row.size(), expected_columns.size());
    EXPECT_EQ(row[0].as_string(), "lag_replica"); // slot_name
    EXPECT_EQ(row[2].as_string(), "streaming");   // state (waited for above)

    // Catch-up sent every flushed record, so sent_lsn equals the primary's
    // flushed LSN — and is positive, since 10 transactions were written.
    ASSERT_FALSE(row[3].is_null());
    EXPECT_EQ(row[3].as_int64(), static_cast<int64_t>(primary_writer_->flushed_lsn()));
    EXPECT_GT(row[3].as_int64(), 0);

    // The replica has not acknowledged anything: its positions are unknown
    // (NULL) and every lag column reports the -1 "unknown" sentinel.
    EXPECT_TRUE(row[4].is_null()); // write_lsn
    EXPECT_TRUE(row[5].is_null()); // flush_lsn
    EXPECT_TRUE(row[6].is_null()); // replay_lsn
    ASSERT_FALSE(row[7].is_null());
    EXPECT_EQ(row[7].as_int64(), -1); // write_lag_ms
    ASSERT_FALSE(row[8].is_null());
    EXPECT_EQ(row[8].as_int64(), -1); // flush_lag_ms
    ASSERT_FALSE(row[9].is_null());
    EXPECT_EQ(row[9].as_int64(), -1);        // replay_lag_ms
    EXPECT_EQ(row[10].as_string(), "async"); // sync_state

    // Inject a standby status acknowledging only the first few records. The
    // replica is far behind the 30 records written, so lag must be positive.
    StandbyStatusMessage status;
    status.received_lsn = 3;
    status.flushed_lsn = 2;
    status.applied_lsn = 1;
    status.timestamp_us = 1;
    auto status_bytes = serialize_standby_status(status);
    {
        std::lock_guard lock(*to_primary_mu);
        to_primary_buf->insert(to_primary_buf->end(), status_bytes.begin(), status_bytes.end());
    }
    to_primary_cv->notify_all();

    // Wait until the sender has consumed the status update.
    ASSERT_TRUE(wait_until([&] {
        auto statuses = sender_mgr.get_sender_statuses();
        return statuses.size() == 1 && statuses[0].flushed_lsn != invalid_lsn;
    })) << "sender never processed the standby status update";

    auto qr2 = engine.execute("SHOW REPLICATION STATUS");
    ASSERT_TRUE(qr2.has_value()) << qr2.error().message;
    ASSERT_EQ(qr2->rows.size(), 1u);
    const auto& row2 = qr2->rows[0];
    ASSERT_EQ(row2.size(), expected_columns.size());

    // Replica positions now reflect the acknowledged LSNs.
    ASSERT_FALSE(row2[4].is_null());
    EXPECT_EQ(row2[4].as_int64(), 3); // write_lsn = received_lsn
    ASSERT_FALSE(row2[5].is_null());
    EXPECT_EQ(row2[5].as_int64(), 2); // flush_lsn
    ASSERT_FALSE(row2[6].is_null());
    EXPECT_EQ(row2[6].as_int64(), 1); // replay_lsn

    // Lag is the distance from the primary's current LSN to each acknowledged
    // position. Nothing writes WAL after the setup above, so it is exact.
    const auto current_lsn = static_cast<int64_t>(primary_writer_->current_lsn());
    ASSERT_FALSE(row2[7].is_null());
    EXPECT_EQ(row2[7].as_int64(), current_lsn - 3); // write_lag_ms
    EXPECT_GT(row2[7].as_int64(), 0);
    ASSERT_FALSE(row2[8].is_null());
    EXPECT_EQ(row2[8].as_int64(), current_lsn - 2); // flush_lag_ms
    EXPECT_GT(row2[8].as_int64(), 0);
    ASSERT_FALSE(row2[9].is_null());
    EXPECT_EQ(row2[9].as_int64(), current_lsn - 1); // replay_lag_ms
    EXPECT_GT(row2[9].as_int64(), 0);

    sender_mgr.stop_all();
}

// =============================================================================
// Failover (Promotion)
// - Start primary + standby, insert data, promote standby
// - Verify new primary accepts writes
// =============================================================================

TEST_F(ReplicationIntegrationTest, FailoverPromotion) {
    // --- Set up standby-side WAL writer ---
    auto standby_wal_dir = base_dir_ / "standby_promote_wal";
    fs::create_directories(standby_wal_dir);
    auto standby_writer = std::make_unique<WalWriter>(standby_wal_dir, test_wal_opts());
    ASSERT_TRUE(standby_writer->open().has_value());

    // --- Set up standby query engine in standby mode ---
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

    auto db = catalog.get_database("demo");
    ASSERT_TRUE(db.has_value());
    engine.set_current_database(db->database_id);

    // Confirm the engine is in standby (recovery) mode before promotion.
    auto recovery = engine.execute("SELECT pg_is_in_recovery()");
    ASSERT_TRUE(recovery.has_value());
    ASSERT_EQ(recovery->rows.size(), 1u);
    EXPECT_TRUE(recovery->rows[0][0].as_bool());

    // Writes must be rejected while in standby mode.
    auto rejected = engine.execute("CREATE TABLE should_fail (id INT)");
    EXPECT_FALSE(rejected.has_value()) << "standby should reject DDL before promotion";

    // --- Drive promotion through PromotionManager ---
    // receiver = nullptr: no live WAL receiver to stop (offline failover scenario).
    PromotionManager promo_mgr(config, /*receiver=*/nullptr, *standby_writer, dm, standby_wal_dir);

    // Timeline starts at 1 and must report standby mode.
    EXPECT_EQ(promo_mgr.timeline_id(), 1u);
    EXPECT_TRUE(promo_mgr.is_standby());

    // Wire the promotion callback: flip the query engine out of standby.
    bool callback_invoked = false;
    promo_mgr.set_on_promoted([&] {
        engine.set_standby_mode(false);
        callback_invoked = true;
    });

    // Promote — stops any receiver, writes a PROMOTE WAL record, increments
    // the timeline, flips config_.standby_mode, and invokes the callback.
    auto promote_result = promo_mgr.promote();
    ASSERT_TRUE(promote_result.has_value()) << promote_result.error().message;

    // --- Verify PromotionManager state after promotion ---
    EXPECT_TRUE(callback_invoked) << "on-promoted callback was not invoked";
    EXPECT_FALSE(promo_mgr.is_standby())
        << "PromotionManager still reports standby after promotion";
    EXPECT_EQ(promo_mgr.timeline_id(), 2u) << "timeline ID was not incremented by promotion";

    // The PROMOTE record must be in the WAL — flushed_lsn advances beyond 0.
    EXPECT_GT(standby_writer->flushed_lsn(), lsn_t{0})
        << "PROMOTE WAL record was not flushed to the standby WAL";

    // Promoting a second time must be rejected (already primary).
    auto second_promote = promo_mgr.promote();
    EXPECT_FALSE(second_promote.has_value()) << "double-promote should return an error";

    // --- Verify the engine now accepts writes ---
    // pg_is_in_recovery() must return false on the new primary.
    auto post_promote = engine.execute("SELECT pg_is_in_recovery()");
    ASSERT_TRUE(post_promote.has_value());
    ASSERT_EQ(post_promote->rows.size(), 1u);
    EXPECT_FALSE(post_promote->rows[0][0].as_bool())
        << "engine still reports in_recovery after promotion";

    auto create =
        engine.execute("CREATE TABLE promoted_tbl (id INT, val VARCHAR, PRIMARY KEY (id))");
    ASSERT_TRUE(create.has_value()) << create.error().message;

    auto ins = engine.execute("INSERT INTO promoted_tbl VALUES (1, 'after_promote')");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;

    auto sel = engine.execute("SELECT val FROM promoted_tbl WHERE id = 1");
    ASSERT_TRUE(sel.has_value()) << sel.error().message;
    ASSERT_EQ(sel->rows.size(), 1u);
    EXPECT_EQ(sel->rows[0][0].as_string(), "after_promote");

    // --- Timeline must be persisted to disk ---
    auto tl_path = standby_wal_dir / "timeline_id";
    EXPECT_TRUE(fs::exists(tl_path)) << "timeline_id file not written by PromotionManager";
    if (fs::exists(tl_path)) {
        std::ifstream tl_file(tl_path);
        uint64_t persisted_tl = 0;
        tl_file >> persisted_tl;
        EXPECT_EQ(persisted_tl, 2u) << "persisted timeline_id does not match promoted timeline";
    }

    static_cast<void>(standby_writer->close());
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
