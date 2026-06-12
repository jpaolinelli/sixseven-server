/// QA regression tests for GDB-747: Stamp xmin/xmax in DML; UPDATE creates
/// new versions.
///
/// Acceptance criteria:
///  AC1: No in-place tuple overwrite remains on the UPDATE path — UPDATE
///       leaves the OLD version with xmax set (chained via t_ctid) and a NEW
///       version with a fresh xmin at a different RID, verified at heap level.
///  AC2: Aborted transactions leave tuples invisible (xmin aborted) or
///       undeleted (xmax aborted):
///       - INSERT in aborted txn  -> row invisible to subsequent SELECT;
///       - DELETE in aborted txn  -> row still visible;
///       - UPDATE in aborted txn  -> old version visible, new version not.
///  Plus: committed DML paths keep working (autocommit and explicit COMMIT).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/mvcc_tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

class QA_GDB747 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb747";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE accounts (id INT, owner VARCHAR, balance INT)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    TableHeap& accounts_heap() {
        auto schema = catalog_.get_table(default_database_id, "accounts");
        EXPECT_TRUE(schema.has_value());
        auto ts = storage_->get_table_storage(schema->table_id);
        EXPECT_TRUE(ts.has_value());
        return *(*ts)->heap;
    }

    /// All physical versions on page 1 (visible or not).
    std::vector<std::pair<RID, MvccTupleHeader>> physical_versions() {
        std::vector<std::pair<RID, MvccTupleHeader>> out;
        auto& heap = accounts_heap();
        for (uint16_t slot = 0; slot < 32; ++slot) {
            auto header = heap.get_tuple_header(RID{1, slot});
            if (header) {
                out.emplace_back(RID{1, slot}, *header);
            }
        }
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// AC1: UPDATE creates a new version — no in-place overwrite
// =============================================================================

TEST_F(QA_GDB747, AC1_UpdateLeavesOldVersionWithXmaxAndNewVersionWithFreshXmin) {
    exec_ok("INSERT INTO accounts VALUES (1, 'alice', 100)");
    exec_ok("UPDATE accounts SET balance = 250 WHERE id = 1");

    auto versions = physical_versions();
    ASSERT_EQ(versions.size(), 2u)
        << "expected old + new physical versions after UPDATE (no in-place overwrite)";

    const auto& [old_rid, old_hdr] = versions[0];
    const auto& [new_rid, new_hdr] = versions[1];

    EXPECT_NE(old_rid, new_rid);
    // Old version stamped deleted by a real transaction and chained forward.
    EXPECT_NE(old_hdr.xmax, invalid_txn_id);
    EXPECT_NE(old_hdr.xmax, frozen_txn_id);
    ASSERT_TRUE(old_hdr.has_next_version());
    EXPECT_EQ(old_hdr.t_ctid, new_rid);
    // New version carries a fresh xmin from the same transaction, undeleted.
    EXPECT_EQ(new_hdr.xmin, old_hdr.xmax);
    EXPECT_EQ(new_hdr.xmax, invalid_txn_id);

    // Reads see exactly the new version.
    auto qr = exec_ok("SELECT balance FROM accounts WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 250);
}

// =============================================================================
// AC2: abort semantics
// =============================================================================

TEST_F(QA_GDB747, AC2_InsertInAbortedTxnIsInvisible) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO accounts VALUES (1, 'ghost', 0)");
    exec_ok("ROLLBACK");

    EXPECT_TRUE(exec_ok("SELECT * FROM accounts").rows.empty());

    auto count = exec_ok("SELECT COUNT(*) FROM accounts");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 0);

    // Heap-level: the version exists but is stamped with an aborted xmin.
    auto versions = physical_versions();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_NE(versions[0].second.xmin, frozen_txn_id);
}

TEST_F(QA_GDB747, AC2_DeleteInAbortedTxnLeavesRowVisible) {
    exec_ok("INSERT INTO accounts VALUES (1, 'alice', 100)");

    exec_ok("BEGIN");
    auto del = exec_ok("DELETE FROM accounts WHERE id = 1");
    EXPECT_EQ(del.affected_rows, 1);
    exec_ok("ROLLBACK");

    auto qr = exec_ok("SELECT owner FROM accounts WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");

    auto count = exec_ok("SELECT COUNT(*) FROM accounts");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 1);
}

TEST_F(QA_GDB747, AC2_UpdateInAbortedTxnRestoresOldVersionHidesNewVersion) {
    exec_ok("INSERT INTO accounts VALUES (1, 'alice', 100)");

    exec_ok("BEGIN");
    exec_ok("UPDATE accounts SET owner = 'mallory', balance = 0 WHERE id = 1");
    exec_ok("ROLLBACK");

    auto qr = exec_ok("SELECT owner, balance FROM accounts WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u) << "old version must be visible after aborted UPDATE";
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100);

    // The aborted new version must not surface anywhere.
    auto all = exec_ok("SELECT owner FROM accounts");
    ASSERT_EQ(all.rows.size(), 1u);
    EXPECT_EQ(all.rows[0][0].as_string(), "alice");
}

// =============================================================================
// Committed paths still work
// =============================================================================

TEST_F(QA_GDB747, CommittedAndAutocommitDmlBehaviorUnchanged) {
    // Autocommit.
    exec_ok("INSERT INTO accounts VALUES (1, 'alice', 100)");
    exec_ok("INSERT INTO accounts VALUES (2, 'bob', 200)");
    EXPECT_EQ(exec_ok("UPDATE accounts SET balance = 150 WHERE id = 1").affected_rows, 1);
    EXPECT_EQ(exec_ok("DELETE FROM accounts WHERE id = 2").affected_rows, 1);

    auto qr = exec_ok("SELECT id, balance FROM accounts");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 150);

    // Explicit committed transaction.
    exec_ok("BEGIN");
    exec_ok("INSERT INTO accounts VALUES (3, 'carol', 300)");
    exec_ok("UPDATE accounts SET balance = 175 WHERE id = 1");
    exec_ok("COMMIT");

    auto after = exec_ok("SELECT id, balance FROM accounts ORDER BY id");
    ASSERT_EQ(after.rows.size(), 2u);
    EXPECT_EQ(after.rows[0][1].as_int32(), 175);
    EXPECT_EQ(after.rows[1][0].as_int32(), 3);

    auto count = exec_ok("SELECT COUNT(*) FROM accounts");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 2);
}

TEST_F(QA_GDB747, RollbackAfterMixedDmlRestoresPriorState) {
    exec_ok("INSERT INTO accounts VALUES (1, 'alice', 100)");
    exec_ok("INSERT INTO accounts VALUES (2, 'bob', 200)");

    exec_ok("BEGIN");
    exec_ok("INSERT INTO accounts VALUES (3, 'carol', 300)");
    exec_ok("UPDATE accounts SET balance = 0 WHERE id = 1");
    exec_ok("DELETE FROM accounts WHERE id = 2");
    exec_ok("ROLLBACK");

    auto qr = exec_ok("SELECT id, balance FROM accounts ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[1][1].as_int32(), 200);

    auto count = exec_ok("SELECT COUNT(*) FROM accounts");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 2);
}
