// GDB-1253: NEAREST(...) in an unsupported clause position.
//
// A NEAREST(...) predicate is only honored in a WHERE clause (single-table or
// pushed through a JOIN) and in a JOIN ON clause (where it is explicitly
// rejected by GDB-1250). Placing NEAREST in the SELECT list, ORDER BY, GROUP BY,
// or HAVING was previously a silent no-op: the binder dropped the vector search
// and the query returned the full, unranked result set with StatusCode::OK.
//
// This is the same class of silent-wrong-results that GDB-1249 eliminated for
// the JOIN+WHERE/ON case, but through a different code path. Consistent with the
// GDB-1249 stopgap, these positions now reject with INVALID_ARGUMENT and a clear
// NEAREST message. Single-table and JOIN-pushdown NEAREST in WHERE are
// unaffected.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class NearestPositionRejectTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_test_nearest_position_reject";
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

    void assert_nearest_rejected(const std::string& sql, const std::string& needle) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << "expected rejection for: " << sql;
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << sql;
        const std::string& m = result.error().message;
        EXPECT_NE(m.find("NEAREST"), std::string::npos) << m;
        EXPECT_NE(m.find(needle), std::string::npos) << m;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// Reproduces ticket step (1): single-table NEAREST in ORDER BY.
TEST_F(NearestPositionRejectTest, SingleTableNearestInOrderByRejected) {
    assert_nearest_rejected("SELECT id FROM books ORDER BY NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]",
                            "ORDER BY");
}

// Reproduces ticket step (2): single-table NEAREST in the SELECT list.
TEST_F(NearestPositionRejectTest, SingleTableNearestInSelectListRejected) {
    assert_nearest_rejected("SELECT id, (NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]) AS n FROM books",
                            "SELECT list");
}

// Reproduces ticket step (3): JOIN + NEAREST in ORDER BY.
TEST_F(NearestPositionRejectTest, JoinNearestInOrderByRejected) {
    assert_nearest_rejected("SELECT b.id FROM books b JOIN reviews r ON r.book_id = b.id "
                            "ORDER BY NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]",
                            "ORDER BY");
}

// HAVING previously surfaced an INTERNAL_ERROR ("unsupported_expr") instead of a
// clear NEAREST message. It now rejects with a clear NEAREST error.
TEST_F(NearestPositionRejectTest, NearestInHavingRejected) {
    assert_nearest_rejected("SELECT id FROM books GROUP BY id "
                            "HAVING NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]",
                            "HAVING");
}

// NEAREST in GROUP BY is likewise rejected with a clear message.
TEST_F(NearestPositionRejectTest, NearestInGroupByRejected) {
    assert_nearest_rejected("SELECT id FROM books GROUP BY NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]",
                            "GROUP BY");
}

// Regression guard: NEAREST in a single-table WHERE remains supported and ranked.
TEST_F(NearestPositionRejectTest, SingleTableNearestInWhereStillWorks) {
    auto result =
        engine_->execute("SELECT id FROM books WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

// Regression guard: a query with no NEAREST in any position is unaffected, even
// when it uses ORDER BY / the SELECT list normally.
TEST_F(NearestPositionRejectTest, PlainOrderByUnaffected) {
    auto result = engine_->execute("SELECT id FROM books ORDER BY id");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 4u);
    EXPECT_EQ(result->rows[0][0].as_int32(), 1);
}

} // namespace
