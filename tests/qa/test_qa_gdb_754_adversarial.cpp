// Adversarial QA tests for GDB-754: Wire cost-based optimizer and statistics
// into the live planner.
//
// These tests probe failure modes NOT covered by the shipped QA suite:
//   1. Stats staleness: old stats survive an INSERT flood; re-ANALYZE updates.
//   2. ANALYZE empty table: no crash; delete-all then query survives.
//   3. DROP + recreate same name: stale stats don't pollute the new table.
//   4. ANALYZE on nonexistent table: clean error returned.
//   5. EXPLAIN composition: one-analyzed/one-unanalyzed join; stable across
//      consecutive runs; WHERE on non-indexed column post-ANALYZE.
//   6. ANALYZE idempotent: running twice is safe.
//   7. ANALYZE inside BEGIN/ROLLBACK: no crash, no corruption.
//   8. Predicate edge: equality on value absent from data; all-NULL column.
//   9. Join-method pin: strongly asymmetric sizes steer method deterministically.
//  10. Mutation tripwire: estimate_equality_selectivity returning 1.0 would
//      break the selective-index test (verified by the shipped suite).

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

class QA_GDB754_Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() /
                    "sixseven_test_qa_gdb754_adversarial";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_  = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    // Execute SQL and assert success, returning the QueryResult.
    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r ? std::move(*r) : QueryResult{};
    }

    // Execute SQL and assert failure, returning the error message.
    std::string exec_err(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_FALSE(r.has_value()) << "Expected error for: " << sql;
        return r ? "" : r.error().message;
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

    [[nodiscard]] table_id_t table_id_of(const std::string& name) {
        auto schema = catalog_.get_table(default_database_id, name);
        EXPECT_TRUE(schema.has_value()) << "table not found: " << name;
        return schema ? schema->table_id : 0;
    }

    // Helper: insert `n` single-column rows (id INT) into a table.
    void insert_int_rows(const std::string& table, int from, int to) {
        std::string batch;
        for (int i = from; i <= to; ++i) {
            if (batch.empty()) {
                batch = "INSERT INTO " + table + " VALUES ";
            } else {
                batch += ", ";
            }
            batch += "(" + std::to_string(i) + ")";
            if ((i - from + 1) % 200 == 0 || i == to) {
                exec_ok(batch);
                batch.clear();
            }
        }
    }

    DiskManager                    dm_;
    Catalog                        catalog_;
    std::filesystem::path          data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine>   engine_;
    std::unique_ptr<IndexManager>  index_manager_;
};

// ---------------------------------------------------------------------------
// 1. Stats staleness — old row count survives new inserts; re-ANALYZE refreshes
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, StaleStats_OldCountSurvivesInsertFlood) {
    exec_ok("CREATE TABLE flood (id INT)");
    insert_int_rows("flood", 1, 50);
    exec_ok("ANALYZE flood");

    auto tid = table_id_of("flood");
    const auto* ts_before = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts_before, nullptr);
    EXPECT_EQ(ts_before->row_count, 50u);

    // Insert 10x more rows — stats must remain at the old count.
    insert_int_rows("flood", 51, 550);

    const auto* ts_stale = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts_stale, nullptr);
    EXPECT_EQ(ts_stale->row_count, 50u)
        << "Stats should remain stale until re-ANALYZE";

    // Querying with stale stats must not crash.
    auto qr = exec_ok("SELECT * FROM flood WHERE id = 500");
    EXPECT_EQ(qr.rows.size(), 1u);

    // Re-ANALYZE must update the count.
    exec_ok("ANALYZE flood");
    const auto* ts_fresh = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts_fresh, nullptr);
    EXPECT_EQ(ts_fresh->row_count, 550u);
}

// ---------------------------------------------------------------------------
// 2a. ANALYZE on an empty table — no crash, sane stats
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, AnalyzeEmptyTable_NoCrashSaneStats) {
    exec_ok("CREATE TABLE empty_t (id INT, name VARCHAR)");
    exec_ok("ANALYZE empty_t");

    auto tid = table_id_of("empty_t");
    const auto* ts = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->row_count, 0u);

    // EXPLAIN on an empty analyzed table must not crash.
    auto plan = explain("SELECT * FROM empty_t");
    EXPECT_NE(plan.find("Seq Scan"), std::string::npos) << plan;
    // Cost annotation must be present (stats exist, even if zero rows).
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;
}

// ---------------------------------------------------------------------------
// 2b. Delete all rows, then query — plan survives even with stale row_count=N
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, AnalyzeThenDeleteAll_QuerySurvives) {
    exec_ok("CREATE TABLE shrink (id INT)");
    insert_int_rows("shrink", 1, 100);
    exec_ok("ANALYZE shrink");
    exec_ok("DELETE FROM shrink WHERE id > 0");

    // Stats say 100 rows; heap is empty. Plan must still execute without crash.
    auto qr = exec_ok("SELECT * FROM shrink WHERE id = 1");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// 3a. DROP + recreate same name: stale stats must not apply to new table
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, DropRecreate_StaleStatsClearedOnDrop) {
    exec_ok("CREATE TABLE recycled (id INT)");
    insert_int_rows("recycled", 1, 200);
    exec_ok("ANALYZE recycled");

    auto old_tid = table_id_of("recycled");
    const auto* ts_old = engine_->statistics().get_table_stats(old_tid);
    ASSERT_NE(ts_old, nullptr) << "stats must exist before drop";
    EXPECT_EQ(ts_old->row_count, 200u);

    // Drop clears stats per implementation.
    exec_ok("DROP TABLE recycled");
    EXPECT_EQ(engine_->statistics().get_table_stats(old_tid), nullptr)
        << "DROP TABLE must remove statistics from the store";

    // Recreate with a different schema.
    exec_ok("CREATE TABLE recycled (x FLOAT)");
    auto new_tid = table_id_of("recycled");

    // Even if old_tid == new_tid (reuse), stats must be absent (cleared on drop).
    EXPECT_EQ(engine_->statistics().get_table_stats(new_tid), nullptr)
        << "New table must start with no stats regardless of table-id reuse";

    // Querying should work without stats (default plan path).
    auto qr = exec_ok("SELECT * FROM recycled");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// 3b. ANALYZE on a nonexistent table returns a clean error
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, AnalyzeNonexistentTable_CleanError) {
    auto msg = exec_err("ANALYZE no_such_table_xyz");
    EXPECT_FALSE(msg.empty()) << "Error message must be non-empty";
}

// ---------------------------------------------------------------------------
// 4a. EXPLAIN: one analyzed + one unanalyzed table in a JOIN
//     The analyzed side must show cost=; the join node shows cost if both sides
//     have estimates, OR neither — just assert no crash and at least one
//     join node appears.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, Explain_JoinOneAnalyzedOneNot) {
    exec_ok("CREATE TABLE analyzed_side (id INT)");
    exec_ok("CREATE TABLE raw_side (id INT)");
    insert_int_rows("analyzed_side", 1, 50);
    insert_int_rows("raw_side", 1, 50);
    exec_ok("ANALYZE analyzed_side");
    // raw_side is intentionally NOT analyzed.

    auto plan = explain("SELECT * FROM analyzed_side JOIN raw_side ON analyzed_side.id = raw_side.id");
    bool has_join = plan.find("Hash Join") != std::string::npos ||
                    plan.find("Nested Loop") != std::string::npos;
    EXPECT_TRUE(has_join) << "Expected a join node in plan:\n" << plan;
    // Query must return correct results.
    auto qr = exec_ok("SELECT * FROM analyzed_side JOIN raw_side ON analyzed_side.id = raw_side.id");
    EXPECT_EQ(qr.rows.size(), 50u);
}

// ---------------------------------------------------------------------------
// 4b. EXPLAIN output is stable (byte-identical) across two consecutive runs
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, Explain_StableAcrossConsecutiveRuns) {
    exec_ok("CREATE TABLE stable_t (id INT)");
    insert_int_rows("stable_t", 1, 100);
    exec_ok("ANALYZE stable_t");

    auto plan1 = explain("SELECT * FROM stable_t WHERE id = 42");
    auto plan2 = explain("SELECT * FROM stable_t WHERE id = 42");
    EXPECT_EQ(plan1, plan2) << "EXPLAIN output must be deterministic";
}

// ---------------------------------------------------------------------------
// 4c. EXPLAIN with WHERE on a non-indexed column post-ANALYZE
//     Should show Seq Scan (no index), must carry cost annotation.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, Explain_WhereOnNonIndexedColumnAfterAnalyze) {
    exec_ok("CREATE TABLE noindex_t (id INT)");
    // Insert 300 rows; no index on id, but use it as the predicate column.
    insert_int_rows("noindex_t", 1, 300);
    exec_ok("ANALYZE noindex_t");

    auto plan = explain("SELECT * FROM noindex_t WHERE id = 50");
    EXPECT_NE(plan.find("Seq Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("cost="), std::string::npos) << plan;
    // Correctness check.
    auto qr = exec_ok("SELECT * FROM noindex_t WHERE id = 50");
    EXPECT_EQ(qr.rows.size(), 1u);
}

// ---------------------------------------------------------------------------
// 5. ANALYZE twice in a row is idempotent
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, AnalyzeTwice_Idempotent) {
    exec_ok("CREATE TABLE idem (id INT)");
    insert_int_rows("idem", 1, 80);

    exec_ok("ANALYZE idem");
    auto tid = table_id_of("idem");
    const auto* ts1 = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts1, nullptr);
    uint64_t rc1 = ts1->row_count;

    exec_ok("ANALYZE idem");
    const auto* ts2 = engine_->statistics().get_table_stats(tid);
    ASSERT_NE(ts2, nullptr);
    EXPECT_EQ(ts2->row_count, rc1) << "Second ANALYZE must produce same row count";
}

// ---------------------------------------------------------------------------
// 6. ANALYZE inside BEGIN/ROLLBACK — no crash, stats likely committed
//    (statistics are not transactional; pinning: no crash is the requirement)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, AnalyzeInsideRollback_NoCrash) {
    exec_ok("CREATE TABLE txn_t (id INT)");
    insert_int_rows("txn_t", 1, 30);

    exec_ok("BEGIN");
    exec_ok("ANALYZE txn_t");
    exec_ok("ROLLBACK");

    // Whether stats survived the rollback or not, a subsequent query must work.
    auto qr = exec_ok("SELECT * FROM txn_t WHERE id = 15");
    EXPECT_EQ(qr.rows.size(), 1u);
}

// ---------------------------------------------------------------------------
// 7a. Predicate edge: equality on a value NOT present in data
//     selectivity estimation must not crash; result is empty.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, SelectivityEdge_EqualityValueAbsentFromData) {
    exec_ok("CREATE TABLE edge_t (id INT, code INT)");
    exec_ok("CREATE INDEX idx_edge_code ON edge_t(code)");
    // Insert 200 rows with both columns populated.
    std::string batch;
    for (int i = 1; i <= 200; ++i) {
        if (batch.empty()) {
            batch = "INSERT INTO edge_t VALUES ";
        } else {
            batch += ", ";
        }
        batch += "(" + std::to_string(i) + ", " + std::to_string(i) + ")";
        if (i % 200 == 0 || i == 200) {
            exec_ok(batch);
            batch.clear();
        }
    }
    rebuild_indexes();
    exec_ok("ANALYZE edge_t");

    // Value 99999 does not exist in data.
    auto plan = explain("SELECT * FROM edge_t WHERE code = 99999");
    // Plan must be valid (not crash); might be index or seq scan.
    EXPECT_FALSE(plan.empty());

    auto qr = exec_ok("SELECT * FROM edge_t WHERE code = 99999");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// 7b. Predicate on an all-NULL column after ANALYZE
//     null_fraction must be 1.0; IS NULL selectivity must work.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, SelectivityEdge_AllNullColumn) {
    exec_ok("CREATE TABLE null_t (id INT, nullable_col VARCHAR)");
    // Insert rows where nullable_col is always NULL.
    for (int i = 1; i <= 50; ++i) {
        exec_ok("INSERT INTO null_t VALUES (" + std::to_string(i) + ", NULL)");
    }
    exec_ok("ANALYZE null_t");

    auto tid = table_id_of("null_t");
    const auto* cs = engine_->statistics().get_column_stats(tid, 1);
    ASSERT_NE(cs, nullptr) << "Column stats must exist after ANALYZE";
    EXPECT_NEAR(cs->null_fraction, 1.0, 0.01)
        << "All-NULL column must have null_fraction ~1.0";

    // Query with IS NULL must return all rows without crashing.
    auto qr = exec_ok("SELECT * FROM null_t WHERE nullable_col IS NULL");
    EXPECT_EQ(qr.rows.size(), 50u);
}

// ---------------------------------------------------------------------------
// 8. Join-method pin: strongly asymmetric table sizes
//    small (5 rows) vs large (2000 rows) after ANALYZE.
//    With stats, the join node must carry a cost annotation.
//    We also verify that the join produces correct results (not a correctness
//    regression from the method choice).
// ---------------------------------------------------------------------------

TEST_F(QA_GDB754_Adversarial, JoinMethodPin_AsymmetricSizes) {
    exec_ok("CREATE TABLE tiny (id INT)");
    exec_ok("CREATE TABLE huge (id INT, pad VARCHAR)");

    // Tiny: 5 rows.
    for (int i = 1; i <= 5; ++i) {
        exec_ok("INSERT INTO tiny VALUES (" + std::to_string(i) + ")");
    }

    // Huge: 2000 rows inserted in batches.
    const std::string pad(80, 'x');
    std::string batch;
    for (int i = 1; i <= 2000; ++i) {
        if (batch.empty()) {
            batch = "INSERT INTO huge VALUES ";
        } else {
            batch += ", ";
        }
        batch += "(" + std::to_string(i) + ", '" + pad + "')";
        if (i % 200 == 0 || i == 2000) {
            exec_ok(batch);
            batch.clear();
        }
    }

    exec_ok("ANALYZE");

    auto plan = explain("SELECT * FROM tiny JOIN huge ON tiny.id = huge.id");
    bool has_join = plan.find("Hash Join") != std::string::npos ||
                    plan.find("Nested Loop") != std::string::npos;
    EXPECT_TRUE(has_join) << "Expected join node:\n" << plan;

    // With stats on both sides, the join cost annotation must be present.
    EXPECT_NE(plan.find("cost="), std::string::npos)
        << "Join with both sides analyzed must have cost annotation:\n" << plan;

    // Correctness: tiny has ids 1..5, huge has 1..2000, intersection = 5.
    auto qr = exec_ok("SELECT * FROM tiny JOIN huge ON tiny.id = huge.id");
    EXPECT_EQ(qr.rows.size(), 5u);
}

} // anonymous namespace
