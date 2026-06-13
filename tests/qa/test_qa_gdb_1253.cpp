// QA adversarial tests for GDB-1253.
//
// GDB-1253 rejects NEAREST(...) with INVALID_ARGUMENT when it appears in the
// SELECT list, ORDER BY, GROUP BY, or HAVING (the reject-path stopgap,
// consistent with GDB-1249). The supported WHERE NEAREST path (single-table
// and JOIN-pushdown) must remain unaffected.
//
// These tests try to BREAK the guard:
//   * NEAREST nested inside expression node types the guard's recursive walk
//     (expr_contains_nearest) does NOT descend into: FunctionCallExpr, CastExpr,
//     CaseExpr, InExpr, BetweenExpr, LikeExpr, IsNullExpr. NEAREST parses as a
//     primary, so it can legally sit underneath any of these.
//   * Mixed positions, case variations, subqueries / CTEs / set operations.
//   * Regression guards for the supported WHERE paths.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QaGdb1253 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1253";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        seed();
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << sql << ": " << result.error().message;
    }

    void seed() {
        exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, vec EMBEDDING)");
        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        EmbeddingColumnDef emb;
        emb.table_id = books->table_id;
        emb.column_id = 2;
        emb.dimension = 4;
        emb.source_expr = "title";
        emb.provider = "builtin/4";
        ASSERT_TRUE(catalog_.register_embedding_column(emb).has_value());

        // Vectors ordered by increasing distance to [1,0,0,0]: ids 1,2 nearest.
        exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (3, 'c', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (4, 'd', [0.0, 0.0, 1.0, 0.0])");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
    }

    // A NEAREST that escaped the guard becomes a silent no-op: the query
    // succeeds and returns the full unranked set (4 rows for single-table
    // books). This helper asserts the *bug* outcome is gone: the query must NOT
    // succeed-and-return-4. It must either reject (INVALID_ARGUMENT) or, if some
    // other deterministic error happens, at least not silently return the full
    // unranked set with OK.
    void assert_not_silent_noop(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (result.has_value()) {
            ADD_FAILURE() << "SILENT NO-OP: NEAREST dropped, full unranked set returned ("
                          << result->rows.size() << " rows) for: " << sql;
        }
    }

    void assert_nearest_rejected(const std::string& sql, const std::string& needle) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << "expected rejection for: " << sql;
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << sql;
        const std::string& m = result.error().message;
        EXPECT_NE(m.find("NEAREST"), std::string::npos) << "message lacks NEAREST: " << m;
        EXPECT_NE(m.find(needle), std::string::npos) << "message lacks '" << needle << "': " << m;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// Direct positions (mirror ticket repro; baseline that the guard fires).
// ---------------------------------------------------------------------------

TEST_F(QaGdb1253, GDB1253_OrderBy_Direct_Rejected) {
    assert_nearest_rejected("SELECT id FROM books ORDER BY NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]",
                            "ORDER BY");
}

TEST_F(QaGdb1253, GDB1253_SelectList_Direct_Rejected) {
    assert_nearest_rejected("SELECT id, (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS n FROM books",
                            "SELECT list");
}

TEST_F(QaGdb1253, GDB1253_GroupBy_Direct_Rejected) {
    assert_nearest_rejected("SELECT id FROM books GROUP BY NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]",
                            "GROUP BY");
}

TEST_F(QaGdb1253, GDB1253_Having_Direct_Rejected) {
    assert_nearest_rejected(
        "SELECT id FROM books GROUP BY id HAVING NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]", "HAVING");
}

TEST_F(QaGdb1253, GDB1253_Join_OrderBy_Direct_Rejected) {
    assert_nearest_rejected("SELECT b.id FROM books b JOIN reviews r ON r.book_id = b.id "
                            "ORDER BY NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]",
                            "ORDER BY");
}

// ---------------------------------------------------------------------------
// ADVERSARIAL: NEAREST wrapped in non-Binary/Unary expression node types.
// Before GDB-1258, expr_contains_nearest only descended BinaryExpr and
// UnaryExpr, so NEAREST nested under a function call / cast / CASE / IN /
// BETWEEN / LIKE / IS NULL bypassed the reject and produced a silent no-op.
// The guard now recurses through every expression node type with children.
// ---------------------------------------------------------------------------

// NEAREST as a function-call argument in ORDER BY.
TEST_F(QaGdb1253, GDB1253_OrderBy_NearestInsideFunctionArg_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books ORDER BY ABS(NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0])");
}

// NEAREST inside a CAST in ORDER BY.
TEST_F(QaGdb1253, GDB1253_OrderBy_NearestInsideCast_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books ORDER BY CAST((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS INT)");
}

// NEAREST inside a CASE in the SELECT list.
TEST_F(QaGdb1253, GDB1253_SelectList_NearestInsideCase_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id, CASE WHEN NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0] THEN 1 ELSE 0 END AS n "
        "FROM books");
}

// NEAREST inside an IN list in ORDER BY.
TEST_F(QaGdb1253, GDB1253_OrderBy_NearestInsideInList_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books ORDER BY id IN ((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]))");
}

// NEAREST inside a BETWEEN in HAVING.
TEST_F(QaGdb1253, GDB1253_Having_NearestInsideBetween_NotSilentNoOp) {
    assert_not_silent_noop("SELECT id FROM books GROUP BY id "
                           "HAVING id BETWEEN (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AND 100");
}

// NEAREST inside IS NULL in the SELECT list.
TEST_F(QaGdb1253, GDB1253_SelectList_NearestInsideIsNull_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id, ((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) IS NULL) AS n FROM books");
}

// ---------------------------------------------------------------------------
// ADVERSARIAL: subqueries, CTEs, set operations.
// ---------------------------------------------------------------------------

// NEAREST in the ORDER BY of a derived-table subquery.
TEST_F(QaGdb1253, GDB1253_Subquery_OrderBy_NotSilentNoOp) {
    assert_not_silent_noop("SELECT id FROM (SELECT id, vec FROM books "
                           "ORDER BY NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS t");
}

// NEAREST in the SELECT list of a CTE.
TEST_F(QaGdb1253, GDB1253_Cte_SelectList_NotSilentNoOp) {
    assert_not_silent_noop(
        "WITH t AS (SELECT id, (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS n FROM books) "
        "SELECT id FROM t");
}

// ---------------------------------------------------------------------------
// ADVERSARIAL: case-insensitivity and spacing of the rejected positions.
// ---------------------------------------------------------------------------

TEST_F(QaGdb1253, GDB1253_OrderBy_LowerCase_Rejected) {
    assert_nearest_rejected("select id from books order by nearest(vec, 2) to [1.0,0.0,0.0,0.0]",
                            "ORDER BY");
}

// ---------------------------------------------------------------------------
// REGRESSION GUARDS: the supported WHERE path must be unaffected.
// ---------------------------------------------------------------------------

TEST_F(QaGdb1253, GDB1253_Where_SingleTable_StillRanked) {
    auto result =
        engine_->execute("SELECT id FROM books WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u) << "top-2 NEAREST should return exactly 2 rows";
}

TEST_F(QaGdb1253, GDB1253_Where_JoinPushdown_StillWorks) {
    auto result = engine_->execute("SELECT b.id FROM books b JOIN reviews r ON r.book_id = b.id "
                                   "WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // top-2 books are ids 1,2 which both have matching reviews -> 2 joined rows.
    EXPECT_EQ(result->rows.size(), 2u);
}

// A NEAREST in WHERE combined with a plain (non-NEAREST) ORDER BY must remain
// supported: the guard must not over-reject ordinary ORDER BY.
TEST_F(QaGdb1253, GDB1253_Where_Nearest_With_PlainOrderBy_StillWorks) {
    auto result = engine_->execute(
        "SELECT id FROM books WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0] ORDER BY id");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

// No NEAREST anywhere: ordinary query fully unaffected.
TEST_F(QaGdb1253, GDB1253_NoNearest_PlainQuery_Unaffected) {
    auto result = engine_->execute("SELECT id FROM books ORDER BY id");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 4u);
    EXPECT_EQ(result->rows[0][0].as_int32(), 1);
}

} // namespace
