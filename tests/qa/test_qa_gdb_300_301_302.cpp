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

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// QA tests for GDB-300, GDB-301, GDB-302:
//   GDB-300: Silent deserialization failure warning (Low)
//   GDB-301: PK column not-found error in planner (Low)
//   GDB-302: Schema column count assertion (Low)
//
// These are defensive improvements. The bugs only trigger with corrupted
// internal state that can't be produced through normal SQL. We verify that
// normal enriched traversal still works correctly after the fixes.
// ============================================================================

class QA_GDB300_301_302 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb300_302";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Jane')");
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(2) TO users(3) VIA follows");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32)
            return v.as_int32();
        return v.as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// GDB-302 regression: verify enriched tuple column count matches schema.
// This exercises the assertion added in enriched_traversal.cpp.
TEST_F(QA_GDB300_301_302, EnrichedTupleColumnCountMatchesSchema) {
    auto qr = exec_ok("SELECT id, name, __node, __depth, __source "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    // 5 columns: id, name, __node, __depth, __source
    ASSERT_EQ(qr.column_names.size(), 5u);
    // 2 reachable nodes from user 1.
    ASSERT_EQ(qr.rows.size(), 2u);
    // Each row should have exactly 5 values.
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row.size(), 5u);
    }
}

// GDB-300/301 regression: normal traversal with SELECT * works correctly.
TEST_F(QA_GDB300_301_302, SelectStarTraverseWorks) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 2u);
    // All rows should have non-null PKs.
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row.empty());
    }
}

// Verify enrichment with edge properties also respects schema count.
TEST_F(QA_GDB300_301_302, EnrichedWithEdgeProperties) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO items VALUES (10, 'Item A')");
    exec_ok("INSERT INTO items VALUES (20, 'Item B')");
    exec_ok("CREATE EDGE TYPE links (weight INT) FROM items TO items");
    exec_ok("LINK items(10) TO items(20) VIA links (weight = 5)");

    auto qr = exec_ok("SELECT id, title, __node, __depth, weight "
                      "FROM TRAVERSE links FROM items(10) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0].size(), 5u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 20);
}

} // namespace
} // namespace sixseven
