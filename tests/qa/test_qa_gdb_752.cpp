/// QA regression tests for GDB-752: CREATE TABLE accepts duplicate column names.
///
/// Audit finding: Catalog::create_table lacked a duplicate-column-name check
/// that Catalog::add_column already enforced. As a result,
/// CREATE TABLE t (a INT, a STRING) silently produced an ambiguous schema.
///
/// The fix adds an O(n^2) case-insensitive scan inside create_table before
/// any state is mutated, so failed creates leave no partial catalog entry.
///
/// These tests exercise the full engine pipeline (parse -> bind -> dispatch ->
/// execute) and assert that duplicate-column CREATE TABLE is rejected cleanly
/// while valid CREATE TABLE is unaffected.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class QA_GDB752 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb752";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    Error exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error for: " << sql;
        if (result.has_value()) {
            return Error{StatusCode::OK, "expected error but got success"};
        }
        return result.error();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// GDB-752 tests
// =============================================================================

/// Exact-case duplicate column name must be rejected.
TEST_F(QA_GDB752, DuplicateColumnExactCaseRejected) {
    auto err = exec_err("CREATE TABLE t (a INT, b VARCHAR, a BOOL)");
    EXPECT_EQ(err.code, StatusCode::ALREADY_EXISTS);
    EXPECT_NE(err.message.find('a'), std::string::npos)
        << "error message should name the duplicate column; got: " << err.message;
}

/// Case-insensitive duplicate (a INT, A VARCHAR) must be rejected.
TEST_F(QA_GDB752, DuplicateColumnDifferingCaseRejected) {
    auto err = exec_err("CREATE TABLE t (a INT, A VARCHAR)");
    EXPECT_EQ(err.code, StatusCode::ALREADY_EXISTS);
}

/// A valid CREATE TABLE with distinct column names must succeed.
TEST_F(QA_GDB752, ValidCreateTableUnaffected) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR, active BOOL)");

    // Confirm the table is queryable.
    auto qr = exec_ok("SELECT id, name FROM t");
    EXPECT_EQ(qr.rows.size(), 0u);
}

/// After a failed duplicate-column CREATE TABLE the table must not exist —
/// no partial state. A subsequent valid CREATE TABLE with the same name
/// must succeed (proving the name was not reserved).
TEST_F(QA_GDB752, NoPartialStateAfterFailure) {
    // This should fail.
    auto result = engine_->execute("CREATE TABLE t (x INT, x VARCHAR)");
    ASSERT_FALSE(result.has_value());

    // Table 't' must not be queryable.
    auto probe = engine_->execute("SELECT x FROM t");
    ASSERT_FALSE(probe.has_value()) << "table 't' should not exist after a failed CREATE TABLE";

    // A valid CREATE TABLE with the same name must succeed.
    exec_ok("CREATE TABLE t (x INT, y VARCHAR)");
    auto qr = exec_ok("SELECT x, y FROM t");
    EXPECT_EQ(qr.rows.size(), 0u);
}
