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
// QA tests for GDB-308: LINK PK existence validation
// ============================================================================

class QA_GDB308 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb308";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Set up table and edge type.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
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

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// LINK with valid PKs should succeed.
TEST_F(QA_GDB308, ValidPKsSucceed) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
}

// LINK with non-existent source PK should fail.
TEST_F(QA_GDB308, NonExistentSourceFails) {
    exec_error("LINK users(999) TO users(2) VIA follows", StatusCode::NOT_FOUND);
}

// LINK with non-existent target PK should fail.
TEST_F(QA_GDB308, NonExistentTargetFails) {
    exec_error("LINK users(1) TO users(999) VIA follows", StatusCode::NOT_FOUND);
}

// LINK with both non-existent PKs should fail (source checked first).
TEST_F(QA_GDB308, BothNonExistentFails) {
    exec_error("LINK users(888) TO users(999) VIA follows", StatusCode::NOT_FOUND);
}

// LINK with UUID table and valid UUID PKs.
TEST_F(QA_GDB308, UUIDTableValidPKs) {
    exec_ok("CREATE TABLE docs (id UUID PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO docs VALUES ('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Doc1')");
    exec_ok("INSERT INTO docs VALUES ('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Doc2')");
    exec_ok("CREATE EDGE TYPE refs FROM docs TO docs");
    exec_ok("LINK docs('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO docs('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA refs");
}

// LINK with UUID table and non-existent UUID should fail.
TEST_F(QA_GDB308, UUIDTableNonExistentFails) {
    exec_ok("CREATE TABLE items (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES ('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Item1')");
    exec_ok("CREATE EDGE TYPE links FROM items TO items");
    exec_error("LINK items('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
               "TO items('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') VIA links",
               StatusCode::NOT_FOUND);
}

} // namespace
} // namespace sixseven
