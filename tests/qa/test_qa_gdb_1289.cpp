// GDB-1289: TIMESTAMP = 'string' comparison -- maintainer ruling is
// PG-compatible COERCION: a TIMESTAMP column compared against a valid
// timestamp STRING literal coerces the literal to TIMESTAMP and the
// comparison proceeds (result BOOL, rows filtered correctly), exactly as
// PostgreSQL does. An invalid timestamp string produces a clean bind-time
// error rather than crashing or silently matching nothing.
//
// These are full end-to-end regression tests driving the real QueryEngine
// (CREATE TABLE / INSERT / SELECT ... WHERE), unlike the binder-only unit
// tests in test_qa_gdb_218_219_220.cpp which only check that the expression
// binds.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

namespace {

class QA_GDB1289 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1289";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());

        Config config = Config::load_defaults();
        auto bootstrap_result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config, data_dir_);
        ASSERT_TRUE(bootstrap_result.has_value()) << bootstrap_result.error().message;

        exec_ok("CREATE TABLE events (id INT, ts TIMESTAMP)");
        exec_ok("INSERT INTO events VALUES (1, '2025-12-31 23:00:00')");
        exec_ok("INSERT INTO events VALUES (2, '2026-01-01 00:00:00')");
        exec_ok("INSERT INTO events VALUES (3, '2026-01-02 00:00:00')");
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "SQL failed: " << sql << (result ? "" : (" -- " + result.error().message));
        return result ? std::move(*result) : QueryResult{};
    }

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
};

} // namespace

// -- Equality: valid datetime literal, coercion + correct row filtering -----

TEST_F(QA_GDB1289, EqualsValidDateTimeLiteralMatchesExactRow) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts = '2026-01-01 00:00:00'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

// -- Ordering: correct rows filtered in both directions ----------------------

TEST_F(QA_GDB1289, GreaterThanValidLiteralFiltersLaterRows) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts > '2026-01-01 00:00:00'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

TEST_F(QA_GDB1289, LessThanValidLiteralFiltersEarlierRows) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts < '2026-01-01 00:00:00'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// -- Operand order: literal-on-left behaves identically ----------------------

TEST_F(QA_GDB1289, LiteralOnLeftEqualsColumnMatchesExactRow) {
    auto qr = exec_ok("SELECT id FROM events WHERE '2026-01-01 00:00:00' = ts");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

TEST_F(QA_GDB1289, LiteralOnLeftLessThanColumnFiltersLaterRows) {
    // '2026-01-01' < ts  <=>  ts > '2026-01-01'
    auto qr = exec_ok("SELECT id FROM events WHERE '2026-01-01 00:00:00' < ts");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

// -- Date-only literal form (midnight) ---------------------------------------

TEST_F(QA_GDB1289, DateOnlyLiteralEqualsMidnightRow) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts = '2026-01-01'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

TEST_F(QA_GDB1289, DateOnlyLiteralGreaterThanFiltersCorrectly) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts > '2026-01-01'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

// -- Invalid timestamp string: clean error, not a crash or silent no-match ---

TEST_F(QA_GDB1289, InvalidTimestampStringComparisonReturnsCleanError) {
    auto result = exec("SELECT id FROM events WHERE ts = 'hello'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("timestamp"), std::string::npos)
        << "Error message should name the invalid timestamp: " << result.error().message;
}

TEST_F(QA_GDB1289, InvalidTimestampStringOnLeftReturnsCleanError) {
    auto result = exec("SELECT id FROM events WHERE 'not-a-timestamp' = ts");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- Sibling strict-rejection semantics remain unchanged (BOOL/FLOAT) --------

TEST_F(QA_GDB1289, BoolColumnVsStringLiteralStillRejected) {
    exec_ok("CREATE TABLE flags (id INT, active BOOL)");
    auto result = exec("SELECT id FROM flags WHERE active = 'true'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB1289, FloatColumnVsStringLiteralStillRejected) {
    exec_ok("CREATE TABLE measurements (id INT, value DOUBLE)");
    auto result = exec("SELECT id FROM measurements WHERE value > 'high'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}
