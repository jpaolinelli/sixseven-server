/// QA regression tests for GDB-777: apply MVCC snapshot visibility in all
/// scan operators.
///
/// Acceptance criteria:
///  AC1: Uncommitted rows from other sessions are invisible — scans filter
///       through is_visible(header, snapshot, txn_mgr, viewer).
///  AC2: The CountScan fast path is MVCC-aware (bypassed when a snapshot read
///       view is active): COUNT(*) never reports uncommitted rows to other
///       sessions even though the heap's live row counter includes them.
///  Plus: self-visibility — a transaction sees its own uncommitted inserts
///       and does not see rows it deleted; aborted rows are never visible;
///       deleted-but-uncommitted rows stay visible to others until commit.
///
/// "Another session" is modeled at the operator level: the real SeqScan /
/// CountScan operators run over the shared TableHeap under an explicitly
/// installed MvccReadView with a fresh snapshot and no viewer transaction —
/// exactly what the engine installs for an autocommit reader.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/count_scan.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/read_view.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

class QA_GDB777 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb777";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE items (id INT)");

        storage_schema_ = Schema({{"id", TypeId::INT32}});
        OutputColumn c1{"items", "id", TypeId::INT32, false, 0};
        scan_schema_ = OutputSchema({c1});
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

    TableHeap& items_heap() {
        auto schema = catalog_.get_table(default_database_id, "items");
        EXPECT_TRUE(schema.has_value());
        auto ts = storage_->get_table_storage(schema->table_id);
        EXPECT_TRUE(ts.has_value());
        return *(*ts)->heap;
    }

    /// Rows visible to "another session": run the real SeqScanOperator over
    /// the shared heap under a fresh-snapshot read view with no viewer txn.
    size_t other_session_seq_scan_rows() {
        MvccReadViewGuard guard(
            MvccReadView{engine_->transaction_manager().take_snapshot(), invalid_txn_id});
        SeqScanOperator scan(items_heap(), storage_schema_, scan_schema_);
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        size_t n = 0;
        while (true) {
            auto row = scan.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value()) {
                break;
            }
            ++n;
        }
        scan.close();
        return n;
    }

    /// COUNT(*) as seen by "another session": the real CountScanOperator
    /// under a fresh-snapshot read view with no viewer txn.
    int64_t other_session_count_scan() {
        MvccReadViewGuard guard(
            MvccReadView{engine_->transaction_manager().take_snapshot(), invalid_txn_id});
        OutputSchema count_schema({{"", "__agg_0", TypeId::INT64, false, 0}});
        CountScanOperator scan(items_heap(), std::move(count_schema));
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        auto row = scan.next();
        EXPECT_TRUE(row.has_value()) << row.error().message;
        if (!row.has_value() || !row->has_value()) {
            ADD_FAILURE() << "count scan produced no row";
            return -1;
        }
        int64_t count = (*row)->values[0].as_int64();
        scan.close();
        return count;
    }

    /// Engine-level scalar: first cell of the first row.
    int64_t scalar(const std::string& sql) {
        auto qr = exec_ok(sql);
        EXPECT_EQ(qr.rows.size(), 1u) << sql;
        if (qr.rows.size() != 1 || qr.rows[0].empty()) {
            return -1;
        }
        return qr.rows[0][0].as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    Schema storage_schema_;
    OutputSchema scan_schema_;
};

// =============================================================================
// AC1: Uncommitted rows from other sessions invisible
// =============================================================================

TEST_F(QA_GDB777, AC1_UncommittedInsertsInvisibleToOtherSession) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO items VALUES (1)");
    exec_ok("INSERT INTO items VALUES (2)");

    // Another session sees nothing while the writer has not committed.
    EXPECT_EQ(other_session_seq_scan_rows(), 0u);

    // Self-visibility: the writing transaction sees its own inserts.
    auto qr = exec_ok("SELECT id FROM items");
    EXPECT_EQ(qr.rows.size(), 2u);

    exec_ok("ROLLBACK");
}

TEST_F(QA_GDB777, AC1_CommittedInsertsVisibleToFreshSnapshot) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO items VALUES (1)");
    exec_ok("INSERT INTO items VALUES (2)");
    EXPECT_EQ(other_session_seq_scan_rows(), 0u);
    exec_ok("COMMIT");

    // A new statement snapshot taken after the commit sees both rows.
    EXPECT_EQ(other_session_seq_scan_rows(), 2u);
    auto qr = exec_ok("SELECT id FROM items");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QA_GDB777, AC1_RolledBackInsertsNeverVisible) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO items VALUES (1)");
    exec_ok("ROLLBACK");

    EXPECT_EQ(other_session_seq_scan_rows(), 0u);
    EXPECT_EQ(other_session_count_scan(), 0);
    auto qr = exec_ok("SELECT id FROM items");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB777, AC1_UncommittedDeleteVisibleToOthersInvisibleToSelf) {
    exec_ok("INSERT INTO items VALUES (1)"); // Autocommit — committed.

    exec_ok("BEGIN");
    exec_ok("DELETE FROM items WHERE id = 1");

    // The deleter no longer sees the row...
    auto self = exec_ok("SELECT id FROM items");
    EXPECT_EQ(self.rows.size(), 0u);
    // ...but another session still does (delete not committed).
    EXPECT_EQ(other_session_seq_scan_rows(), 1u);
    EXPECT_EQ(other_session_count_scan(), 1);

    exec_ok("COMMIT");

    // After commit the row is gone for everyone.
    EXPECT_EQ(other_session_seq_scan_rows(), 0u);
    EXPECT_EQ(other_session_count_scan(), 0);
    auto after = exec_ok("SELECT id FROM items");
    EXPECT_EQ(after.rows.size(), 0u);
}

// =============================================================================
// AC2: CountScan fast path MVCC-aware / bypassed under a read view
// =============================================================================

TEST_F(QA_GDB777, AC2_CountScanIgnoresUncommittedRowsDespiteRowCounter) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO items VALUES (1)");
    exec_ok("INSERT INTO items VALUES (2)");
    exec_ok("INSERT INTO items VALUES (3)");

    // The heap's live row counter already includes the uncommitted inserts —
    // if CountScan used the fast path it would report 3 to everyone.
    EXPECT_EQ(items_heap().row_count(), 3u);

    // Another session's COUNT(*) sees none of them.
    EXPECT_EQ(other_session_count_scan(), 0);

    // The writer's own COUNT(*) (engine CountScan path) sees all three.
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM items"), 3);

    exec_ok("ROLLBACK");

    EXPECT_EQ(other_session_count_scan(), 0);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM items"), 0);
}

TEST_F(QA_GDB777, AC2_CountScanMatchesCommittedStateAfterCommit) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO items VALUES (1)");
    exec_ok("INSERT INTO items VALUES (2)");
    exec_ok("COMMIT");

    EXPECT_EQ(other_session_count_scan(), 2);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM items"), 2);
}

// =============================================================================
// Self-visibility (writer's own view through real engine statements)
// =============================================================================

TEST_F(QA_GDB777, SelfSeesOwnUncommittedInsertAndNotOwnDelete) {
    exec_ok("INSERT INTO items VALUES (1)"); // Committed baseline.

    exec_ok("BEGIN");
    exec_ok("INSERT INTO items VALUES (2)");
    auto both = exec_ok("SELECT id FROM items");
    EXPECT_EQ(both.rows.size(), 2u);

    exec_ok("DELETE FROM items WHERE id = 1");
    auto remaining = exec_ok("SELECT id FROM items");
    ASSERT_EQ(remaining.rows.size(), 1u);
    EXPECT_EQ(remaining.rows[0][0].as_int32(), 2);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM items"), 1);

    exec_ok("COMMIT");
    EXPECT_EQ(other_session_count_scan(), 1);
}
