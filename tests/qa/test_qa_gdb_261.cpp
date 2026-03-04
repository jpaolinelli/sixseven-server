#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace giodb {
namespace {

// ============================================================================
// QA tests for GDB-261: CREATE DATABASE IF NOT EXISTS
// ============================================================================

class QA_GDB261 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb261";
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
            EXPECT_EQ(result.error().code, expected);
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// IF NOT EXISTS before database name (standard parser syntax).
TEST_F(QA_GDB261, IfNotExistsBeforeName) {
    exec_ok("CREATE DATABASE IF NOT EXISTS analytics");
    // Second time should succeed silently.
    exec_ok("CREATE DATABASE IF NOT EXISTS analytics");
}

// IF NOT EXISTS after database name (README syntax).
TEST_F(QA_GDB261, IfNotExistsAfterName) {
    exec_ok("CREATE DATABASE mydb IF NOT EXISTS");
    // Second time should succeed silently.
    exec_ok("CREATE DATABASE mydb IF NOT EXISTS");
}

// Without IF NOT EXISTS, duplicate should fail.
TEST_F(QA_GDB261, WithoutIfNotExistsDuplicateFails) {
    exec_ok("CREATE DATABASE testdb");
    exec_error("CREATE DATABASE testdb", StatusCode::ALREADY_EXISTS);
}

} // namespace
} // namespace giodb
