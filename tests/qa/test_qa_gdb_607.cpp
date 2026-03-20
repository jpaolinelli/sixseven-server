#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// QA regression test for GDB-607:
// Window functions use wrong default frame when ORDER BY is absent
// =============================================================================

class GDB607WindowFrameDefaultQA : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb607";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE t (id INT PRIMARY KEY, grp TEXT)");
        exec_ok("INSERT INTO t VALUES (1, 'A')");
        exec_ok("INSERT INTO t VALUES (2, 'A')");
        exec_ok("INSERT INTO t VALUES (3, 'A')");
        exec_ok("INSERT INTO t VALUES (4, 'B')");
        exec_ok("INSERT INTO t VALUES (5, 'B')");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\n"
                                        << "Error: " << result.error().message;
        return std::move(*result);
    }

    static int64_t get_int(const Value& v) {
        switch (v.type_id()) {
        case TypeId::INT32:
            return v.as_int32();
        case TypeId::INT64:
            return v.as_int64();
        default:
            ADD_FAILURE() << "get_int: unexpected type " << static_cast<int>(v.type_id());
            return 0;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// Reproduce exact steps from ticket: COUNT(*) OVER (PARTITION BY grp) should
// return partition totals, not running counts.
TEST_F(GDB607WindowFrameDefaultQA, ReproSteps_CountPartitionByGrp) {
    auto qr = exec_ok("SELECT id, grp, COUNT(*) OVER (PARTITION BY grp) AS total FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        auto grp = row[1].as_string();
        auto total = get_int(row[2]);
        if (grp == "A") {
            EXPECT_EQ(total, 3) << "All rows in partition 'A' should show total=3";
        } else {
            EXPECT_EQ(total, 2) << "All rows in partition 'B' should show total=2";
        }
    }
}

// COUNT(*) OVER () — no partition, no order by — should return total row count.
TEST_F(GDB607WindowFrameDefaultQA, CountOverEmpty) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER () AS total FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (auto& row : qr.rows) {
        EXPECT_EQ(get_int(row[1]), 5) << "COUNT(*) OVER () should return 5 for all rows";
    }
}

// COUNT(*) OVER (ORDER BY id) should still produce running counts (default
// frame WITH ORDER BY is UNBOUNDED PRECEDING to CURRENT ROW).
TEST_F(GDB607WindowFrameDefaultQA, CountWithOrderByIsRunning) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER (ORDER BY id) AS running FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(get_int(qr.rows[i][1]), static_cast<int64_t>(i + 1));
    }
}

// Explicit frame should override the default regardless of ORDER BY presence.
TEST_F(GDB607WindowFrameDefaultQA, ExplicitFrameOverrides) {
    auto qr = exec_ok("SELECT id, COUNT(*) OVER ("
                      "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW"
                      ") AS running FROM t");
    ASSERT_EQ(qr.rows.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(get_int(qr.rows[i][1]), static_cast<int64_t>(i + 1));
    }
}
