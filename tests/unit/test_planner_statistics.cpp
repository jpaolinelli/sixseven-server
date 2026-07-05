// GDB-754: Wire cost-based optimizer and statistics into the live planner.
//
// Covers the wiring seams:
//   - ANALYZE populates the QueryEngine's StatisticsStore (single table + all).
//   - EXPLAIN shows cost estimates once statistics exist; output is unchanged
//     for unanalyzed tables.
//   - Planner access-path decisions change with statistics: selective
//     predicates keep the index scan, unselective predicates switch to a
//     sequential scan.
//   - DROP TABLE removes gathered statistics.

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

class PlannerStatisticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_planner_statistics";
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

    /// Build the live B+ tree indexes from the catalog and attach them.
    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(catalog_, *storage_);
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    /// Flatten the EXPLAIN result rows into a single newline-joined string.
    std::string explain(const std::string& sql) {
        auto qr = exec_ok("EXPLAIN " + sql);
        std::string text;
        for (const auto& row : qr.rows) {
            text += row[0].as_string();
            text += "\n";
        }
        return text;
    }

    /// Create `items` and bulk-insert @p rows rows with distinct ids and a
    /// wide padding column so the heap spans many pages.
    void create_and_fill_items(int rows) {
        exec_ok("CREATE TABLE items (id INT, pad VARCHAR)");
        const std::string padding(120, 'x');
        std::string batch;
        for (int i = 1; i <= rows; ++i) {
            if (batch.empty()) {
                batch = "INSERT INTO items VALUES ";
            } else {
                batch += ", ";
            }
            batch += "(" + std::to_string(i) + ", '" + padding + "')";
            if (i % 100 == 0 || i == rows) {
                exec_ok(batch);
                batch.clear();
            }
        }
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

// -- ANALYZE populates the statistics store ----------------------------------

TEST_F(PlannerStatisticsTest, AnalyzeSingleTablePopulatesStatistics) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'b'), (4, 'c'), (5, 'c')");

    EXPECT_EQ(engine_->statistics().get_table_stats(table_id_of("t")), nullptr);

    auto qr = exec_ok("ANALYZE t");
    EXPECT_EQ(qr.message, "ANALYZE");

    const auto* ts = engine_->statistics().get_table_stats(table_id_of("t"));
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 5u);
    EXPECT_GE(ts->page_count, 1u);
    EXPECT_GT(ts->avg_row_width, 0.0);

    // Column 0 (id) has 5 distinct values; column 1 (name) has 3.
    const auto* id_stats = engine_->statistics().get_column_stats(table_id_of("t"), 0);
    ASSERT_NE(id_stats, nullptr);
    EXPECT_EQ(id_stats->ndistinct, 5u);
    const auto* name_stats = engine_->statistics().get_column_stats(table_id_of("t"), 1);
    ASSERT_NE(name_stats, nullptr);
    EXPECT_EQ(name_stats->ndistinct, 3u);
}

TEST_F(PlannerStatisticsTest, BareAnalyzePopulatesAllTables) {
    exec_ok("CREATE TABLE a (id INT)");
    exec_ok("CREATE TABLE b (id INT)");
    exec_ok("INSERT INTO a VALUES (1), (2)");
    exec_ok("INSERT INTO b VALUES (10)");

    exec_ok("ANALYZE");

    const auto* sa = engine_->statistics().get_table_stats(table_id_of("a"));
    const auto* sb = engine_->statistics().get_table_stats(table_id_of("b"));
    ASSERT_NE(sa, nullptr);
    ASSERT_NE(sb, nullptr);
    EXPECT_EQ(sa->row_count, 2u);
    EXPECT_EQ(sb->row_count, 1u);
}

TEST_F(PlannerStatisticsTest, DropTableRemovesStatistics) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ANALYZE t");
    auto tid = table_id_of("t");
    ASSERT_NE(engine_->statistics().get_table_stats(tid), nullptr);

    exec_ok("DROP TABLE t");
    EXPECT_EQ(engine_->statistics().get_table_stats(tid), nullptr);
}

// GDB-1209: ANALYZE is dispatched end-to-end from QueryEngine::execute (wired
// in GDB-711) into the existing, fully-tested planner::analyze_table(). This
// regression pins that ANALYZE on an unknown table surfaces a proper
// NOT_FOUND error rather than falling through to a generic NOT_IMPLEMENTED /
// "planner does not support this statement type" error or crashing.
TEST_F(PlannerStatisticsTest, AnalyzeNonexistentTableReturnsNotFound) {
    auto result = engine_->execute("ANALYZE no_such_table");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// GDB-1209: VACUUM is likewise dispatched end-to-end (GDB-711) into the real
// reclamation machinery (GDB-969). This regression pins that VACUUM on a live
// table succeeds through the full SQL pipeline (no NOT_IMPLEMENTED) and that
// the table's rows remain intact and queryable afterward.
TEST_F(PlannerStatisticsTest, VacuumTableSucceedsEndToEnd) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    exec_ok("DELETE FROM t WHERE id = 2");

    auto qr = exec_ok("VACUUM t");
    EXPECT_EQ(qr.message, "VACUUM");

    auto after = exec_ok("SELECT * FROM t");
    EXPECT_EQ(after.rows.size(), 2u);
}

// GDB-1209: bare VACUUM (no table name) sweeps every user table and still
// reports success end-to-end.
TEST_F(PlannerStatisticsTest, BareVacuumSucceedsAcrossAllTables) {
    exec_ok("CREATE TABLE a (id INT)");
    exec_ok("CREATE TABLE b (id INT)");
    exec_ok("INSERT INTO a VALUES (1), (2)");
    exec_ok("INSERT INTO b VALUES (10)");

    auto qr = exec_ok("VACUUM");
    EXPECT_EQ(qr.message, "VACUUM");
}

// -- EXPLAIN cost annotations -------------------------------------------------

TEST_F(PlannerStatisticsTest, ExplainShowsCostsOnlyAfterAnalyze) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");

    auto before = explain("SELECT * FROM t WHERE id = 2");
    EXPECT_EQ(before.find("cost="), std::string::npos) << "Unanalyzed table must not show costs:\n"
                                                       << before;

    exec_ok("ANALYZE t");

    auto after = explain("SELECT * FROM t WHERE id = 2");
    EXPECT_NE(after.find("cost="), std::string::npos) << after;
    EXPECT_NE(after.find("rows="), std::string::npos) << after;
}

TEST_F(PlannerStatisticsTest, ExplainPlainScanShowsCostsAfterAnalyze) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1), (2)");
    exec_ok("ANALYZE t");

    auto plan = explain("SELECT * FROM t");
    EXPECT_NE(plan.find("Seq Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;
}

// -- Access path decisions change with statistics ------------------------------

TEST_F(PlannerStatisticsTest, SelectivePredicateUsesIndexScanAfterAnalyze) {
    create_and_fill_items(600);
    exec_ok("CREATE INDEX idx_items_id ON items(id)");
    rebuild_indexes();
    exec_ok("ANALYZE items");

    auto plan = explain("SELECT * FROM items WHERE id = 42");
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;
}

TEST_F(PlannerStatisticsTest, UnselectivePredicateSwitchesToSeqScanAfterAnalyze) {
    create_and_fill_items(600);
    exec_ok("CREATE INDEX idx_items_id ON items(id)");
    rebuild_indexes();

    // Default behavior without statistics: a matching index is always used.
    auto before = explain("SELECT * FROM items WHERE id > 0");
    EXPECT_NE(before.find("Index Scan"), std::string::npos) << before;

    exec_ok("ANALYZE items");

    // id > 0 matches every row — the cost-based optimizer must prefer the
    // sequential scan once it knows the table size and value range.
    auto after = explain("SELECT * FROM items WHERE id > 0");
    EXPECT_NE(after.find("Seq Scan"), std::string::npos) << after;
    EXPECT_EQ(after.find("Index Scan"), std::string::npos) << after;

    // The stats-driven plan still returns correct results.
    auto qr = exec_ok("SELECT * FROM items WHERE id > 595");
    EXPECT_EQ(qr.rows.size(), 5u);
}

TEST_F(PlannerStatisticsTest, UnanalyzedTableKeepsDefaultIndexPlan) {
    create_and_fill_items(200);
    exec_ok("CREATE INDEX idx_items_id ON items(id)");
    rebuild_indexes();

    // Without ANALYZE the planner behaves exactly as before: index scan for
    // any matching predicate and no cost annotations.
    auto plan = explain("SELECT * FROM items WHERE id > 0");
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("cost="), std::string::npos) << plan;
}

// -- Join planning with statistics ---------------------------------------------

TEST_F(PlannerStatisticsTest, JoinPlanAnnotatedWithCostsAfterAnalyze) {
    exec_ok("CREATE TABLE l (id INT, v VARCHAR)");
    exec_ok("CREATE TABLE r (id INT, w VARCHAR)");
    exec_ok("INSERT INTO l VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    exec_ok("INSERT INTO r VALUES (1, 'x'), (2, 'y')");
    exec_ok("ANALYZE");

    auto plan = explain("SELECT * FROM l JOIN r ON l.id = r.id");
    bool has_join = plan.find("Hash Join") != std::string::npos ||
                    plan.find("Nested Loop") != std::string::npos;
    EXPECT_TRUE(has_join) << plan;
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;

    // The join itself executes correctly under the stats-aware plan.
    auto qr = exec_ok("SELECT * FROM l JOIN r ON l.id = r.id");
    EXPECT_EQ(qr.rows.size(), 2u);
}

} // anonymous namespace
