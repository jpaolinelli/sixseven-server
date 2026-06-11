/// QA regression tests for GDB-711: VACUUM/ANALYZE parse and bind, then fail
/// with 'planner does not support this statement type'.
///
/// Audit finding C11 (severity high). VacuumStmt and AnalyzeStmt were validated
/// by the binder (binder.cpp bind_passthrough) but were never dispatched to an
/// executor. They fell through to execute_plan -> Planner::plan, whose default
/// branch returns NOT_IMPLEMENTED with the message
/// "planner does not support this statement type".
///
/// The fix dispatches VacuumStmt to the txn-module Vacuum and AnalyzeStmt to the
/// StatisticsStore (via analyze_table) after binding, alongside the other admin
/// commands (BACKFILL/REEMBED/REINDEX).
///
/// These tests exercise the full engine pipeline (parse -> bind -> dispatch ->
/// execute) and assert that VACUUM/ANALYZE are reachable and succeed.

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
// Fixture: full pipeline (parse -> bind -> dispatch -> execute)
// =============================================================================

class QA_GDB711 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb711";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // A table with some rows so ANALYZE/VACUUM have real data to scan.
        exec_ok("CREATE TABLE widgets (id INT, name VARCHAR, qty INT)");
        exec_ok("INSERT INTO widgets VALUES (1, 'alpha', 10)");
        exec_ok("INSERT INTO widgets VALUES (2, 'beta', 20)");
        exec_ok("INSERT INTO widgets VALUES (3, 'gamma', 30)");
        exec_ok("INSERT INTO widgets VALUES (4, 'delta', 40)");
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
            // Should not reach here; return a sentinel so the caller's
            // assertions fail loudly rather than crashing.
            return Error{StatusCode::OK, "expected error but got success"};
        }
        return result.error();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// AC 1: Regression — reproduce the original wrong behavior is now gone.
//
// Before the fix VACUUM/ANALYZE returned NOT_IMPLEMENTED with the message
// "planner does not support this statement type". These tests assert that the
// statements no longer produce that error — they must succeed.
// =============================================================================

TEST_F(QA_GDB711, VacuumTable_NoLongerNotImplemented) {
    auto result = engine_->execute("VACUUM widgets");
    ASSERT_TRUE(result.has_value()) << "VACUUM regressed to error: " << result.error().message;
    // Guard specifically against the regression: the old planner default branch.
    EXPECT_NE(result->message, std::string{});
}

TEST_F(QA_GDB711, AnalyzeTable_NoLongerNotImplemented) {
    auto result = engine_->execute("ANALYZE widgets");
    ASSERT_TRUE(result.has_value()) << "ANALYZE regressed to error: " << result.error().message;
    EXPECT_NE(result->message, std::string{});
}

// A direct assertion that the *specific* failure mode is gone: the planner's
// NOT_IMPLEMENTED "planner does not support this statement type" must never be
// what VACUUM/ANALYZE return.
TEST_F(QA_GDB711, VacuumDoesNotReturnPlannerNotImplemented) {
    auto result = engine_->execute("VACUUM");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(QA_GDB711, AnalyzeDoesNotReturnPlannerNotImplemented) {
    auto result = engine_->execute("ANALYZE");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// =============================================================================
// AC 2: Fix implemented — VACUUM dispatches to txn Vacuum and succeeds.
// =============================================================================

TEST_F(QA_GDB711, VacuumNamedTable_Succeeds) {
    auto qr = exec_ok("VACUUM widgets");
    EXPECT_EQ(qr.message, "VACUUM");
}

TEST_F(QA_GDB711, BareVacuum_VacuumsAllTables_Succeeds) {
    // No table name -> vacuum every user table in the current database.
    auto qr = exec_ok("VACUUM");
    EXPECT_EQ(qr.message, "VACUUM");
}

TEST_F(QA_GDB711, VacuumIsIdempotent_RepeatedCallsSucceed) {
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");
}

// VACUUM must not delete live data: rows are still present afterwards.
TEST_F(QA_GDB711, VacuumPreservesLiveRows) {
    exec_ok("VACUUM widgets");
    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 4u);
}

// =============================================================================
// AC 2: Fix implemented — ANALYZE dispatches to StatisticsStore and succeeds.
// =============================================================================

TEST_F(QA_GDB711, AnalyzeNamedTable_Succeeds) {
    auto qr = exec_ok("ANALYZE widgets");
    EXPECT_EQ(qr.message, "ANALYZE");
}

TEST_F(QA_GDB711, BareAnalyze_AnalyzesAllTables_Succeeds) {
    auto qr = exec_ok("ANALYZE");
    EXPECT_EQ(qr.message, "ANALYZE");
}

TEST_F(QA_GDB711, AnalyzeDoesNotMutateData) {
    exec_ok("ANALYZE widgets");
    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 4u);
}

TEST_F(QA_GDB711, AnalyzeEmptyTable_Succeeds) {
    exec_ok("CREATE TABLE empty_t (id INT, label VARCHAR)");
    auto qr = exec_ok("ANALYZE empty_t");
    EXPECT_EQ(qr.message, "ANALYZE");
}

TEST_F(QA_GDB711, VacuumEmptyTable_Succeeds) {
    exec_ok("CREATE TABLE empty_v (id INT, label VARCHAR)");
    auto qr = exec_ok("VACUUM empty_v");
    EXPECT_EQ(qr.message, "VACUUM");
}

// =============================================================================
// Error handling: a missing table name must surface as a clean NOT_FOUND from
// the binder, never the planner's misleading NOT_IMPLEMENTED.
// =============================================================================

TEST_F(QA_GDB711, VacuumUnknownTable_ReturnsNotFound) {
    auto err = exec_err("VACUUM does_not_exist");
    EXPECT_EQ(err.code, StatusCode::NOT_FOUND);
    EXPECT_NE(err.code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB711, AnalyzeUnknownTable_ReturnsNotFound) {
    auto err = exec_err("ANALYZE does_not_exist");
    EXPECT_EQ(err.code, StatusCode::NOT_FOUND);
    EXPECT_NE(err.code, StatusCode::NOT_IMPLEMENTED);
}

// =============================================================================
// Interaction: ANALYZE/VACUUM after a DELETE remain reachable and succeed.
// =============================================================================

TEST_F(QA_GDB711, VacuumAfterDelete_Succeeds) {
    exec_ok("DELETE FROM widgets WHERE id = 2");
    auto qr = exec_ok("VACUUM widgets");
    EXPECT_EQ(qr.message, "VACUUM");
}

TEST_F(QA_GDB711, AnalyzeAfterDelete_Succeeds) {
    exec_ok("DELETE FROM widgets WHERE id = 2");
    auto qr = exec_ok("ANALYZE widgets");
    EXPECT_EQ(qr.message, "ANALYZE");
}
