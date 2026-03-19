/// @file test_qa_gdb_603.cpp
/// @brief QA adversarial tests for GDB-603: ORDER BY column alias resolution
///        when HAVING clause is present.
///
/// Targets: alias resolution in ORDER BY with/without HAVING, aggregate aliases,
/// non-aggregate aliases, case insensitivity, multiple ORDER BY aliases,
/// alias shadowing actual columns, mixed alias + column ORDER BY, ASC/DESC,
/// HAVING with LIMIT/OFFSET, unknown alias error path, expressions in ORDER BY.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB603 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb603";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Create test table with enough data for interesting GROUP BY / HAVING.
        exec_ok("CREATE TABLE emp (id INT, dept VARCHAR, salary INT, rating INT)");
        exec_ok("INSERT INTO emp VALUES (1,  'eng',   100, 5)");
        exec_ok("INSERT INTO emp VALUES (2,  'eng',   120, 4)");
        exec_ok("INSERT INTO emp VALUES (3,  'eng',   130, 3)");
        exec_ok("INSERT INTO emp VALUES (4,  'sales', 80,  5)");
        exec_ok("INSERT INTO emp VALUES (5,  'sales', 90,  4)");
        exec_ok("INSERT INTO emp VALUES (6,  'sales', 100, 3)");
        exec_ok("INSERT INTO emp VALUES (7,  'hr',    70,  5)");
        exec_ok("INSERT INTO emp VALUES (8,  'hr',    75,  4)");
        exec_ok("INSERT INTO emp VALUES (9,  'ops',   60,  3)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << "\n  -> " << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected) << sql;
        }
    }

    std::filesystem::path data_dir_;
    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// AC-1: The exact reproducer from the bug report must succeed.
// =============================================================================

TEST_F(QA_GDB603, ExactReproducer_AVGAliasWithHavingOrderBy) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 2 ORDER BY avg_sal DESC");
    // eng(3, avg~116.67) and sales(3, avg~90) pass HAVING; hr(2), ops(1) don't.
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[1][0].as_string(), "sales");
}

// =============================================================================
// AC-2: Without HAVING, alias ORDER BY still works (regression guard).
// =============================================================================

TEST_F(QA_GDB603, AliasOrderByWithoutHaving) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
        "FROM emp GROUP BY dept ORDER BY avg_sal DESC");
    ASSERT_EQ(qr.rows.size(), 4u);
    // eng(116.67) > sales(90) > hr(72.5) > ops(60)
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[3][0].as_string(), "ops");
}

// =============================================================================
// AC-3: HAVING without ORDER BY alias works (regression guard).
// =============================================================================

TEST_F(QA_GDB603, HavingWithoutOrderByAlias) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 2");
    ASSERT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Adversarial: ORDER BY COUNT alias with HAVING
// =============================================================================

TEST_F(QA_GDB603, OrderByCountAliasWithHaving) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 1 ORDER BY cnt ASC");
    // hr(2), eng(3), sales(3) pass; ops(1) doesn't.
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2);
}

// =============================================================================
// Adversarial: Multiple aliases in ORDER BY with HAVING
// =============================================================================

TEST_F(QA_GDB603, MultipleAliasesInOrderBy) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 1 ORDER BY cnt DESC, avg_sal ASC");
    ASSERT_EQ(qr.rows.size(), 3u);
    // cnt: eng=3, sales=3, hr=2. Tied cnt=3 sorted by avg_sal ASC: sales(90) < eng(116.67).
    EXPECT_EQ(qr.rows[0][0].as_string(), "sales");
    EXPECT_EQ(qr.rows[1][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[2][0].as_string(), "hr");
}

// =============================================================================
// Adversarial: Case-insensitive alias matching in ORDER BY
// =============================================================================

TEST_F(QA_GDB603, CaseInsensitiveAliasOrderBy) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS MyCount "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 2 ORDER BY mycount DESC");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
}

// =============================================================================
// Adversarial: Non-aggregate alias ORDER BY (no GROUP BY)
// =============================================================================

TEST_F(QA_GDB603, NonAggregateAliasOrderBy) {
    auto qr = exec_ok(
        "SELECT id, salary AS pay FROM emp ORDER BY pay DESC");
    ASSERT_EQ(qr.rows.size(), 9u);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 130); // highest salary
    EXPECT_EQ(qr.rows[8][1].as_int32(), 60);  // lowest salary
}

// =============================================================================
// Adversarial: Alias shadows an actual column name
// =============================================================================

TEST_F(QA_GDB603, AliasShadowsActualColumn) {
    // 'id' is a real column, but alias 'id' refers to salary.
    // PostgreSQL: ORDER BY resolves to the alias if present.
    auto qr = exec_ok(
        "SELECT salary AS id FROM emp ORDER BY id ASC");
    ASSERT_EQ(qr.rows.size(), 9u);
    // Should sort by salary (the alias), not the actual id column.
    EXPECT_EQ(qr.rows[0][0].as_int32(), 60);
    EXPECT_EQ(qr.rows[8][0].as_int32(), 130);
}

// =============================================================================
// Adversarial: Mixed alias and real column in ORDER BY
// =============================================================================

TEST_F(QA_GDB603, MixedAliasAndRealColumnOrderBy) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
        "FROM emp GROUP BY dept ORDER BY dept ASC");
    ASSERT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[1][0].as_string(), "hr");
    EXPECT_EQ(qr.rows[2][0].as_string(), "ops");
    EXPECT_EQ(qr.rows[3][0].as_string(), "sales");
}

// =============================================================================
// Adversarial: ORDER BY alias with HAVING and LIMIT
// =============================================================================

TEST_F(QA_GDB603, AliasOrderByHavingLimit) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 1 ORDER BY cnt DESC LIMIT 2");
    ASSERT_EQ(qr.rows.size(), 2u);
    // Top 2 by count: eng(3) and sales(3).
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 3);
}

// =============================================================================
// Adversarial: ORDER BY alias with HAVING, LIMIT, and OFFSET
// =============================================================================

TEST_F(QA_GDB603, AliasOrderByHavingLimitOffset) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 1 ORDER BY cnt DESC LIMIT 1 OFFSET 1");
    ASSERT_EQ(qr.rows.size(), 1u);
}

// =============================================================================
// Error path: Unknown alias in ORDER BY should still error
// =============================================================================

TEST_F(QA_GDB603, UnknownAliasErrors) {
    exec_error(
        "SELECT dept, COUNT(*) AS cnt FROM emp GROUP BY dept HAVING COUNT(*) > 1 "
        "ORDER BY nonexistent",
        StatusCode::NOT_FOUND);
}

// =============================================================================
// Error path: Unknown alias without HAVING should also error
// =============================================================================

TEST_F(QA_GDB603, UnknownAliasWithoutHavingErrors) {
    exec_error(
        "SELECT dept, COUNT(*) AS cnt FROM emp GROUP BY dept ORDER BY bogus",
        StatusCode::NOT_FOUND);
}

// =============================================================================
// Adversarial: SUM alias with HAVING
// =============================================================================

TEST_F(QA_GDB603, SumAliasWithHaving) {
    auto qr = exec_ok(
        "SELECT dept, SUM(salary) AS total_pay "
        "FROM emp GROUP BY dept HAVING SUM(salary) > 200 ORDER BY total_pay ASC");
    // eng: 350, sales: 270 pass; hr: 145, ops: 60 don't.
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "sales"); // 270
    EXPECT_EQ(qr.rows[1][0].as_string(), "eng");   // 350
}

// =============================================================================
// Adversarial: MIN/MAX alias with HAVING
// =============================================================================

TEST_F(QA_GDB603, MinMaxAliasWithHaving) {
    auto qr = exec_ok(
        "SELECT dept, MIN(salary) AS low, MAX(salary) AS high "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 1 ORDER BY low DESC");
    // eng(100), sales(80), hr(70). ops excluded.
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[2][0].as_string(), "hr");
}

// =============================================================================
// Adversarial: Multiple aggregate aliases, ORDER BY second one
// =============================================================================

TEST_F(QA_GDB603, OrderBySecondAggregateAlias) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, SUM(salary) AS total, AVG(rating) AS avg_r "
        "FROM emp GROUP BY dept HAVING COUNT(*) >= 2 ORDER BY avg_r DESC");
    // eng: avg_r=4, sales: avg_r=4, hr: avg_r=4.5. All have count >= 2 except ops.
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "hr"); // highest avg rating
}

// =============================================================================
// Adversarial: HAVING uses alias expression, ORDER BY uses different alias
// =============================================================================

TEST_F(QA_GDB603, HavingCountOrderByAvg) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt, AVG(rating) AS avg_r "
        "FROM emp GROUP BY dept HAVING COUNT(*) >= 3 ORDER BY avg_r ASC");
    // Only eng(3) and sales(3) have count >= 3.
    ASSERT_EQ(qr.rows.size(), 2u);
    // eng avg_r=4, sales avg_r=4 — same avg, order may be either.
    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Adversarial: ORDER BY ASC and DESC with same alias
// =============================================================================

TEST_F(QA_GDB603, OrderByAscDescDirections) {
    auto asc = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept ORDER BY cnt ASC");
    auto desc = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept ORDER BY cnt DESC");
    ASSERT_EQ(asc.rows.size(), 4u);
    ASSERT_EQ(desc.rows.size(), 4u);
    // First of ASC should equal last of DESC.
    EXPECT_EQ(asc.rows[0][1].as_int64(), desc.rows[3][1].as_int64());
    EXPECT_EQ(asc.rows[3][1].as_int64(), desc.rows[0][1].as_int64());
}

// =============================================================================
// Adversarial: Single group passes HAVING, ORDER BY alias
// =============================================================================

TEST_F(QA_GDB603, SingleGroupPassesHaving) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept HAVING COUNT(*) = 1 ORDER BY cnt DESC");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "ops");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
}

// =============================================================================
// Adversarial: No groups pass HAVING, ORDER BY alias — empty result
// =============================================================================

TEST_F(QA_GDB603, NoGroupsPassHaving) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept HAVING COUNT(*) > 100 ORDER BY cnt DESC");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Adversarial: All groups pass HAVING, ORDER BY alias
// =============================================================================

TEST_F(QA_GDB603, AllGroupsPassHaving) {
    auto qr = exec_ok(
        "SELECT dept, COUNT(*) AS cnt "
        "FROM emp GROUP BY dept HAVING COUNT(*) >= 1 ORDER BY cnt ASC");
    ASSERT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1); // ops
}

// =============================================================================
// Adversarial: ORDER BY alias on empty table
// =============================================================================

TEST_F(QA_GDB603, OrderByAliasOnEmptyTable) {
    exec_ok("CREATE TABLE empty_t (id INT, grp VARCHAR)");
    auto qr = exec_ok(
        "SELECT grp, COUNT(*) AS cnt FROM empty_t GROUP BY grp "
        "HAVING COUNT(*) > 0 ORDER BY cnt DESC");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Adversarial: Qualified column ref should NOT trigger alias resolution
//              (table.column in ORDER BY should not match an alias)
// =============================================================================

TEST_F(QA_GDB603, QualifiedColumnRefNotMatchedAsAlias) {
    // "emp.cnt" is qualified — should look for column 'cnt' in table 'emp',
    // NOT resolve to the alias. This should error because 'cnt' is not a column.
    exec_error(
        "SELECT dept, COUNT(*) AS cnt FROM emp GROUP BY dept ORDER BY emp.cnt",
        StatusCode::NOT_FOUND);
}
