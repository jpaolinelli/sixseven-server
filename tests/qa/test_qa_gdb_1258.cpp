// QA v2 adversarial tests for GDB-1258.
//
// GDB-1258: the NEAREST-position reject guard (GDB-1253) was bypassed when
// NEAREST was nested under expression node types expr_contains_nearest did not
// descend into (CAST/CASE/IN/IS NULL/etc.), resurfacing the silent no-op.
//
// The fix (931dc7c) makes expr_contains_nearest walk exhaustively through every
// expression node type that carries child expressions. These tests are
// adversarial: they try DEEPER and COMBINED nesting shapes than the original
// repros, plus over-rejection guards confirming legitimate WHERE / subquery /
// CTE NEAREST is NOT rejected.
//
// "Silent no-op" outcome = query succeeds (OK) and returns the full unranked
// set with the vector search dropped. assert_not_silent_noop fails only on that
// specific bug outcome; a clean INVALID_ARGUMENT rejection (or any error) passes.

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

class QaGdb1258 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1258";
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

        exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (3, 'c', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (4, 'd', [0.0, 0.0, 1.0, 0.0])");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
    }

    // The bug outcome: query succeeds AND returns the full unranked books set
    // (4 rows) with NEAREST silently dropped. Fail only on that.
    void assert_not_silent_noop(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (result.has_value()) {
            ADD_FAILURE() << "SILENT NO-OP: NEAREST dropped, full unranked set returned ("
                          << result->rows.size() << " rows) for: " << sql;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// DEEPER / COMBINED nestings beyond the original single-wrapper repros.
// ---------------------------------------------------------------------------

// CAST wrapping a CASE wrapping NEAREST in ORDER BY.
TEST_F(QaGdb1258, OrderBy_CastOfCaseOfNearest_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books ORDER BY "
        "CAST((CASE WHEN NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0] THEN 1 ELSE 0 END) AS INT)");
}

// Function-call arg wrapping a CAST wrapping NEAREST in the SELECT list.
TEST_F(QaGdb1258, SelectList_FuncOfCastOfNearest_NotSilentNoOp) {
    assert_not_silent_noop("SELECT id, ABS(CAST((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS INT)) "
                           "AS n FROM books");
}

// NEAREST under a binary arithmetic op nested under a CASE in ORDER BY.
TEST_F(QaGdb1258, OrderBy_CaseOfArithmeticOfNearest_NotSilentNoOp) {
    assert_not_silent_noop("SELECT id FROM books ORDER BY "
                           "CASE WHEN 1=1 THEN (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AND 1=1 "
                           "ELSE 0 END");
}

// NEAREST inside LIKE in the SELECT list.
TEST_F(QaGdb1258, SelectList_NearestInsideLike_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id, (title LIKE (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0])) AS n FROM books");
}

// NEAREST inside an ARRAY literal in the SELECT list.
TEST_F(QaGdb1258, SelectList_NearestInsideArray_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id, [(NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0])] AS n FROM books");
}

// NEAREST as a function-call NAMED argument in ORDER BY.
TEST_F(QaGdb1258, OrderBy_NearestInNamedArg_NotSilentNoOp) {
    assert_not_silent_noop("SELECT id FROM books ORDER BY "
                           "ABS(x := (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]))");
}

// NEAREST inside NOT IN in ORDER BY.
TEST_F(QaGdb1258, OrderBy_NearestInsideNotIn_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books ORDER BY id NOT IN ((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]))");
}

// NEAREST inside a window function OVER ORDER BY, in the SELECT list.
TEST_F(QaGdb1258, SelectList_NearestInsideWindow_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id, ROW_NUMBER() OVER (ORDER BY (NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0])) AS n "
        "FROM books");
}

// Deeply nested: CAST(CASE(IN(IS NULL(NEAREST)))).
TEST_F(QaGdb1258, OrderBy_DeeplyNestedNearest_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books ORDER BY CAST((CASE WHEN "
        "((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) IS NULL) IN (true) THEN 1 ELSE 0 END) AS INT)");
}

// GROUP BY with NEAREST inside a CAST.
TEST_F(QaGdb1258, GroupBy_NearestInsideCast_NotSilentNoOp) {
    assert_not_silent_noop(
        "SELECT id FROM books GROUP BY CAST((NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS INT)");
}

// ---------------------------------------------------------------------------
// OVER-REJECTION GUARDS: legitimate NEAREST and plain queries must NOT be
// rejected by the now-exhaustive walker.
// ---------------------------------------------------------------------------

// WHERE NEAREST remains the supported ranked path.
TEST_F(QaGdb1258, Where_SingleTable_StillRanked) {
    auto result =
        engine_->execute("SELECT id FROM books WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

// WHERE NEAREST with an ordinary CAST/CASE elsewhere (no NEAREST) must work:
// the walker must not trip on CAST/CASE that do NOT contain NEAREST.
TEST_F(QaGdb1258, Where_Nearest_WithPlainCastInSelect_StillWorks) {
    auto result = engine_->execute("SELECT CAST(id AS INT) AS c FROM books "
                                   "WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

// Plain query with CAST/CASE/function in ORDER BY (no NEAREST) unaffected.
TEST_F(QaGdb1258, PlainQuery_CastCaseInOrderBy_Unaffected) {
    auto result = engine_->execute(
        "SELECT id FROM books ORDER BY CAST(id AS INT), CASE WHEN id > 2 THEN 1 ELSE 0 END");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 4u);
}

// A subquery whose OWN WHERE uses NEAREST (legitimate ranked path) must not be
// rejected just because it appears nested in an outer FROM.
TEST_F(QaGdb1258, Subquery_WhereNearest_StillRanked) {
    auto result = engine_->execute(
        "SELECT id FROM (SELECT id, vec FROM books "
        "WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) AS t");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

// CTE whose body uses WHERE NEAREST (legitimate) must not be over-rejected.
TEST_F(QaGdb1258, Cte_WhereNearest_StillRanked) {
    auto result = engine_->execute(
        "WITH t AS (SELECT id FROM books WHERE NEAREST(vec, 2) TO [1.0,0.0,0.0,0.0]) "
        "SELECT id FROM t");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

} // namespace
