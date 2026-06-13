// GDB-1249 / GDB-1250: NEAREST(...) combined with a JOIN.
//
// GDB-1249 originally rejected every NEAREST+JOIN query as a silent-no-op
// stopgap. GDB-1250 implements real pushdown: NEAREST in a WHERE with a JOIN now
// succeeds (the vector scan is pushed to the table owning the EMBEDDING column).
// NEAREST inside a JOIN ON clause remains unsupported and still errors. The
// derived-table workaround (NEAREST inside a subquery) continues to plan and
// execute unchanged.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class NearestJoinRejectTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_nearest_join_reject";
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

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
    }

    static void assert_rejected(const Result<QueryResult>& result, const std::string& needle) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
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

// GDB-1250: NEAREST in a WHERE with a JOIN now succeeds (was rejected by the
// GDB-1249 stopgap). It returns the reviews of the 2 nearest books {1, 2}.
TEST_F(NearestJoinRejectTest, NearestInWhereWithJoinNowSucceeds) {
    auto result =
        engine_->execute("SELECT b.id FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{1, 2}));
}

// NEAREST inside a JOIN ON clause remains unsupported (GDB-1250 v1 restriction).
TEST_F(NearestJoinRejectTest, NearestInJoinOnClauseRejected) {
    assert_rejected(
        engine_->execute("SELECT b.id FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "AND NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]"),
        "ON clause");
}

TEST_F(NearestJoinRejectTest, DerivedTableNearestNotOverRejected) {
    auto result = engine_->execute(
        "SELECT nb.id, r.stars FROM "
        "(SELECT id, title, _distance FROM books WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]) nb "
        "JOIN reviews r ON nb.id = r.book_id");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    std::vector<int32_t> expected = {1, 2};
    EXPECT_EQ(ids, expected);
}

TEST_F(NearestJoinRejectTest, SingleTableNearestUnaffected) {
    auto result =
        engine_->execute("SELECT id FROM books WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

} // namespace
