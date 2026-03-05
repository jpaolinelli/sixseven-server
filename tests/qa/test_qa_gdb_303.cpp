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

namespace sixseven {
namespace {

// ============================================================================
// QA tests for GDB-303: Edge properties not accessible via TRAVERSE alias
// ============================================================================

class QA_GDB303 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb303";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Set up tables and edges.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("CREATE EDGE TYPE follows (weight INT) FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows (weight = 99)");
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

// Edge property accessible without alias (should still work).
TEST_F(QA_GDB303, EdgePropertyWithoutAlias) {
    auto qr = exec_ok("SELECT follows.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 99);
}

// Edge property accessible via alias (the bug fix).
TEST_F(QA_GDB303, EdgePropertyViaAlias) {
    auto qr = exec_ok("SELECT t.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 99);
}

// Table columns and meta-columns accessible via alias (regression).
TEST_F(QA_GDB303, TableColumnsViaAlias) {
    auto qr =
        exec_ok("SELECT t.name, t.__depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
}

// Edge mode: edge property via alias.
TEST_F(QA_GDB303, EdgeModePropertyViaAlias) {
    auto qr = exec_ok(
        "SELECT t.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES AS t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 99);
}

} // namespace
} // namespace sixseven
