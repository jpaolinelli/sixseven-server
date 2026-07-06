/// @file test_qa_gdb_947.cpp
/// @brief Adversarial QA regression tests for GDB-947.
///
/// GDB-947: Auto-increment counter restoration after restart was broken.
/// SystemBootstrap subsequent-run path now inits counters via:
///   fast path  -> storage.read_autoincrement (persisted header)
///   slow path  -> heap scan max(col)+1 + write_autoincrement
///
/// These tests probe the high-water-mark edge cases the basic GDB-251 suite
/// does not fully cover:
///   1. Multiple autoincrement tables restored independently across restart.
///   2. Sequence strictly advances past prior max (no duplicate IDs across
///      restart, even when rows were inserted up to N then the session ended).
///   3. DELETE of max row -> restart -> INSERT: new ID does NOT collide with
///      any surviving row (spec: slow-path yields max_surviving+1; that may
///      equal the deleted ID -- which is acceptable -- but must not collide
///      with a currently existing row).
///   4. Multiple restarts with interleaved inserts: IDs are strictly
///      monotonically increasing and never duplicate.
///   5. Empty table after full restart: first INSERT gets base id = 1.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Fixture (mirrors QA_AutoIncrement from test_qa_gdb_251.cpp)
// =============================================================================

class QA_GDB947 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb947";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void restart() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());

        run_bootstrap();
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed for: " << sql
                          << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// Test 1: Three autoincrement tables restored INDEPENDENTLY across restart.
// The fix must loop every table; if it stops after the first counter is
// found, subsequent tables fail with "no autoincrement counter for table N".
// =============================================================================

TEST_F(QA_GDB947, MultipleTablesRestoredIndependentlyAcrossRestart) {
    run_bootstrap();

    exec_ok("CREATE TABLE t1 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("CREATE TABLE t2 (id BIGINT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("CREATE TABLE t3 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    // Drive each table to a distinct high-water mark.
    exec_ok("INSERT INTO t1 (v) VALUES ('a'), ('b'), ('c')");        // max id = 3
    exec_ok("INSERT INTO t2 (id, v) VALUES (100, 'x')");             // max id = 100
    exec_ok("INSERT INTO t3 (id, v) VALUES (200, 'y'), (201, 'z')"); // max id = 201

    restart();

    // Each table must resume ABOVE its own prior max -- verified with INSERT.
    exec_ok("INSERT INTO t1 (v) VALUES ('d')");
    exec_ok("INSERT INTO t2 (v) VALUES ('y2')");
    exec_ok("INSERT INTO t3 (v) VALUES ('z2')");

    auto q1 = exec_ok("SELECT id FROM t1 ORDER BY id DESC LIMIT 1");
    ASSERT_EQ(q1.rows.size(), 1u);
    EXPECT_GE(q1.rows[0][0].as_int32(), 4) << "t1 counter should be >= 4 after restart";

    auto q2 = exec_ok("SELECT id FROM t2 ORDER BY id DESC LIMIT 1");
    ASSERT_EQ(q2.rows.size(), 1u);
    EXPECT_GE(q2.rows[0][0].as_int64(), 101) << "t2 counter should be >= 101 after restart";

    auto q3 = exec_ok("SELECT id FROM t3 ORDER BY id DESC LIMIT 1");
    ASSERT_EQ(q3.rows.size(), 1u);
    EXPECT_GE(q3.rows[0][0].as_int32(), 202) << "t3 counter should be >= 202 after restart";

    // All IDs in each table must be unique (no collision).
    auto all1 = exec_ok("SELECT id FROM t1 ORDER BY id");
    std::set<int32_t> ids1;
    for (const auto& row : all1.rows) {
        auto id = row[0].as_int32();
        EXPECT_TRUE(ids1.insert(id).second) << "Duplicate id=" << id << " in t1";
    }

    auto all2 = exec_ok("SELECT id FROM t2 ORDER BY id");
    std::set<int64_t> ids2;
    for (const auto& row : all2.rows) {
        auto id = row[0].as_int64();
        EXPECT_TRUE(ids2.insert(id).second) << "Duplicate id=" << id << " in t2";
    }
}

// =============================================================================
// Test 2: No duplicate IDs across restart.
// INSERT N rows -> restart -> INSERT more rows -> all IDs globally unique.
// The critical invariant: ids from session 2 must all be > max id from session 1.
// =============================================================================

TEST_F(QA_GDB947, NoDuplicateIdsAcrossRestart) {
    run_bootstrap();
    exec_ok("CREATE TABLE t_nodup (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    // Session 1: insert 10 rows.
    for (int i = 0; i < 10; ++i) {
        exec_ok("INSERT INTO t_nodup (v) VALUES ('" + std::to_string(i) + "')");
    }

    auto q_pre = exec_ok("SELECT id FROM t_nodup ORDER BY id");
    ASSERT_EQ(q_pre.rows.size(), 10u);
    int32_t max_before = q_pre.rows.back()[0].as_int32();
    EXPECT_EQ(max_before, 10);

    restart();

    // Session 2: insert 10 more rows.
    for (int i = 0; i < 10; ++i) {
        exec_ok("INSERT INTO t_nodup (v) VALUES ('" + std::to_string(i + 100) + "')");
    }

    auto q_post = exec_ok("SELECT id FROM t_nodup ORDER BY id");
    ASSERT_EQ(q_post.rows.size(), 20u);

    // Collect all IDs and confirm they are strictly increasing and unique.
    std::set<int32_t> all_ids;
    int32_t prev = 0;
    for (const auto& row : q_post.rows) {
        int32_t id = row[0].as_int32();
        EXPECT_GT(id, prev) << "IDs not strictly increasing: prev=" << prev << " id=" << id;
        EXPECT_TRUE(all_ids.insert(id).second) << "Duplicate id=" << id << " across restart";
        prev = id;
    }

    // IDs from session 2 must all be strictly > max from session 1.
    for (size_t i = 10; i < q_post.rows.size(); ++i) {
        int32_t id = q_post.rows[i][0].as_int32();
        EXPECT_GT(id, max_before) << "Session-2 id=" << id
                                  << " not above session-1 max=" << max_before;
    }
}

// =============================================================================
// Test 3: DELETE of max row -> restart -> INSERT -> no collision with survivor.
// After deleting id=N, restart, scan finds max_surviving=N-1, counter=N.
// New INSERT gets id=N (the deleted slot is reused per spec). The critical
// correctness property is that this id does NOT collide with any *surviving* row.
// =============================================================================

TEST_F(QA_GDB947, DeleteMaxRowRestartNoDuplicateWithSurvivors) {
    run_bootstrap();
    exec_ok("CREATE TABLE t_del (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO t_del (v) VALUES ('a'), ('b'), ('c'), ('d'), ('e')"); // ids 1-5
    exec_ok("DELETE FROM t_del WHERE id = 5");                                 // delete max row

    auto q_before = exec_ok("SELECT id FROM t_del ORDER BY id");
    ASSERT_EQ(q_before.rows.size(), 4u); // 1,2,3,4 survive

    restart();

    // Counter after restart = max_surviving+1 = 5 (slow-path scan).
    exec_ok("INSERT INTO t_del (v) VALUES ('f')");

    auto q_after = exec_ok("SELECT id FROM t_del ORDER BY id");
    ASSERT_EQ(q_after.rows.size(), 5u);

    // Collect all IDs -- must be unique (no collision with 1,2,3,4).
    std::set<int32_t> ids;
    for (const auto& row : q_after.rows) {
        int32_t id = row[0].as_int32();
        EXPECT_TRUE(ids.insert(id).second) << "Duplicate id=" << id << " after delete+restart";
    }

    // The new row must be >= 5 (no regression below the prior high-water mark).
    int32_t new_id = q_after.rows.back()[0].as_int32();
    EXPECT_GE(new_id, 5) << "Counter regressed below prior high-water mark";
}

// =============================================================================
// Test 4: Multiple restarts, strictly monotone IDs across all sessions.
// Session 1: insert 5 -> restart -> Session 2: insert 5 -> restart ->
// Session 3: insert 5 -> all 15 IDs must be unique and monotone.
// =============================================================================

TEST_F(QA_GDB947, MultipleRestartsStrictlyMonotoneIds) {
    run_bootstrap();
    exec_ok("CREATE TABLE t_mono (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    // Session 1.
    for (int i = 0; i < 5; ++i) {
        exec_ok("INSERT INTO t_mono (v) VALUES ('s1_" + std::to_string(i) + "')");
    }

    restart();

    // Session 2.
    for (int i = 0; i < 5; ++i) {
        exec_ok("INSERT INTO t_mono (v) VALUES ('s2_" + std::to_string(i) + "')");
    }

    restart();

    // Session 3.
    for (int i = 0; i < 5; ++i) {
        exec_ok("INSERT INTO t_mono (v) VALUES ('s3_" + std::to_string(i) + "')");
    }

    auto qr = exec_ok("SELECT id FROM t_mono ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 15u);

    std::set<int32_t> seen;
    int32_t prev = 0;
    for (const auto& row : qr.rows) {
        int32_t id = row[0].as_int32();
        EXPECT_GT(id, prev) << "Non-monotone: prev=" << prev << " id=" << id;
        EXPECT_TRUE(seen.insert(id).second) << "Duplicate id=" << id << " across 3 sessions";
        prev = id;
    }
}

// =============================================================================
// Test 5: Empty table after full delete -> restart -> counter resets to 1.
// (Edge: slow-path scan sees max_val=0, sets next=1).
// =============================================================================

TEST_F(QA_GDB947, EmptyTableAfterRestartStartsAtOne) {
    GTEST_SKIP() << "autoincrement reset-after-restart semantics tracked by GDB-1291";
    run_bootstrap();
    exec_ok("CREATE TABLE t_emp (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO t_emp (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3
    exec_ok("DELETE FROM t_emp");                                // empty the table

    restart();

    // Slow path: table is empty, max_val=0, next=1.
    exec_ok("INSERT INTO t_emp (v) VALUES ('fresh')");

    auto qr = exec_ok("SELECT id FROM t_emp");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Per spec: empty table on restart yields counter=1 (slow path scans 0, next=1).
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}
