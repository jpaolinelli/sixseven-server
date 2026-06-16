/// GDB-837: IN (subquery) evaluation — end-to-end executor tests.
///
/// Tests drive real SQL through the QueryEngine so the full parser → binder →
/// planner (SEMI/ANTI join rewrite) → executor path is exercised.  NULL
/// three-valued-logic cases and the NOT IN footgun are explicitly covered.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class InSubqueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_in_subquery";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
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
        EXPECT_TRUE(result.has_value()) << sql << ": " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << "Expected error for: " << sql;
        EXPECT_EQ(result.error().code, expected_code) << result.error().message;
    }

    /// Return the int32 values from column 0 of a result, sorted ascending.
    std::vector<int32_t> col0_ints_sorted(const QueryResult& qr) {
        std::vector<int32_t> out;
        out.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            out.push_back(row[0].as_int32());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Basic IN (subquery) — matching rows returned
// =============================================================================

TEST_F(InSubqueryTest, BasicInSubquery) {
    // outer: ids 1,2,3,4,5
    // inner (allowed): ids 2,4
    // Expected: rows 2 and 4
    exec_ok("CREATE TABLE outer_t (id INT, val VARCHAR)");
    exec_ok("INSERT INTO outer_t VALUES (1, 'a')");
    exec_ok("INSERT INTO outer_t VALUES (2, 'b')");
    exec_ok("INSERT INTO outer_t VALUES (3, 'c')");
    exec_ok("INSERT INTO outer_t VALUES (4, 'd')");
    exec_ok("INSERT INTO outer_t VALUES (5, 'e')");

    exec_ok("CREATE TABLE allowed (ref_id INT)");
    exec_ok("INSERT INTO allowed VALUES (2)");
    exec_ok("INSERT INTO allowed VALUES (4)");

    auto qr = exec_ok("SELECT id FROM outer_t WHERE id IN (SELECT ref_id FROM allowed)");
    ASSERT_EQ(qr.rows.size(), 2u);
    auto ids = col0_ints_sorted(qr);
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 4);
}

// =============================================================================
// No match → 0 rows
// =============================================================================

TEST_F(InSubqueryTest, NoMatchReturnsZeroRows) {
    exec_ok("CREATE TABLE items (id INT)");
    exec_ok("INSERT INTO items VALUES (10)");
    exec_ok("INSERT INTO items VALUES (20)");

    exec_ok("CREATE TABLE exclusions (x INT)");
    exec_ok("INSERT INTO exclusions VALUES (99)");

    auto qr = exec_ok("SELECT id FROM items WHERE id IN (SELECT x FROM exclusions)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// All match → all rows returned
// =============================================================================

TEST_F(InSubqueryTest, AllMatchReturnsAllRows) {
    exec_ok("CREATE TABLE nums (n INT)");
    exec_ok("INSERT INTO nums VALUES (1)");
    exec_ok("INSERT INTO nums VALUES (2)");
    exec_ok("INSERT INTO nums VALUES (3)");

    exec_ok("CREATE TABLE allowed2 (v INT)");
    exec_ok("INSERT INTO allowed2 VALUES (1)");
    exec_ok("INSERT INTO allowed2 VALUES (2)");
    exec_ok("INSERT INTO allowed2 VALUES (3)");

    auto qr = exec_ok("SELECT n FROM nums WHERE n IN (SELECT v FROM allowed2)");
    EXPECT_EQ(qr.rows.size(), 3u);
}

// =============================================================================
// NOT IN basic (no NULLs) → complement
// =============================================================================

TEST_F(InSubqueryTest, NotInBasicReturnsComplement) {
    // outer: 1,2,3,4,5 — NOT IN (2,4) → should return 1,3,5
    exec_ok("CREATE TABLE set_a (id INT)");
    exec_ok("INSERT INTO set_a VALUES (1)");
    exec_ok("INSERT INTO set_a VALUES (2)");
    exec_ok("INSERT INTO set_a VALUES (3)");
    exec_ok("INSERT INTO set_a VALUES (4)");
    exec_ok("INSERT INTO set_a VALUES (5)");

    exec_ok("CREATE TABLE set_b (excluded_id INT)");
    exec_ok("INSERT INTO set_b VALUES (2)");
    exec_ok("INSERT INTO set_b VALUES (4)");

    auto qr = exec_ok("SELECT id FROM set_a WHERE id NOT IN (SELECT excluded_id FROM set_b)");
    ASSERT_EQ(qr.rows.size(), 3u);
    auto ids = col0_ints_sorted(qr);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 3);
    EXPECT_EQ(ids[2], 5);
}

// =============================================================================
// NULL in subquery set: outer value not matching → row filtered (UNKNOWN)
// =============================================================================

TEST_F(InSubqueryTest, NullInSubqueryFiltersNonMatchingOuter) {
    // subquery set has NULL + 10; outer rows are 1,2,10.
    // x IN (10, NULL):
    //   10 → TRUE (returned)
    //   1  → UNKNOWN (filtered by WHERE)
    //   2  → UNKNOWN (filtered by WHERE)
    // So only row with id=10 is returned.
    exec_ok("CREATE TABLE outer_n (id INT)");
    exec_ok("INSERT INTO outer_n VALUES (1)");
    exec_ok("INSERT INTO outer_n VALUES (2)");
    exec_ok("INSERT INTO outer_n VALUES (10)");

    exec_ok("CREATE TABLE sub_n (v INT)");
    exec_ok("INSERT INTO sub_n VALUES (10)");
    exec_ok("INSERT INTO sub_n (v) VALUES (NULL)");

    auto qr = exec_ok("SELECT id FROM outer_n WHERE id IN (SELECT v FROM sub_n)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 10);
}

// =============================================================================
// NOT IN with NULL in subquery → returns NO rows (the classic footgun)
// =============================================================================

TEST_F(InSubqueryTest, NotInWithNullInSubqueryReturnsNoRows) {
    // x NOT IN (set with NULL) is NEVER TRUE (UNKNOWN or FALSE for every row).
    // Outer: 1,2,3.  Sub: (99, NULL).
    // 1 NOT IN (99, NULL) → UNKNOWN → filtered
    // 2 NOT IN (99, NULL) → UNKNOWN → filtered
    // 3 NOT IN (99, NULL) → UNKNOWN → filtered
    // Result: 0 rows.
    exec_ok("CREATE TABLE foo (id INT)");
    exec_ok("INSERT INTO foo VALUES (1)");
    exec_ok("INSERT INTO foo VALUES (2)");
    exec_ok("INSERT INTO foo VALUES (3)");

    exec_ok("CREATE TABLE bar (v INT)");
    exec_ok("INSERT INTO bar VALUES (99)");
    exec_ok("INSERT INTO bar (v) VALUES (NULL)");

    auto qr = exec_ok("SELECT id FROM foo WHERE id NOT IN (SELECT v FROM bar)");
    EXPECT_EQ(qr.rows.size(), 0u) << "NOT IN with NULL in subquery must return 0 rows";
}

// =============================================================================
// Empty subquery: IN → 0 rows
// =============================================================================

TEST_F(InSubqueryTest, EmptySubqueryInReturnsFalse) {
    exec_ok("CREATE TABLE src (id INT)");
    exec_ok("INSERT INTO src VALUES (1)");
    exec_ok("INSERT INTO src VALUES (2)");

    exec_ok("CREATE TABLE empty_sub (v INT)");
    // no rows in empty_sub

    auto qr = exec_ok("SELECT id FROM src WHERE id IN (SELECT v FROM empty_sub)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Empty subquery: NOT IN → all rows
// =============================================================================

TEST_F(InSubqueryTest, EmptySubqueryNotInReturnsAll) {
    exec_ok("CREATE TABLE src2 (id INT)");
    exec_ok("INSERT INTO src2 VALUES (7)");
    exec_ok("INSERT INTO src2 VALUES (8)");
    exec_ok("INSERT INTO src2 VALUES (9)");

    exec_ok("CREATE TABLE empty_sub2 (v INT)");
    // no rows

    auto qr = exec_ok("SELECT id FROM src2 WHERE id NOT IN (SELECT v FROM empty_sub2)");
    EXPECT_EQ(qr.rows.size(), 3u);
}

// =============================================================================
// Error: subquery with 2 columns → INVALID_ARGUMENT
// =============================================================================

TEST_F(InSubqueryTest, MultiColumnSubqueryReturnsError) {
    exec_ok("CREATE TABLE mc (a INT, b INT)");
    exec_ok("INSERT INTO mc VALUES (1, 2)");

    exec_ok("CREATE TABLE outer_mc (x INT)");
    exec_ok("INSERT INTO outer_mc VALUES (1)");

    exec_error("SELECT x FROM outer_mc WHERE x IN (SELECT a, b FROM mc)",
               StatusCode::INVALID_ARGUMENT);
}
