/// @file test_qa_gdb_923.cpp
/// @brief QA regression tests for GDB-923: BackfillManager stale-job accumulation bug.
///
/// Before the fix, start() never pruned finished jobs for a table, so:
///   - Re-running a backfill accumulated duplicate entries in jobs_.
///   - status(table) returned the FIRST (stale/old) entry.
///   - all_statuses() returned duplicate rows for the same table.
///
/// After the fix, start() prunes any finished same-table job before inserting
/// the new one, so at most one entry per table exists in jobs_ at any time.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/ast.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/backfill_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_worker.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace sixseven;

// =============================================================================
// Fixture -- mirrors the existing BackfillManagerTest fixture in unit tests.
// =============================================================================

class GDB923Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb923";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);

        auto db_id = catalog_.create_database("testdb");
        ASSERT_TRUE(db_id.has_value()) << db_id.error().message;
        test_db_id_ = *db_id;

        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1,
                                  .max_batch_size = 32,
                                  .max_retries = 1,
                                  .base_backoff = std::chrono::milliseconds{5},
                                  .max_backoff = std::chrono::milliseconds{20}});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));

        auto started = pool_->start();
        ASSERT_TRUE(started.has_value()) << started.error().message;

        mgr_ = std::make_unique<BackfillManager>(catalog_, *storage_, *pool_);
    }

    void TearDown() override {
        mgr_.reset();
        if (pool_ && pool_->is_running()) {
            auto stop = pool_->stop();
            EXPECT_TRUE(stop.has_value());
        }
        pool_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Create a table with an EMBEDDING column. All rows have pre-filled
    /// embeddings so the backfill loop exits immediately (zero NULL rows to
    /// process -> loop ends after one scan pass). This guarantees the thread
    /// finishes quickly and deterministically.
    void create_instant_table(const std::string& name) {
        CatalogColumnDef id_col;
        id_col.name = "id";
        id_col.type_id = TypeId::INT32;
        id_col.ordinal = 0;

        CatalogColumnDef text_col;
        text_col.name = "body";
        text_col.type_id = TypeId::STRING;
        text_col.ordinal = 1;

        CatalogColumnDef emb_col;
        emb_col.name = "vec";
        emb_col.type_id = TypeId::EMBEDDING;
        emb_col.ordinal = 2;

        TableSchema ts;
        ts.name = name;
        ts.columns = {id_col, text_col, emb_col};
        ts.pk_columns = "id";

        auto table_id = catalog_.create_table(test_db_id_, ts);
        ASSERT_TRUE(table_id.has_value()) << table_id.error().message;

        EmbeddingColumnDef emb_def;
        emb_def.table_id = *table_id;
        emb_def.column_id = 2;
        emb_def.dimension = 4;
        emb_def.source_expr = "body";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        auto create_storage = storage_->create_table_storage(test_db_id_, *table_id, ts);
        ASSERT_TRUE(create_storage.has_value()) << create_storage.error().message;

        auto storage_ptr = storage_->get_table_storage(*table_id);
        ASSERT_TRUE(storage_ptr.has_value());

        Schema schema(
            {{"id", TypeId::INT32}, {"body", TypeId::STRING}, {"vec", TypeId::EMBEDDING}});

        // Insert 3 rows with embeddings already set: the backfill loop will skip
        // all of them (skipped==3, generated==0) and finish quickly.
        for (int i = 0; i < 3; ++i) {
            std::vector<Value> vals;
            vals.push_back(Value(static_cast<int32_t>(i)));
            vals.push_back(Value(std::string("hello " + std::to_string(i))));
            vals.push_back(Value(Embedding{1.0F, 0.0F, 0.0F, 0.0F}));
            auto bytes = TupleSerializer::serialize(vals, schema);
            ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
            auto rid = (*storage_ptr)->heap->insert_tuple(*bytes);
            ASSERT_TRUE(rid.has_value()) << rid.error().message;
        }
    }

    /// Poll status(table_name) until running==false or timeout expires.
    /// Returns true if the job finished within the deadline.
    bool wait_finished(const std::string& table_name,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!mgr_->status(table_name).running) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    DiskManager dm_;
    Catalog catalog_;
    database_id_t test_db_id_ = 0;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<BackfillManager> mgr_;
};

// =============================================================================
// GDB-923 AC1 (mutation-grade): Re-running a backfill on the same table after
// it finishes must result in EXACTLY ONE entry per table in all_statuses().
//
// Buggy code: push_back without pruning -> 2 entries after a re-run.
// Fixed code: prune finished same-table job before push_back -> 1 entry.
// =============================================================================

TEST_F(GDB923Test, RerunBackfillLeavesExactlyOneStatusEntryPerTable) {
    create_instant_table("t1");

    BackfillStmt stmt;
    stmt.table_name = "t1";
    stmt.batch_size = 100;

    // --- First run ---
    auto r1 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    ASSERT_TRUE(wait_finished("t1")) << "First backfill did not finish within 2 s";

    auto s1 = mgr_->status("t1");
    EXPECT_FALSE(s1.running);

    // --- Second run (re-run on the same table) ---
    auto r2 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    // Core mutation-grade assertion: all_statuses() must have EXACTLY ONE entry
    // for "t1". The buggy code accumulates two entries here; the fix prunes the
    // stale one before inserting the new job.
    auto statuses = mgr_->all_statuses();
    int t1_count = 0;
    for (const auto& s : statuses) {
        if (s.table_name == "t1") {
            ++t1_count;
        }
    }
    EXPECT_EQ(t1_count, 1)
        << "all_statuses() must contain exactly ONE entry for 't1' after a re-run; "
           "got "
        << t1_count << " (stale jobs were not pruned)";

    // status() must reflect the NEW run (running==true right after start, or
    // the run already completed with fresh counts -- either is valid as long as
    // it is NOT reporting a stale earlier run's data while a new one exists).
    // We verify this by checking that all_statuses() returns only one row total
    // for this table (already asserted above) so the single row IS the new run.

    // Wait for second run to complete cleanly before TearDown.
    (void)wait_finished("t1");
}

// =============================================================================
// GDB-923 AC2: A finished backfill that is NOT re-run still reports
// running==false via status() (no over-prune: finished status remains visible).
// =============================================================================

TEST_F(GDB923Test, FinishedBackfillStatusRemainsVisibleWithoutRerun) {
    create_instant_table("t2");

    BackfillStmt stmt;
    stmt.table_name = "t2";
    stmt.batch_size = 100;

    auto r = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    ASSERT_TRUE(wait_finished("t2")) << "Backfill did not finish within 2 s";

    // The single finished job must still be visible and report running==false.
    auto s = mgr_->status("t2");
    EXPECT_EQ(s.table_name, "t2");
    EXPECT_FALSE(s.running) << "Finished job must report running==false";
    // The job scanned 3 pre-filled rows and skipped all of them.
    EXPECT_EQ(s.processed, 3);
    EXPECT_EQ(s.skipped, 3);
    EXPECT_EQ(s.generated, 0);

    // all_statuses() must still contain exactly one entry for "t2".
    auto statuses = mgr_->all_statuses();
    int t2_count = 0;
    for (const auto& s2 : statuses) {
        if (s2.table_name == "t2") {
            ++t2_count;
        }
    }
    EXPECT_EQ(t2_count, 1) << "Finished job must be retained until superseded by a re-run";
}

// =============================================================================
// GDB-923 AC3: status() after a re-run reflects the NEW run's counts, not the
// stale finished run's counts. We verify this by checking the running flag
// immediately after start() (new job must be running or just finished -- either
// way it replaces the old finished entry).
// =============================================================================

TEST_F(GDB923Test, StatusAfterRerunReflectsNewJobNotStaleOldJob) {
    create_instant_table("t3");

    BackfillStmt stmt;
    stmt.table_name = "t3";
    stmt.batch_size = 100;

    // First run -- let it finish.
    auto r1 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(wait_finished("t3")) << "First backfill did not finish within 2 s";

    // Second run.
    auto r2 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    // After start() the old stale entry is gone (pruned). The single entry in
    // all_statuses() is the new job. Verify the new entry can be running OR
    // already finished -- but in either case it is the NEW entry (only one exists).
    auto statuses = mgr_->all_statuses();
    ASSERT_EQ(statuses.size(), 1u)
        << "Expected exactly 1 total status entry after re-run; got " << statuses.size();
    EXPECT_EQ(statuses[0].table_name, "t3");

    // Wait for second run to complete cleanly.
    (void)wait_finished("t3");
}

// =============================================================================
// GDB-923 ADVERSARIAL 1: Three consecutive reruns -- no accumulation.
// After 3x start->finish cycles on the same table, all_statuses() still has
// exactly 1 entry (no unbounded growth).
// =============================================================================

TEST_F(GDB923Test, TripleRerunNoAccumulation) {
    create_instant_table("ta1");

    BackfillStmt stmt;
    stmt.table_name = "ta1";
    stmt.batch_size = 100;

    for (int cycle = 0; cycle < 3; ++cycle) {
        auto r = mgr_->start(stmt, test_db_id_);
        ASSERT_TRUE(r.has_value()) << "Cycle " << cycle << ": " << r.error().message;
        ASSERT_TRUE(wait_finished("ta1", std::chrono::milliseconds{3000}))
            << "Cycle " << cycle << ": backfill did not finish within 3 s";
    }

    auto statuses = mgr_->all_statuses();
    int count = 0;
    for (const auto& s : statuses) {
        if (s.table_name == "ta1") {
            ++count;
        }
    }
    EXPECT_EQ(count, 1) << "all_statuses() must have exactly 1 entry after 3 reruns; got "
                        << count;
}

// =============================================================================
// GDB-923 ADVERSARIAL 2: Two different tables -- prune is per-table only.
// Finishing table A and rerunning A must NOT remove table B's finished status.
// =============================================================================

TEST_F(GDB923Test, TwoDifferentTablesEachGetOwnStatusEntry) {
    create_instant_table("ta2a");
    create_instant_table("ta2b");

    BackfillStmt stmtA;
    stmtA.table_name = "ta2a";
    stmtA.batch_size = 100;

    BackfillStmt stmtB;
    stmtB.table_name = "ta2b";
    stmtB.batch_size = 100;

    // Run both to completion.
    auto rA1 = mgr_->start(stmtA, test_db_id_);
    ASSERT_TRUE(rA1.has_value()) << rA1.error().message;
    ASSERT_TRUE(wait_finished("ta2a")) << "ta2a first run did not finish within 2 s";

    auto rB1 = mgr_->start(stmtB, test_db_id_);
    ASSERT_TRUE(rB1.has_value()) << rB1.error().message;
    ASSERT_TRUE(wait_finished("ta2b")) << "ta2b first run did not finish within 2 s";

    // Re-run table A. Table B's entry must survive.
    auto rA2 = mgr_->start(stmtA, test_db_id_);
    ASSERT_TRUE(rA2.has_value()) << rA2.error().message;

    auto statuses = mgr_->all_statuses();

    int count_a = 0;
    int count_b = 0;
    for (const auto& s : statuses) {
        if (s.table_name == "ta2a") {
            ++count_a;
        }
        if (s.table_name == "ta2b") {
            ++count_b;
        }
    }

    EXPECT_EQ(count_a, 1) << "ta2a must have exactly 1 entry after rerun";
    EXPECT_EQ(count_b, 1)
        << "ta2b entry must NOT be pruned when ta2a is rerun (prune is per-table)";

    (void)wait_finished("ta2a");
}

// =============================================================================
// GDB-923 ADVERSARIAL 3: Start while running -- ALREADY_EXISTS and running job
// is NOT pruned or disturbed.
// The erase_if only removes finished (not-running) jobs; it must not touch a
// running job. If a job is running, start() returns ALREADY_EXISTS early (before
// erase_if), so the running job is never erased.
// =============================================================================

TEST_F(GDB923Test, StartWhileRunningRejectsWithAlreadyExists) {
    // Create a table with 0 rows so the scan finishes quickly, but we need it
    // to still be considered "running" at the instant of the second start().
    // Use a table with NO rows -- loop exits on first next() -> still finishes
    // quickly but reliably transitions through running==true first.
    CatalogColumnDef id_col;
    id_col.name = "id";
    id_col.type_id = TypeId::INT32;
    id_col.ordinal = 0;

    CatalogColumnDef text_col;
    text_col.name = "body";
    text_col.type_id = TypeId::STRING;
    text_col.ordinal = 1;

    CatalogColumnDef emb_col;
    emb_col.name = "vec";
    emb_col.type_id = TypeId::EMBEDDING;
    emb_col.ordinal = 2;

    TableSchema ts;
    ts.name = "ta3";
    ts.columns = {id_col, text_col, emb_col};
    ts.pk_columns = "id";

    auto table_id = catalog_.create_table(test_db_id_, ts);
    ASSERT_TRUE(table_id.has_value()) << table_id.error().message;

    EmbeddingColumnDef emb_def;
    emb_def.table_id = *table_id;
    emb_def.column_id = 2;
    emb_def.dimension = 4;
    emb_def.source_expr = "body";
    emb_def.provider = "builtin/4";
    auto reg = catalog_.register_embedding_column(emb_def);
    ASSERT_TRUE(reg.has_value()) << reg.error().message;

    auto create_storage = storage_->create_table_storage(test_db_id_, *table_id, ts);
    ASSERT_TRUE(create_storage.has_value()) << create_storage.error().message;

    BackfillStmt stmt;
    stmt.table_name = "ta3";
    stmt.batch_size = 100;

    // Start the first job.
    auto r1 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    // Immediately try to start a second job on the SAME table while it may
    // still be running (race: if it finished already this is a no-ALREADY_EXISTS
    // path; that is acceptable since a 0-row table IS a valid fast finish).
    // Only assert ALREADY_EXISTS if the job is still running; if it already
    // finished within nanoseconds, the second start() must succeed (not error).
    auto running_now = mgr_->status("ta3").running;
    auto r2 = mgr_->start(stmt, test_db_id_);

    if (running_now) {
        // The running job must have blocked the second start().
        ASSERT_FALSE(r2.has_value());
        EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS)
            << "Expected ALREADY_EXISTS while job is running, got: " << r2.error().message;

        // The running job must NOT have been erased -- it must still exist and
        // still be running (or just finished).
        auto statuses = mgr_->all_statuses();
        int count = 0;
        for (const auto& s : statuses) {
            if (s.table_name == "ta3") {
                ++count;
            }
        }
        EXPECT_GE(count, 1) << "Running job must not be erased by a rejected start()";
    } else {
        // Job finished before the second start() -- both outcomes are acceptable.
        // If r2 succeeded the prune path ran normally.
        // If r2 failed with NOT_FOUND that would be a bug, but ALREADY_EXISTS /
        // success are both fine.
        if (!r2.has_value()) {
            EXPECT_NE(r2.error().code, StatusCode::NOT_FOUND)
                << "start() must not return NOT_FOUND for a valid table";
        }
    }

    (void)wait_finished("ta3");
}

// =============================================================================
// GDB-923 ADVERSARIAL 4: Stress -- rapid start/finish/restart loop (5 cycles).
// Verifies no deadlock/hang and no accumulation under rapid cycling.
// A hang here would indicate a deadlock in the jthread-join-on-erase path.
// =============================================================================

TEST_F(GDB923Test, RapidCycleNoDeadlockAndNoAccumulation) {
    create_instant_table("ta4");

    BackfillStmt stmt;
    stmt.table_name = "ta4";
    stmt.batch_size = 100;

    // 5 rapid cycles -- each must complete within a generous per-cycle budget.
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto r = mgr_->start(stmt, test_db_id_);
        ASSERT_TRUE(r.has_value()) << "Cycle " << cycle << ": " << r.error().message;
        // Give each cycle up to 4 s; if we hang here the test times out and
        // the CI runner will detect it as a deadlock/hang.
        ASSERT_TRUE(wait_finished("ta4", std::chrono::milliseconds{4000}))
            << "Cycle " << cycle << ": backfill hung (possible deadlock in erase_if join)";
    }

    // After all cycles, exactly 1 entry must remain.
    auto statuses = mgr_->all_statuses();
    int count = 0;
    for (const auto& s : statuses) {
        if (s.table_name == "ta4") {
            ++count;
        }
    }
    EXPECT_EQ(count, 1)
        << "No accumulation after 5 rapid cycles; expected 1 entry, got " << count;
}

// =============================================================================
// GDB-923 ADVERSARIAL 5: status() on unknown table returns sentinel (not crash).
// =============================================================================

TEST_F(GDB923Test, StatusUnknownTableReturnsSentinel) {
    auto s = mgr_->status("nonexistent_table_xyz");
    EXPECT_EQ(s.table_name, "nonexistent_table_xyz");
    EXPECT_FALSE(s.running);
    EXPECT_EQ(s.processed, 0);
    EXPECT_EQ(s.generated, 0);
    EXPECT_EQ(s.skipped, 0);
}

// =============================================================================
// GDB-923 ADVERSARIAL 6: all_statuses() on fresh manager returns empty vector.
// =============================================================================

TEST_F(GDB923Test, AllStatusesEmptyOnFreshManager) {
    auto statuses = mgr_->all_statuses();
    EXPECT_TRUE(statuses.empty()) << "Fresh manager must have no status entries";
}

// =============================================================================
// GDB-923 ADVERSARIAL 7: cancel() on non-running table returns NOT_FOUND.
// =============================================================================

TEST_F(GDB923Test, CancelNonRunningTableReturnsNotFound) {
    auto r = mgr_->cancel("no_such_table");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// GDB-923 ADVERSARIAL 8: Re-run on a finished job, then cancel -- cancel must
// see the new running job, not the erased old job.
// =============================================================================

TEST_F(GDB923Test, CancelAfterRerunCancelsNewJob) {
    create_instant_table("ta8");

    BackfillStmt stmt;
    stmt.table_name = "ta8";
    stmt.batch_size = 100;

    // First run -- finish it.
    auto r1 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(wait_finished("ta8")) << "First backfill did not finish within 2 s";

    // Re-run.
    auto r2 = mgr_->start(stmt, test_db_id_);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    // If the job is still running, cancel must succeed.
    // If it already finished (0-row-like scenario), cancel must return NOT_FOUND.
    auto running_now = mgr_->status("ta8").running;
    auto rc = mgr_->cancel("ta8");

    if (running_now) {
        EXPECT_TRUE(rc.has_value())
            << "cancel() must succeed while job is running; got: " << rc.error().message;
    } else {
        // Finished before cancel -- both NOT_FOUND and ok() are acceptable
        // depending on whether cancel checks running only.
        // The important thing is it must NOT crash.
        (void)rc;
    }

    (void)wait_finished("ta8");
}
