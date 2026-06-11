/// @file test_qa_gdb_729.cpp
/// QA regression tests for GDB-729: Config::apply_setting threw uncaught
/// std::invalid_argument / std::out_of_range on non-numeric input, causing a
/// server crash-loop on every restart when a bad value had been persisted via
/// SET.
///
/// Fix: apply_setting now returns Result<void> and uses std::from_chars
/// (no exceptions). SettingsCache::update validates the value before
/// persisting, so malformed values never reach sys_settings.
/// SystemBootstrap::load_settings skips and logs bad persisted values.
///
/// AC1: SET a non-numeric value into a numeric setting returns an error at SET
///      time — the setting is NOT persisted.
/// AC2: apply_setting("replication.keepalive_interval_ms", "abc") returns
///      INVALID_ARGUMENT (was: would throw and terminate the server).
/// AC3: Trailing junk ("123abc") is rejected.
/// AC4: Valid numeric values continue to work identically.
/// AC5: String/bool/unknown keys accept any value.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture — full bootstrap so SettingsCache and sys_settings are live.
// =============================================================================

class QA729SettingsValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa729";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        bootstrap_qa_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        config_ = Config::load_defaults();
        cache_ = std::make_unique<SettingsCache>();

        auto boot = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        auto load = cache_->load(*engine_);
        ASSERT_TRUE(load.has_value()) << load.error().message;

        engine_->set_settings_cache(cache_.get());

        // Switch to default database for user-facing SQL.
        engine_->set_current_database(default_database_id);
    }

    void TearDown() override {
        engine_.reset();
        cache_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    // Execute SQL and expect success.
    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r ? std::move(*r) : QueryResult{};
    }

    // Execute SQL and expect failure with the given status code.
    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto r = engine_->execute(sql);
        EXPECT_FALSE(r.has_value()) << "Expected error for: " << sql;
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code, expected_code) << "Got: " << r.error().message;
        }
    }

    // Return the persisted value of a setting from sys_settings.
    std::string get_persisted(const std::string& key) {
        auto prev = engine_->current_database_id();
        engine_->set_current_database(system_database_id);
        auto r = engine_->execute("SELECT value FROM sys_settings WHERE key = '" + key + "'");
        engine_->set_current_database(prev);
        if (!r || r->rows.empty()) {
            return "";
        }
        return r->rows[0][0].is_null() ? "" : r->rows[0][0].as_string();
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<SettingsCache> cache_;
    Config config_;
};

// =============================================================================
// AC1: SET a non-numeric value into a numeric setting must error at SET time
//      and must NOT persist the bad value to sys_settings.
// =============================================================================

TEST_F(QA729SettingsValidationTest, SetAlphaValueIntoNumericSettingFails) {
    // 'replication.keepalive_interval_ms' is runtime-mutable and numeric.
    // Switch to system DB context to use the SET command on a system setting.
    engine_->set_current_database(system_database_id);
    auto r = engine_->execute("SET replication.keepalive_interval_ms = 'abc'");
    engine_->set_current_database(default_database_id);

    EXPECT_FALSE(r.has_value()) << "Expected error, but SET succeeded";
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT) << r.error().message;
    }

    // The persisted value must still be the default (10000), not 'abc'.
    EXPECT_EQ(get_persisted("replication.keepalive_interval_ms"), "10000");
}

TEST_F(QA729SettingsValidationTest, SetTrailingJunkIntoNumericSettingFails) {
    engine_->set_current_database(system_database_id);
    auto r = engine_->execute("SET replication.keepalive_interval_ms = '5000ms'");
    engine_->set_current_database(default_database_id);

    EXPECT_FALSE(r.has_value()) << "Expected error for trailing-junk value";
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT) << r.error().message;
    }
    EXPECT_EQ(get_persisted("replication.keepalive_interval_ms"), "10000");
}

// =============================================================================
// AC2: apply_setting with a non-numeric value returns INVALID_ARGUMENT
//      (pre-fix: would throw std::invalid_argument -> terminate).
// =============================================================================

TEST(QA_GDB729_ApplySetting, AlphaValueReturnsError) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("replication.keepalive_interval_ms", "abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    // Original value must be unchanged.
    EXPECT_EQ(config.replication_keepalive_interval_ms, 10000);
}

TEST(QA_GDB729_ApplySetting, EmptyValueReturnsError) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("server.port", "");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(config.port, 6767);
}

// =============================================================================
// AC3: Trailing junk is rejected.
// =============================================================================

TEST(QA_GDB729_ApplySetting, TrailingJunkRejected) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("server.port", "80abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(config.port, 6767);
}

TEST(QA_GDB729_ApplySetting, TrailingJunkInI32Rejected) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("replication.max_wal_senders", "10x");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// AC4: Valid numeric values continue to work correctly.
// =============================================================================

TEST(QA_GDB729_ApplySetting, ValidPortApplied) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("server.port", "5432");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.port, 5432);
}

TEST(QA_GDB729_ApplySetting, ValidI32Applied) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("replication.keepalive_interval_ms", "30000");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.replication_keepalive_interval_ms, 30000);
}

TEST(QA_GDB729_ApplySetting, ValidI64Applied) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("replication.promote_max_lag_bytes", "1073741824");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.replication_promote_max_lag_bytes, 1073741824LL);
}

TEST_F(QA729SettingsValidationTest, SetValidNumericValuePersists) {
    engine_->set_current_database(system_database_id);
    auto r = engine_->execute("SET replication.keepalive_interval_ms = '30000'");
    engine_->set_current_database(default_database_id);

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    EXPECT_EQ(get_persisted("replication.keepalive_interval_ms"), "30000");
}

// =============================================================================
// AC5: String / bool / unknown keys accept any value without error.
// =============================================================================

TEST(QA_GDB729_ApplySetting, StringKeyAcceptsArbitraryValue) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("logging.level", "anything_goes");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.log_level, "anything_goes");
}

TEST(QA_GDB729_ApplySetting, BoolKeyAcceptsArbitraryValue) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("replication.archive_enabled", "true");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(config.archive_enabled);
}

TEST(QA_GDB729_ApplySetting, UnknownKeyIgnoredWithoutError) {
    Config config = Config::load_defaults();
    auto r = config.apply_setting("no.such.key", "garbage");
    ASSERT_TRUE(r.has_value()) << r.error().message;
}