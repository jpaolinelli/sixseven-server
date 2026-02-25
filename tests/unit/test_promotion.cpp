#include "giodb/common/config.h"
#include "giodb/server/promotion_manager.h"
#include "giodb/server/replication_connection.h"
#include "giodb/server/replication_message.h"
#include "giodb/server/wal_receiver.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/wal.h"
#include "giodb/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "test_wal_helpers.h"

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// Mock/helper classes
// =============================================================================

namespace {

/// Mock recovery handler that tracks redo calls.
class MockRecoveryHandler : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord& /*record*/) override { return ok(); }
    Result<void> undo(const WalRecord& /*record*/) override { return ok(); }
};

/// Minimal in-memory replication connection for testing.
class MockReplicationConnection : public ReplicationConnection {
public:
    Result<void> send(std::span<const uint8_t> /*data*/) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok();
    }

    Result<std::vector<uint8_t>> receive(size_t /*max_bytes*/,
                                         std::chrono::milliseconds timeout) override {
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        // Block until closed or timeout — simulates waiting for data.
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, timeout, [this] { return !open_.load(); });
        if (!open_.load()) {
            return make_error(StatusCode::NETWORK_ERROR, "closed");
        }
        return ok(std::vector<uint8_t>{});
    }

    void close() override {
        open_.store(false);
        cv_.notify_all();
    }

    bool is_open() const override { return open_.load(); }
    std::string peer_description() const override { return "mock"; }

private:
    std::atomic<bool> open_{true};
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace

// =============================================================================
// Test fixture
// =============================================================================

class PromotionTest : public ::testing::Test {
protected:
    void SetUp() override {
        wal_dir_ = std::make_unique<TempWalDir>();
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_test_promotion_data";
        std::filesystem::create_directories(data_dir_);

        config_ = Config::load_defaults();
        config_.standby_mode = true;

        handler_ = std::make_unique<MockRecoveryHandler>();

        // Set up WAL writer (used by PromotionManager to write PROMOTE record).
        wal_writer_ = std::make_unique<WalWriter>(wal_dir_->path(), test_wal_opts());
        auto open_result = wal_writer_->open();
        ASSERT_TRUE(open_result.has_value()) << open_result.error().message;
    }

    void TearDown() override {
        if (receiver_) {
            receiver_->stop();
            receiver_.reset();
        }
        if (wal_writer_) {
            auto close_result = wal_writer_->close();
            (void)close_result;
            wal_writer_.reset();
        }
        handler_.reset();
        wal_dir_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Create and start a WalReceiver with a mock connection.
    void create_receiver() {
        auto factory = [](const std::string& /*host*/,
                          uint16_t /*port*/) -> Result<std::unique_ptr<ReplicationConnection>> {
            return ok(std::unique_ptr<ReplicationConnection>(
                std::make_unique<MockReplicationConnection>()));
        };

        WalReceiverOptions opts;
        opts.receive_timeout = std::chrono::milliseconds(100);
        opts.retry_interval = std::chrono::milliseconds(100);
        receiver_ = std::make_unique<WalReceiver>(factory, wal_dir_->path(), *handler_, opts);

        auto start_result = receiver_->start("localhost", 6767);
        ASSERT_TRUE(start_result.has_value()) << start_result.error().message;

        // Give the receiver time to connect.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    Config config_;
    std::unique_ptr<TempWalDir> wal_dir_;
    std::filesystem::path data_dir_;
    std::unique_ptr<MockRecoveryHandler> handler_;
    std::unique_ptr<WalWriter> wal_writer_;
    std::unique_ptr<WalReceiver> receiver_;
    DiskManager disk_mgr_;
};

// =============================================================================
// Promotion flow tests
// =============================================================================

TEST_F(PromotionTest, PromoteStandbyToPrimary) {
    create_receiver();

    PromotionManager pm(config_, receiver_.get(), *wal_writer_, disk_mgr_, wal_dir_->path());

    EXPECT_TRUE(pm.is_standby());
    EXPECT_EQ(pm.timeline_id(), 1u);

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_FALSE(pm.is_standby());
    EXPECT_FALSE(config_.standby_mode);
    EXPECT_EQ(pm.timeline_id(), 2u);
}

TEST_F(PromotionTest, PromoteWithoutReceiver) {
    // Promotion should work even if receiver is nullptr (not started).
    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    EXPECT_TRUE(pm.is_standby());

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_FALSE(pm.is_standby());
    EXPECT_EQ(pm.timeline_id(), 2u);
}

TEST_F(PromotionTest, CannotPromoteIfAlreadyPrimary) {
    config_.standby_mode = false;

    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    auto result = pm.promote();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::REPLICATION_ERROR);
    EXPECT_NE(result.error().message.find("already primary"), std::string::npos);
}

TEST_F(PromotionTest, CannotPromoteTwice) {
    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Second promote should fail — server is now primary.
    auto result2 = pm.promote();
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::REPLICATION_ERROR);
    EXPECT_NE(result2.error().message.find("already primary"), std::string::npos);
}

// =============================================================================
// Timeline management tests
// =============================================================================

TEST_F(PromotionTest, TimelineIncrements) {
    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    EXPECT_EQ(pm.timeline_id(), 1u);

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(pm.timeline_id(), 2u);
}

TEST_F(PromotionTest, TimelinePersistence) {
    {
        PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

        auto result = pm.promote();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(pm.timeline_id(), 2u);
    }

    // Verify the timeline file exists.
    auto timeline_file = wal_dir_->path() / "timeline_id";
    ASSERT_TRUE(std::filesystem::exists(timeline_file));

    // Load timeline into a new PromotionManager.
    Config config2 = Config::load_defaults();
    config2.standby_mode = true;
    PromotionManager pm2(config2, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    auto load_result = pm2.load_timeline();
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;
    EXPECT_EQ(pm2.timeline_id(), 2u);
}

TEST_F(PromotionTest, LoadTimelineDefaultsToOne) {
    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    // No timeline file exists — should default to 1.
    auto load_result = pm.load_timeline();
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;
    EXPECT_EQ(pm.timeline_id(), 1u);
}

// =============================================================================
// PROMOTE WAL record tests
// =============================================================================

TEST_F(PromotionTest, WritesPromoteWalRecord) {
    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Close the writer and read back the WAL to find the PROMOTE record.
    auto close_result = wal_writer_->close();
    ASSERT_TRUE(close_result.has_value()) << close_result.error().message;

    WalReader reader(wal_dir_->path());
    auto open_result = reader.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    // Verify the segment file exists.
    auto seg_path = wal_dir_->path() / "wal_000001";
    ASSERT_TRUE(std::filesystem::exists(seg_path))
        << "WAL segment file not found: " << seg_path.string();
    auto seg_size = std::filesystem::file_size(seg_path);
    ASSERT_GT(seg_size, 0u) << "WAL segment file is empty";

    bool found_promote = false;
    int record_count = 0;
    while (true) {
        auto record = reader.next();
        if (!record) {
            break;
        }
        ++record_count;
        if (record->type == WalRecordType::PROMOTE) {
            found_promote = true;

            // Verify the record payload contains the new timeline ID.
            auto timeline_result = PromotionManager::deserialize_promote_data(record->data);
            ASSERT_TRUE(timeline_result.has_value()) << timeline_result.error().message;
            EXPECT_EQ(*timeline_result, 2u);
            break;
        }
    }

    EXPECT_TRUE(found_promote) << "PROMOTE WAL record not found; total records read: "
                               << record_count << ", segment size: " << seg_size;
    (void)reader.close();
}

TEST_F(PromotionTest, SerializeDeserializePromoteData) {
    auto data = PromotionManager::serialize_promote_data(42);
    ASSERT_EQ(data.size(), sizeof(uint64_t));

    auto result = PromotionManager::deserialize_promote_data(data);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 42u);
}

TEST_F(PromotionTest, DeserializePromoteDataTooShort) {
    std::vector<uint8_t> short_data = {0x01, 0x02};
    auto result = PromotionManager::deserialize_promote_data(short_data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// =============================================================================
// Guard rail tests
// =============================================================================

TEST_F(PromotionTest, PromoteMaxLagBytesDefault) {
    // Default is 0 (no lag limit) — promotion should succeed even with lag.
    EXPECT_EQ(config_.replication_promote_max_lag_bytes, 0);

    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());
    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// =============================================================================
// Config tests
// =============================================================================

TEST(PromotionConfigTest, DefaultPromoteMaxLagBytes) {
    Config config = Config::load_defaults();
    EXPECT_EQ(config.replication_promote_max_lag_bytes, 0);
}

TEST(PromotionConfigTest, ApplySettingPromoteMaxLagBytes) {
    Config config = Config::load_defaults();

    config.apply_setting("replication.promote_max_lag_bytes", "1048576");
    EXPECT_EQ(config.replication_promote_max_lag_bytes, 1048576);
}

TEST(PromotionConfigTest, LoadFromJsonWithPromoteMaxLagBytes) {
    auto tmp = std::filesystem::temp_directory_path() / "giodb_test_promotion_config.json";

    {
        std::ofstream f(tmp);
        f << R"({
            "server": {
                "mode": "standby"
            },
            "replication": {
                "promote_max_lag_bytes": 2097152,
                "primary_host": "10.0.0.1",
                "primary_port": 5433
            }
        })";
    }

    auto result = Config::load_from_file(tmp.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(result->standby_mode);
    EXPECT_EQ(result->replication_promote_max_lag_bytes, 2097152);

    std::filesystem::remove(tmp);
}

// =============================================================================
// DiskManager reopen_file_readwrite tests
// =============================================================================

TEST(DiskManagerReopenTest, ReopenReadonlyFileAsReadWrite) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "giodb_test_dm_reopen";
    std::filesystem::create_directories(tmp_dir);
    auto file_path = tmp_dir / "reopen_test.db";

    DiskManager dm;

    // Create a file and write some data.
    auto create_result = dm.create_file(file_path);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
    auto write_id = *create_result;

    auto alloc_result = dm.allocate_page(write_id);
    ASSERT_TRUE(alloc_result.has_value());
    auto page_id = *alloc_result;

    Page page(page_id, PageType::DATA);
    page.raw()[page_header_size] = 0xDE;
    page.raw()[page_header_size + 1] = 0xAD;
    auto write_result = dm.write_page(write_id, page_id, page);
    ASSERT_TRUE(write_result.has_value());

    auto close_result = dm.close_file(write_id);
    ASSERT_TRUE(close_result.has_value());

    // Open as read-only.
    auto ro_result = dm.open_file_readonly(file_path);
    ASSERT_TRUE(ro_result.has_value()) << ro_result.error().message;
    auto ro_id = *ro_result;

    // Verify writes fail on read-only file.
    Page write_attempt(page_id, PageType::DATA);
    auto write_fail = dm.write_page(ro_id, page_id, write_attempt);
    ASSERT_FALSE(write_fail.has_value());
    EXPECT_EQ(write_fail.error().code, StatusCode::READ_ONLY);

    // Reopen in read/write mode.
    auto reopen_result = dm.reopen_file_readwrite(ro_id);
    ASSERT_TRUE(reopen_result.has_value()) << reopen_result.error().message;

    // Reads should still work.
    Page read_page(page_id, PageType::DATA);
    auto read_result = dm.read_page(ro_id, page_id, read_page);
    ASSERT_TRUE(read_result.has_value()) << read_result.error().message;
    EXPECT_EQ(read_page.raw()[page_header_size], 0xDE);
    EXPECT_EQ(read_page.raw()[page_header_size + 1], 0xAD);

    // Writes should now succeed.
    Page new_page(page_id, PageType::DATA);
    new_page.raw()[page_header_size] = 0xBE;
    new_page.raw()[page_header_size + 1] = 0xEF;
    auto write_ok = dm.write_page(ro_id, page_id, new_page);
    ASSERT_TRUE(write_ok.has_value()) << write_ok.error().message;

    // Verify the written data.
    Page verify_page(page_id, PageType::DATA);
    auto verify_result = dm.read_page(ro_id, page_id, verify_page);
    ASSERT_TRUE(verify_result.has_value()) << verify_result.error().message;
    EXPECT_EQ(verify_page.raw()[page_header_size], 0xBE);
    EXPECT_EQ(verify_page.raw()[page_header_size + 1], 0xEF);

    (void)dm.close_file(ro_id);
    std::filesystem::remove_all(tmp_dir);
}

TEST(DiskManagerReopenTest, ReopenAlreadyWritableFileErrors) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "giodb_test_dm_reopen_err";
    std::filesystem::create_directories(tmp_dir);
    auto file_path = tmp_dir / "already_rw.db";

    DiskManager dm;

    auto create_result = dm.create_file(file_path);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
    auto file_id = *create_result;

    // Trying to reopen an already writable file should fail.
    auto reopen_result = dm.reopen_file_readwrite(file_id);
    ASSERT_FALSE(reopen_result.has_value());
    EXPECT_EQ(reopen_result.error().code, StatusCode::INVALID_ARGUMENT);

    (void)dm.close_file(file_id);
    std::filesystem::remove_all(tmp_dir);
}

// =============================================================================
// Promotion callback test
// =============================================================================

TEST_F(PromotionTest, InvokesOnPromotedCallback) {
    bool callback_invoked = false;

    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());
    pm.set_on_promoted([&callback_invoked] { callback_invoked = true; });

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(callback_invoked);
}

TEST_F(PromotionTest, ReopensReadonlyFiles) {
    // Create a data file and open it read-only.
    auto file_path = data_dir_ / "test_table.db";
    auto create_result = disk_mgr_.create_file(file_path);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
    auto write_id = *create_result;

    // Allocate and write a page.
    auto alloc_result = disk_mgr_.allocate_page(write_id);
    ASSERT_TRUE(alloc_result.has_value());
    auto page_id = *alloc_result;

    Page page(page_id, PageType::DATA);
    page.raw()[page_header_size] = 0x42;
    auto write_result = disk_mgr_.write_page(write_id, page_id, page);
    ASSERT_TRUE(write_result.has_value());

    auto close_result = disk_mgr_.close_file(write_id);
    ASSERT_TRUE(close_result.has_value());

    // Open as read-only (simulating standby mode).
    auto ro_result = disk_mgr_.open_file_readonly(file_path);
    ASSERT_TRUE(ro_result.has_value()) << ro_result.error().message;
    auto ro_id = *ro_result;

    // Verify writes fail.
    Page write_page(page_id, PageType::DATA);
    auto write_fail = disk_mgr_.write_page(ro_id, page_id, write_page);
    ASSERT_FALSE(write_fail.has_value());

    // Set up promotion manager with the read-only file.
    PromotionManager pm(config_, nullptr, *wal_writer_, disk_mgr_, wal_dir_->path());
    pm.set_readonly_file_ids({ro_id});

    auto promote_result = pm.promote();
    ASSERT_TRUE(promote_result.has_value()) << promote_result.error().message;

    // After promotion, writes should succeed.
    Page new_page(page_id, PageType::DATA);
    new_page.raw()[page_header_size] = 0x99;
    auto write_ok = disk_mgr_.write_page(ro_id, page_id, new_page);
    ASSERT_TRUE(write_ok.has_value()) << write_ok.error().message;

    (void)disk_mgr_.close_file(ro_id);
}

// =============================================================================
// WAL receiver stop during promotion
// =============================================================================

TEST_F(PromotionTest, StopsWalReceiverDuringPromotion) {
    create_receiver();

    auto state = receiver_->get_state();
    EXPECT_TRUE(state.is_streaming);

    PromotionManager pm(config_, receiver_.get(), *wal_writer_, disk_mgr_, wal_dir_->path());

    auto result = pm.promote();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Receiver should be stopped after promotion.
    state = receiver_->get_state();
    EXPECT_FALSE(state.is_streaming);
}
