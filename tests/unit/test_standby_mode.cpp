#include "giodb/catalog/catalog.h"
#include "giodb/common/config.h"
#include "giodb/common/types.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/page.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace giodb;

// =============================================================================
// Test fixture
// =============================================================================

class StandbyModeTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_test_standby";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Create a table before enabling standby mode.
        auto result = engine_->execute("CREATE TABLE users (id INT, name VARCHAR, age INT)");
        ASSERT_TRUE(result.has_value()) << result.error().message;

        // Insert some data.
        result = engine_->execute("INSERT INTO users VALUES (1, 'alice', 30)");
        ASSERT_TRUE(result.has_value()) << result.error().message;

        // Now enable standby mode.
        engine_->set_standby_mode(true);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Write rejection tests
// =============================================================================

TEST_F(StandbyModeTest, RejectsInsert) {
    auto result = engine_->execute("INSERT INTO users VALUES (2, 'bob', 25)");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DML"), std::string::npos);
    EXPECT_NE(result.error().message.find("read-only standby"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsUpdate) {
    auto result = engine_->execute("UPDATE users SET age = 31 WHERE id = 1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DML"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsDelete) {
    auto result = engine_->execute("DELETE FROM users WHERE id = 1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DML"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsCreateTable) {
    auto result = engine_->execute("CREATE TABLE orders (id INT, total FLOAT)");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DDL"), std::string::npos);
    EXPECT_NE(result.error().message.find("read-only standby"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsDropTable) {
    auto result = engine_->execute("DROP TABLE users");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DDL"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsCreateDatabase) {
    auto result = engine_->execute("CREATE DATABASE testdb");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DDL"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsDropDatabase) {
    auto result = engine_->execute("DROP DATABASE testdb");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("DDL"), std::string::npos);
}

TEST_F(StandbyModeTest, RejectsSet) {
    auto result = engine_->execute("SET server.port = 5433");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
    EXPECT_NE(result.error().message.find("settings"), std::string::npos);
}

TEST_F(StandbyModeTest, AllowsSelect) {
    auto result = engine_->execute("SELECT * FROM users");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 1);
    EXPECT_EQ(result->rows[0][1].as_string(), "alice");
}

TEST_F(StandbyModeTest, AllowsShowTables) {
    auto result = engine_->execute("SHOW TABLES");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 1);
    EXPECT_EQ(result->rows[0][0].as_string(), "users");
}

TEST_F(StandbyModeTest, AllowsShowColumns) {
    auto result = engine_->execute("SHOW COLUMNS FROM users");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 3); // id, name, age
}

TEST_F(StandbyModeTest, StandbyModeToggle) {
    // Currently in standby mode — writes should fail.
    auto result = engine_->execute("INSERT INTO users VALUES (2, 'bob', 25)");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);

    // Disable standby mode (simulating promotion).
    engine_->set_standby_mode(false);
    EXPECT_FALSE(engine_->is_standby_mode());

    // Writes should now succeed.
    result = engine_->execute("INSERT INTO users VALUES (2, 'bob', 25)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->affected_rows, 1);
}

// =============================================================================
// Config tests
// =============================================================================

TEST(StandbyConfigTest, DefaultsToNonStandby) {
    Config config = Config::load_defaults();
    EXPECT_FALSE(config.standby_mode);
    EXPECT_TRUE(config.replication_primary_host.empty());
    EXPECT_EQ(config.replication_primary_port, 5432);
    EXPECT_EQ(config.replication_retry_interval_ms, 5000);
    EXPECT_EQ(config.replication_max_retry_interval_ms, 60000);
}

TEST(StandbyConfigTest, ApplySettingServerMode) {
    Config config = Config::load_defaults();
    EXPECT_FALSE(config.standby_mode);

    config.apply_setting("server.mode", "standby");
    EXPECT_TRUE(config.standby_mode);

    config.apply_setting("server.mode", "primary");
    EXPECT_FALSE(config.standby_mode);
}

TEST(StandbyConfigTest, ApplySettingReplicationParams) {
    Config config = Config::load_defaults();

    config.apply_setting("replication.primary_host", "192.168.1.100");
    EXPECT_EQ(config.replication_primary_host, "192.168.1.100");

    config.apply_setting("replication.primary_port", "5433");
    EXPECT_EQ(config.replication_primary_port, 5433);

    config.apply_setting("replication.retry_interval_ms", "10000");
    EXPECT_EQ(config.replication_retry_interval_ms, 10000);

    config.apply_setting("replication.max_retry_interval_ms", "120000");
    EXPECT_EQ(config.replication_max_retry_interval_ms, 120000);
}

TEST(StandbyConfigTest, LoadFromJsonWithStandbyMode) {
    auto tmp = std::filesystem::temp_directory_path() / "giodb_test_standby_config.json";

    // Write a standby config file.
    {
        std::ofstream f(tmp);
        f << R"({
            "server": {
                "mode": "standby"
            },
            "replication": {
                "primary_host": "10.0.0.1",
                "primary_port": 5433,
                "retry_interval_ms": 3000,
                "max_retry_interval_ms": 30000
            }
        })";
    }

    auto result = Config::load_from_file(tmp.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(result->standby_mode);
    EXPECT_EQ(result->replication_primary_host, "10.0.0.1");
    EXPECT_EQ(result->replication_primary_port, 5433);
    EXPECT_EQ(result->replication_retry_interval_ms, 3000);
    EXPECT_EQ(result->replication_max_retry_interval_ms, 30000);

    std::filesystem::remove(tmp);
}

// =============================================================================
// DiskManager read-only tests
// =============================================================================

TEST(DiskManagerReadOnlyTest, OpenFileReadonly) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "giodb_test_dm_ro";
    std::filesystem::create_directories(tmp_dir);
    auto file_path = tmp_dir / "test.db";

    DiskManager dm;

    // Create a file first.
    auto create_result = dm.create_file(file_path);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
    auto write_id = *create_result;

    // Write some data.
    auto alloc_result = dm.allocate_page(write_id);
    ASSERT_TRUE(alloc_result.has_value());
    auto page_id = *alloc_result;

    Page page(page_id, PageType::DATA);
    page.raw()[page_header_size] = 0xAB;
    page.raw()[page_header_size + 1] = 0xCD;
    auto write_result = dm.write_page(write_id, page_id, page);
    ASSERT_TRUE(write_result.has_value());

    // Close the writable file.
    auto close_result = dm.close_file(write_id);
    ASSERT_TRUE(close_result.has_value());

    // Open as read-only.
    auto ro_result = dm.open_file_readonly(file_path);
    ASSERT_TRUE(ro_result.has_value()) << ro_result.error().message;
    auto ro_id = *ro_result;

    // Reading should work.
    Page read_page(page_id, PageType::DATA);
    auto read_result = dm.read_page(ro_id, page_id, read_page);
    ASSERT_TRUE(read_result.has_value()) << read_result.error().message;
    EXPECT_EQ(read_page.raw()[page_header_size], 0xAB);
    EXPECT_EQ(read_page.raw()[page_header_size + 1], 0xCD);

    // Writing should fail.
    auto write_fail = dm.write_page(ro_id, page_id, page);
    ASSERT_FALSE(write_fail.has_value());
    EXPECT_EQ(write_fail.error().code, StatusCode::READ_ONLY);

    // Allocating should fail.
    auto alloc_fail = dm.allocate_page(ro_id);
    ASSERT_FALSE(alloc_fail.has_value());
    EXPECT_EQ(alloc_fail.error().code, StatusCode::READ_ONLY);

    (void)dm.close_file(ro_id);
    std::filesystem::remove_all(tmp_dir);
}

TEST(DiskManagerReadOnlyTest, SharedLockAllowsMultipleReaders) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "giodb_test_dm_shared";
    std::filesystem::create_directories(tmp_dir);
    auto file_path = tmp_dir / "shared.db";

    DiskManager dm1;
    DiskManager dm2;

    // Create the file.
    auto create_result = dm1.create_file(file_path);
    ASSERT_TRUE(create_result.has_value());
    auto close_result = dm1.close_file(*create_result);
    ASSERT_TRUE(close_result.has_value());

    // Open as read-only from two different DiskManagers.
    auto ro1 = dm1.open_file_readonly(file_path);
    ASSERT_TRUE(ro1.has_value()) << ro1.error().message;

    auto ro2 = dm2.open_file_readonly(file_path);
    ASSERT_TRUE(ro2.has_value()) << ro2.error().message;

    // Both should be open simultaneously.
    auto count1 = dm1.file_page_count(*ro1);
    ASSERT_TRUE(count1.has_value());
    auto count2 = dm2.file_page_count(*ro2);
    ASSERT_TRUE(count2.has_value());
    EXPECT_EQ(*count1, *count2);

    (void)dm1.close_file(*ro1);
    (void)dm2.close_file(*ro2);
    std::filesystem::remove_all(tmp_dir);
}
