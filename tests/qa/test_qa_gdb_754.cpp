// QA regression tests for GDB-754: Wire cost-based optimizer and statistics
// into the live planner.
//
// End-to-end acceptance criteria:
//   AC1: ANALYZE populates statistics (row counts / distinct values).
//   AC2: Planner decisions change with stats — the index scan is kept for
//        selective predicates, an unselective predicate switches to seq scan,
//        and unanalyzed tables keep the default plan.
//   AC3: EXPLAIN shows costs once statistics exist.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/planner/statistics.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QA_GDB754 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb_754";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(catalog_, *storage_);
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    std::string explain(const std::string& sql) {
        auto qr = exec_ok("EXPLAIN " + sql);
        std::string text;
        for (const auto& row : qr.rows) {
            text += row[0].as_string();
            text += "\n";
        }
        return text;
    }

    /// CREATE the orders table with an index on customer_id and insert
    /// skewed data: customer_id 1..rows (unique) and a wide padding column
    /// so the heap spans multiple pages.
    void setup_orders(int rows) {
        exec_ok("CREATE TABLE orders (order_id INT, customer_id INT, note VARCHAR)");
        const std::string padding(120, 'p');
        std::string batch;
        for (int i = 1; i <= rows; ++i) {
            if (batch.empty()) {
                batch = "INSERT INTO orders VALUES ";
            } else {
                batch += ", ";
            }
            batch += "(" + std::to_string(i) + ", " + std::to_string(i) + ", '" + padding + "')";
            if (i % 100 == 0 || i == rows) {
                exec_ok(batch);
                batch.clear();
            }
        }
        exec_ok("CREATE INDEX idx_orders_customer ON orders(customer_id)");
        rebuild_indexes();
    }

    [[nodiscard]] table_id_t table_id_of(const std::string& name) {
        auto schema = catalog_.get_table(default_database_id, name);
        EXPECT_TRUE(schema.has_value());
        return schema ? schema->table_id : 0;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// -- AC1: ANALYZE populates statistics ----------------------------------------

TEST_F(QA_GDB754, AC1_AnalyzePopulatesRowCountsAndDistinctValues) {
    setup_orders(300);

    auto tid = table_id_of("orders");
    EXPECT_EQ(engine_->statistics().get_table_stats(tid), nullptr);

    exec_ok("ANALYZE orders");

    const auto* ts = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 300u);
    EXPECT_GT(ts->page_count, 1u);

    const auto* cs = engine_->statistics().get_column_stats(tid, 1); // customer_id
    ASSERT_NE(cs, nullptr);
    EXPECT_EQ(cs->ndistinct, 300u);
    EXPECT_EQ(cs->null_fraction, 0.0);
}

TEST_F(QA_GDB754, AC1_BareAnalyzeCoversAllTablesInDatabase) {
    exec_ok("CREATE TABLE t1 (id INT)");
    exec_ok("CREATE TABLE t2 (id INT)");
    exec_ok("INSERT INTO t1 VALUES (1), (2), (3)");
    exec_ok("INSERT INTO t2 VALUES (7)");

    exec_ok("ANALYZE");

    const auto* s1 = engine_->statistics().get_table_stats(table_id_of("t1"));
    const auto* s2 = engine_->statistics().get_table_stats(table_id_of("t2"));
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1->row_count, 3u);
    EXPECT_EQ(s2->row_count, 1u);
}

// -- AC2: planner decisions change with statistics ------------------------------

TEST_F(QA_GDB754, AC2_UnselectivePredicateIndexBeforeSeqAfterAnalyze) {
    setup_orders(600);

    // Pre-ANALYZE default behavior: the matching index is always used.
    auto before = explain("SELECT * FROM orders WHERE customer_id > 0");
    EXPECT_NE(before.find("Index Scan"), std::string::npos) << before;

    exec_ok("ANALYZE orders");

    // customer_id > 0 matches every row — with statistics the optimizer
    // must switch to a sequential scan.
    auto after = explain("SELECT * FROM orders WHERE customer_id > 0");
    EXPECT_NE(after.find("Seq Scan"), std::string::npos) << after;
    EXPECT_EQ(after.find("Index Scan"), std::string::npos) << after;

    // Correctness of the stats-driven plan.
    auto qr = exec_ok("SELECT * FROM orders WHERE customer_id > 590");
    EXPECT_EQ(qr.rows.size(), 10u);
}

TEST_F(QA_GDB754, AC2_SelectivePredicateKeepsIndexScanAfterAnalyze) {
    setup_orders(600);
    exec_ok("ANALYZE orders");

    auto plan = explain("SELECT * FROM orders WHERE customer_id = 123");
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;

    auto qr = exec_ok("SELECT * FROM orders WHERE customer_id = 123");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 123);
}

TEST_F(QA_GDB754, AC2_UnanalyzedTableBehaviorUnchanged) {
    setup_orders(200);

    // No ANALYZE: index plan as before, and no cost annotations at all.
    auto plan = explain("SELECT * FROM orders WHERE customer_id > 0");
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("cost="), std::string::npos) << plan;

    auto qr = exec_ok("SELECT * FROM orders WHERE customer_id = 7");
    ASSERT_EQ(qr.rows.size(), 1u);
}

TEST_F(QA_GDB754, AC2_JoinPlanInfluencedByTableSizes) {
    exec_ok("CREATE TABLE small_t (id INT)");
    exec_ok("CREATE TABLE big_t (id INT, pad VARCHAR)");
    exec_ok("INSERT INTO small_t VALUES (1), (2), (3)");
    const std::string padding(100, 'q');
    std::string batch;
    for (int i = 1; i <= 300; ++i) {
        if (batch.empty()) {
            batch = "INSERT INTO big_t VALUES ";
        } else {
            batch += ", ";
        }
        batch += "(" + std::to_string(i) + ", '" + padding + "')";
        if (i % 100 == 0) {
            exec_ok(batch);
            batch.clear();
        }
    }
    exec_ok("ANALYZE");

    auto plan = explain("SELECT * FROM small_t JOIN big_t ON small_t.id = big_t.id");
    bool has_join = plan.find("Hash Join") != std::string::npos ||
                    plan.find("Nested Loop") != std::string::npos;
    EXPECT_TRUE(has_join) << plan;
    // The join node carries a cost estimate derived from both table sizes.
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;

    auto qr = exec_ok("SELECT * FROM small_t JOIN big_t ON small_t.id = big_t.id");
    EXPECT_EQ(qr.rows.size(), 3u);
}

// -- AC3: EXPLAIN shows costs ----------------------------------------------------

TEST_F(QA_GDB754, AC3_ExplainShowsCostAndRowEstimatesAfterAnalyze) {
    setup_orders(300);
    exec_ok("ANALYZE orders");

    auto plan = explain("SELECT * FROM orders WHERE customer_id = 5");
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;
    EXPECT_NE(plan.find("rows="), std::string::npos) << plan;
    // The cost annotation uses the "(cost=S..T rows=N)" shape.
    EXPECT_NE(plan.find(".."), std::string::npos) << plan;
}

TEST_F(QA_GDB754, AC3_ExplainPlainSeqScanShowsCostsAfterAnalyze) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1), (2), (3)");
    exec_ok("ANALYZE t");

    auto plan = explain("SELECT * FROM t");
    EXPECT_NE(plan.find("Seq Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;
    EXPECT_NE(plan.find("rows=3"), std::string::npos) << plan;
}

} // anonymous namespace
