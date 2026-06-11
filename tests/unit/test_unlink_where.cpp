#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Test fixture for UNLINK WHERE with edge properties.
///
/// Builds graph:
///   users:    (1, "Alice"), (2, "Bob")
///   products: (10, "Widget"), (20, "Gadget")
///   edge type "rated" with score DOUBLE and review VARCHAR
///   edges:
///     users(1) -> products(10) via rated (score=4.5, review='good')
///     users(1) -> products(20) via rated (score=1.5, review='bad')
///     users(2) -> products(10) via rated (score=3.0, review='ok')
class UnlinkWhereTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_unlink_where";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");

        exec_ok("CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO products VALUES (10, 'Widget')");
        exec_ok("INSERT INTO products VALUES (20, 'Gadget')");

        exec_ok("CREATE EDGE TYPE rated (score DOUBLE, review VARCHAR) FROM users TO products");

        exec_ok("LINK users(1) TO products(10) VIA rated (score = 4.5, review = 'good')");
        exec_ok("LINK users(1) TO products(20) VIA rated (score = 1.5, review = 'bad')");
        exec_ok("LINK users(2) TO products(10) VIA rated (score = 3.0, review = 'ok')");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected error but got success";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// 1. Basic UNLINK WHERE — filter by numeric edge property
// ============================================================================

TEST_F(UnlinkWhereTest, FilterByNumericProperty) {
    // Remove only low-score ratings from user 1.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE score < 2.0");
    EXPECT_EQ(qr.affected_rows, 1);

    // Verify the high-score edge still exists.
    auto edges =
        exec_ok("SELECT rated.score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH");
    ASSERT_EQ(edges.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(edges.rows[0][0].as_float64(), 4.5);
}

// ============================================================================
// 2. UNLINK WHERE — no matching edges
// ============================================================================

TEST_F(UnlinkWhereTest, NoMatchingEdges) {
    // No edges with score > 100 exist.
    auto qr = exec_ok("UNLINK users(1) FROM products(10) VIA rated WHERE score > 100.0");
    EXPECT_EQ(qr.affected_rows, 0);

    // Original edge still exists.
    auto edges =
        exec_ok("SELECT rated.score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH");
    ASSERT_EQ(edges.rows.size(), 2u);
}

// ============================================================================
// 3. UNLINK WHERE — filter by string edge property
// ============================================================================

TEST_F(UnlinkWhereTest, FilterByStringProperty) {
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE review = 'bad'");
    EXPECT_EQ(qr.affected_rows, 1);

    // Only the 'good' review edge remains for user 1.
    auto edges =
        exec_ok("SELECT rated.review FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH");
    ASSERT_EQ(edges.rows.size(), 1u);
    EXPECT_EQ(edges.rows[0][0].as_string(), "good");
}

// ============================================================================
// 4. UNLINK without WHERE — unchanged behavior
// ============================================================================

TEST_F(UnlinkWhereTest, WithoutWhereUnchanged) {
    auto qr = exec_ok("UNLINK users(2) FROM products(10) VIA rated");
    EXPECT_EQ(qr.affected_rows, 1);

    // User 2 has no more edges.
    auto edges =
        exec_ok("SELECT rated.score FROM TRAVERSE rated FROM users(2) DIRECTION OUT FETCH");
    EXPECT_EQ(edges.rows.size(), 0u);
}

} // namespace
} // namespace sixseven
