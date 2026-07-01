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

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// GDB-1153 adversarial QA fixture
// Counter must be durably persisted on every INSERT so a no-flush restart
// never reuses a previously-issued ID.
// =============================================================================

class QA_GDB1153 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb_1153";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        init_stack();
    }

    void TearDown() override {
        teardown_stack();
        std::filesystem::remove_all(data_dir_);
    }

    void init_stack() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void teardown_stack() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    // Simulates crash restart (no explicit flush).
    // Durability must come from the per-INSERT write, not an incidental flush.
    void restart() {
        teardown_stack();
        init_stack();
        run_bootstrap();
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed: " << sql << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    void exec_should_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error but got success for: " << sql;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// =============================================================================
// AC1: Persisted counter is preferred over max-scan fallback on restart.
// The three tests below were added by the implementer (GDB-1153). They are
// exercised here as acceptance-criterion smoke checks to confirm they pass
// with the fix wired in.
// =============================================================================

TEST_F(QA_GDB1153, AC1_DeleteSingleMaxRow_NextIdNotReused) {
    run_bootstrap();
    exec_ok("CREATE TABLE t1 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t1 (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3
    exec_ok("DELETE FROM t1 WHERE id = 3");

    restart();

    exec_ok("INSERT INTO t1 (v) VALUES ('d')");
    auto qr = exec_ok("SELECT id FROM t1 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 4); // 3 was deleted, must not be reused
}

TEST_F(QA_GDB1153, AC1_DeleteAllRows_NextIdContinuesFromPersistedCounter) {
    run_bootstrap();
    exec_ok("CREATE TABLE t2 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t2 (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3
    exec_ok("DELETE FROM t2");                                // empty table

    restart();

    exec_ok("INSERT INTO t2 (v) VALUES ('fresh')");
    auto qr = exec_ok("SELECT id FROM t2");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 4); // heap scan would yield 1 -- must NOT
}

// =============================================================================
// Adversarial: Interleaved inserts / deletes / multiple restarts
// =============================================================================

TEST_F(QA_GDB1153, MultipleRestarts_CounterStable_NeverReuses) {
    // insert 1..3, restart, insert 4, restart, insert 5, restart, insert 6
    // -- each restart must see the counter from the previous persist
    run_bootstrap();
    exec_ok("CREATE TABLE t3 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t3 (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3

    restart();
    exec_ok("INSERT INTO t3 (v) VALUES ('d')"); // id=4

    restart();
    exec_ok("INSERT INTO t3 (v) VALUES ('e')"); // id=5

    restart();
    exec_ok("INSERT INTO t3 (v) VALUES ('f')"); // id=6

    auto qr = exec_ok("SELECT id FROM t3 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 6u);
    EXPECT_EQ(qr.rows[5][0].as_int32(), 6);
}

TEST_F(QA_GDB1153, MultipleRestartsNoInsertsBetween_CounterStable) {
    // Restart several times with NO inserts between. Counter must remain stable.
    run_bootstrap();
    exec_ok("CREATE TABLE t4 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t4 (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3 -> counter=4

    restart();
    restart();
    restart(); // three no-insert restarts

    exec_ok("INSERT INTO t4 (v) VALUES ('d')");
    auto qr = exec_ok("SELECT id FROM t4 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(qr.rows[3][0].as_int32(), 4); // counter must be 4, not 1
}

TEST_F(QA_GDB1153, InterleavedInsertDeleteRestartCycle) {
    // More complex interleave: insert, delete top, restart, insert, delete top, restart, insert
    run_bootstrap();
    exec_ok("CREATE TABLE t5 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t5 (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3
    exec_ok("DELETE FROM t5 WHERE id = 3");

    restart(); // counter persisted as 4

    exec_ok("INSERT INTO t5 (v) VALUES ('d'), ('e')"); // ids 4,5
    exec_ok("DELETE FROM t5 WHERE id = 5");

    restart(); // counter persisted as 6

    exec_ok("INSERT INTO t5 (v) VALUES ('f')"); // id must be 6
    auto qr = exec_ok("SELECT id FROM t5 ORDER BY id");
    // surviving rows: 1,2,4,6
    ASSERT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 4);
    EXPECT_EQ(qr.rows[3][0].as_int32(), 6);
}

// =============================================================================
// Adversarial: INSERT ... SELECT path must also persist
// Reviewer noted storage_manager_ is wired at planner.cpp:2592 (SELECT path).
// Verify behaviorally that INSERT...SELECT also persists the counter durably.
//
// NOTE: INSERT INTO dst(v) SELECT v FROM src (explicit column list omitting the
// autoincrement column) is a separate known limitation -- InsertOperator's
// child/SELECT path does not auto-fill autoincrement columns for unmapped slots;
// it only applies the NOT NULL guard (see insert.cpp:131-139 vs 210+ which is
// VALUES-only). That limitation is filed separately. This test uses the
// no-explicit-column-list form (INSERT INTO dst SELECT id, v FROM src2) where
// the SELECT explicitly provides the autoincrement column, which exercises the
// GDB-1153 persist path without hitting that limitation.
// =============================================================================

TEST_F(QA_GDB1153, InsertSelectPath_CounterPersistedDurably) {
    // Source table provides explicit id values; dst receives them via INSERT...SELECT.
    // After the INSERT the counter must be persisted so a restart does not reuse.
    //
    // BUG PROBE: The child (SELECT) path in InsertOperator::do_next() does NOT call
    // next_autoincrement() or advance_autoincrement(), so the in-memory counter is
    // never bumped for explicit-id INSERT...SELECT. The persist guard (counter > 0)
    // then skips the write. On restart, max-scan is used instead, and if the max
    // row was deleted, its ID gets reused.
    //
    // This test FAILS on the current GDB-1153 implementation, proving the bug.
    run_bootstrap();
    exec_ok("CREATE TABLE src2 (sid INT, v VARCHAR)");
    exec_ok("INSERT INTO src2 (sid, v) VALUES (10, 'x'), (20, 'y'), (30, 'z')");
    exec_ok("CREATE TABLE dst2 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    // INSERT...SELECT providing explicit id values.
    exec_ok("INSERT INTO dst2 (id, v) SELECT sid, v FROM src2");

    // Delete id=30 (max) so max-scan fallback would yield 30 again.
    exec_ok("DELETE FROM dst2 WHERE id = 30");

    restart(); // no-flush -- durability from per-INSERT persist write

    exec_ok("INSERT INTO dst2 (v) VALUES ('new')");
    auto qr = exec_ok("SELECT id FROM dst2 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    // BUG: Counter was NOT persisted by INSERT...SELECT path, so max-scan yields 21.
    // CORRECT: Counter should be 31 (advance_autoincrement was called with max explicit id=30).
    // When the bug is fixed, this assertion will pass:
    EXPECT_EQ(qr.rows[2][0].as_int32(), 31)
        << "INSERT...SELECT path does not advance or persist the autoincrement counter "
           "when explicit id values are provided. On restart, max-scan is used instead, "
           "yielding max(10,20)+1=21 or lower if rows were deleted. Bug: insert.cpp "
           "child-path (lines 108-173) never calls advance_autoincrement().";
}

// Adversarial: INSERT...SELECT with omitted autoincrement column + explicit list
// hits a known limitation: the unmapped AI column is treated as NOT NULL and
// rejected instead of auto-filled. This test documents the behavior so a future
// fix is visible.
TEST_F(QA_GDB1153, InsertSelectPath_OmittedAutoincrementColumn_NotYetSupported) {
    // This form -- INSERT INTO dst(v) SELECT v FROM src -- should auto-fill id,
    // but currently fails with NOT NULL constraint. Document the behavior here.
    // If this test starts PASSING, remove the EXPECT_FALSE and verify ID values.
    run_bootstrap();
    exec_ok("CREATE TABLE src_omit (v VARCHAR)");
    exec_ok("INSERT INTO src_omit (v) VALUES ('a'), ('b')");
    exec_ok("CREATE TABLE dst_omit (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    auto result = engine_->execute("INSERT INTO dst_omit (v) SELECT v FROM src_omit");
    // Currently fails: autoincrement not auto-filled in INSERT...SELECT explicit-column path.
    // When fixed, this should succeed and produce ids 1,2.
    if (result.has_value()) {
        // Fix landed -- verify correctness.
        auto qr = exec_ok("SELECT id FROM dst_omit ORDER BY id");
        ASSERT_EQ(qr.rows.size(), 2u);
        EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
        EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    } else {
        // Known limitation: CONSTRAINT_VIOLATION on unmapped autoincrement column.
        EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION)
            << "Expected NOT NULL constraint violation for unmapped autoincrement column, got: "
            << result.error().message;
    }
}

// =============================================================================
// Adversarial: Explicit high-value INSERT advances counter; restart continues above it
// =============================================================================

TEST_F(QA_GDB1153, ExplicitHighValueInsert_RestartContinuesAboveExplicitMax) {
    run_bootstrap();
    exec_ok("CREATE TABLE t6 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    // Explicit insert at id=1000; auto-counter must advance to 1001
    exec_ok("INSERT INTO t6 (id, v) VALUES (1000, 'hi')");
    exec_ok("DELETE FROM t6 WHERE id = 1000"); // delete so scan fallback sees nothing

    restart();

    exec_ok("INSERT INTO t6 (v) VALUES ('auto')");
    auto qr = exec_ok("SELECT id FROM t6");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Must be 1001, not 1 (what max-scan fallback from empty table would give)
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1001);
}

TEST_F(QA_GDB1153, ExplicitHighValueInsert_NoDeleteNoRestart_AutoContinues) {
    // No restart: explicit id=500, then auto should be 501.
    run_bootstrap();
    exec_ok("CREATE TABLE t7 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t7 (id, v) VALUES (500, 'explicit')");
    exec_ok("INSERT INTO t7 (v) VALUES ('auto')");

    auto qr = exec_ok("SELECT id FROM t7 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 500);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 501);
}

// =============================================================================
// Adversarial: BIGINT autoincrement column survives restart
// =============================================================================

TEST_F(QA_GDB1153, BigintAutoincrement_SurvivesRestart) {
    run_bootstrap();
    exec_ok("CREATE TABLE t8 (id BIGINT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t8 (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3
    exec_ok("DELETE FROM t8 WHERE id = 3");

    restart();

    exec_ok("INSERT INTO t8 (v) VALUES ('d')");
    auto qr = exec_ok("SELECT id FROM t8 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[2][0].as_int64(), 4); // must not reuse 3
}

TEST_F(QA_GDB1153, BigintAutoincrement_LargeExplicitValue_SurvivesRestart) {
    run_bootstrap();
    exec_ok("CREATE TABLE t9 (id BIGINT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    int64_t big = 9000000000LL;
    exec_ok("INSERT INTO t9 (id, v) VALUES (" + std::to_string(big) + ", 'big')");
    exec_ok("DELETE FROM t9 WHERE id = " + std::to_string(big));

    restart();

    exec_ok("INSERT INTO t9 (v) VALUES ('next')");
    auto qr = exec_ok("SELECT id FROM t9");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), big + 1);
}

// =============================================================================
// Adversarial: Multiple autoincrement tables are independent and each durable
// =============================================================================

TEST_F(QA_GDB1153, TwoTables_IndependentCounters_BothDurableAcrossRestart) {
    run_bootstrap();
    exec_ok("CREATE TABLE ta (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("CREATE TABLE tb (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO ta (v) VALUES ('a1'), ('a2'), ('a3')"); // ids 1,2,3
    exec_ok("INSERT INTO tb (v) VALUES ('b1'), ('b2')");         // ids 1,2

    exec_ok("DELETE FROM ta WHERE id = 3");
    exec_ok("DELETE FROM tb WHERE id = 2");

    restart();

    exec_ok("INSERT INTO ta (v) VALUES ('a4')");
    exec_ok("INSERT INTO tb (v) VALUES ('b3')");

    auto qa = exec_ok("SELECT id FROM ta ORDER BY id");
    ASSERT_EQ(qa.rows.size(), 3u);
    EXPECT_EQ(qa.rows[2][0].as_int32(), 4); // ta counter was 4

    auto qb = exec_ok("SELECT id FROM tb ORDER BY id");
    ASSERT_EQ(qb.rows.size(), 2u);
    EXPECT_EQ(qb.rows[1][0].as_int32(), 3); // tb counter was 3
}

TEST_F(QA_GDB1153, ThreeTables_AllDurableIndependently) {
    run_bootstrap();
    exec_ok("CREATE TABLE tc1 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("CREATE TABLE tc2 (id BIGINT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("CREATE TABLE tc3 (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO tc1 (v) VALUES ('a')");         // id=1
    exec_ok("INSERT INTO tc2 (v) VALUES ('x'), ('y')");  // ids 1,2
    exec_ok("INSERT INTO tc3 (id, v) VALUES (99, 'z')"); // explicit id=99

    exec_ok("DELETE FROM tc1 WHERE id = 1");
    exec_ok("DELETE FROM tc2 WHERE id = 2");
    exec_ok("DELETE FROM tc3 WHERE id = 99");

    restart();

    exec_ok("INSERT INTO tc1 (v) VALUES ('new1')");
    exec_ok("INSERT INTO tc2 (v) VALUES ('new2')");
    exec_ok("INSERT INTO tc3 (v) VALUES ('new3')");

    auto q1 = exec_ok("SELECT id FROM tc1");
    ASSERT_EQ(q1.rows.size(), 1u);
    EXPECT_EQ(q1.rows[0][0].as_int32(), 2); // counter was 2

    auto q2 = exec_ok("SELECT id FROM tc2");
    ASSERT_EQ(q2.rows.size(), 2u); // row 1 still exists
    auto q2last = exec_ok("SELECT id FROM tc2 ORDER BY id DESC LIMIT 1");
    EXPECT_EQ(q2last.rows[0][0].as_int64(), 3); // counter was 3

    auto q3 = exec_ok("SELECT id FROM tc3");
    ASSERT_EQ(q3.rows.size(), 1u);
    EXPECT_EQ(q3.rows[0][0].as_int32(), 100); // counter was 100
}

// =============================================================================
// Adversarial: Edge -- table created but never inserted; restart; first insert = 1
// =============================================================================

TEST_F(QA_GDB1153, TableCreatedNeverInserted_RestartFirstInsertIsOne) {
    run_bootstrap();
    exec_ok("CREATE TABLE t_empty_create (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    restart(); // no inserts before restart

    exec_ok("INSERT INTO t_empty_create (v) VALUES ('first')");
    auto qr = exec_ok("SELECT id FROM t_empty_create");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// =============================================================================
// Adversarial: Crash-fidelity -- confirm restart() does NOT flush
// (test that durability comes from the per-INSERT write, not an incidental flush)
// We verify this by observing that even after deleting all rows (making the
// max-scan fallback return 0+1=1), the restart still yields the correct ID.
// =============================================================================

TEST_F(QA_GDB1153, CrashFidelity_DurabilityFromPerInsertWrite_NotFlush) {
    run_bootstrap();
    exec_ok("CREATE TABLE t_crash (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO t_crash (v) VALUES ('a'), ('b')"); // ids 1,2 -> counter=3

    // Delete ALL rows: max-scan fallback would see empty table -> yield id=1
    exec_ok("DELETE FROM t_crash");

    restart(); // no explicit flush called before restart

    exec_ok("INSERT INTO t_crash (v) VALUES ('post-crash')");
    auto qr = exec_ok("SELECT id FROM t_crash");
    ASSERT_EQ(qr.rows.size(), 1u);
    // If durability came only from a flush (which did not occur), we'd see id=1.
    // The per-INSERT write ensures we see id=3.
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

// =============================================================================
// Regression: existing QA_AutoIncrement.StressInsert500Rows still green
// (per-statement persist must not cause unacceptable slowdown -- test completes)
// =============================================================================

TEST_F(QA_GDB1153, Regression_StressInsert_PerStatementPersistIsAcceptable) {
    // 100 single-row INSERTs, each triggering a per-statement persist write.
    // Test verifies correctness + that performance is still acceptable
    // (test would time-out under a regression).
    run_bootstrap();
    exec_ok("CREATE TABLE t_perf (id INT PRIMARY KEY AUTOINCREMENT, v INT)");

    for (int i = 0; i < 100; ++i) {
        exec_ok("INSERT INTO t_perf (v) VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT COUNT(*) FROM t_perf");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 100);

    auto last = exec_ok("SELECT id FROM t_perf ORDER BY id DESC LIMIT 1");
    ASSERT_EQ(last.rows.size(), 1u);
    EXPECT_EQ(last.rows[0][0].as_int32(), 100);
}

// =============================================================================
// Regression: INT32 max-boundary -- no overflow/UB introduced by the persist path
// =============================================================================

TEST(QA_GDB1153_CatalogUnit, CounterNearInt32Max_NoOverflow) {
    // Verify that advance_autoincrement near INT32_MAX does not wrap or UB.
    Catalog catalog;
    int64_t near_max = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) - 1;
    catalog.init_autoincrement(1, near_max);

    auto v1 = catalog.next_autoincrement(1, TypeId::INT32);
    ASSERT_TRUE(v1.has_value()) << v1.error().message;
    EXPECT_EQ(*v1, near_max);

    // One more -- should reach INT32_MAX exactly.
    auto v2 = catalog.next_autoincrement(1, TypeId::INT32);
    ASSERT_TRUE(v2.has_value()) << v2.error().message;
    EXPECT_EQ(*v2, static_cast<int64_t>(std::numeric_limits<int32_t>::max()));

    // Next must be overflow (CONSTRAINT_VIOLATION).
    auto v3 = catalog.next_autoincrement(1, TypeId::INT32);
    EXPECT_FALSE(v3.has_value());
    EXPECT_EQ(v3.error().code, StatusCode::CONSTRAINT_VIOLATION);
}
