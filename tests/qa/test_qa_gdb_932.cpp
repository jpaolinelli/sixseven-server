/// @file test_qa_gdb_932.cpp
/// @brief QA regression tests for GDB-932: propagate index-maintenance errors
/// instead of swallowing them as warnings.
///
/// Before this fix, a failed BM25 or HNSW index-maintenance call was logged as
/// WARN and execution continued, leaving the table row mutated but the index
/// silently inconsistent. The fix propagates the error, which causes the
/// QueryEngine's implicit-transaction abort path to roll back the table
/// mutation via MVCC (xmin/xmax of the aborting txn become invisible).
///
/// Tests:
///   ForcedFailureInsert   - BM25 add_document fails -> error + row not visible
///   ForcedFailureDelete   - BM25 remove_document fails -> error + row still present
///   ForcedFailureUpdate   - BM25 remove_document (old) fails -> error + old value kept
///   HappyPathInsert       - normal INSERT succeeds; row visible; BM25 hit found
///   HappyPathDelete       - normal DELETE succeeds; row gone; BM25 hit absent
///   HappyPathUpdate       - normal UPDATE succeeds; new value visible; BM25 reflects new text
///   BenignNullTextInsert  - INSERT with NULL text column succeeds (NULL never indexed)
///   BenignNullTextUpdate  - UPDATE setting text to NULL succeeds (no spurious failure)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Fixture
// =============================================================================

class QA_GDB932 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb932";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        make_stack();
        run_bootstrap();
        rebuild_indexes();
        setup_table();
    }

    void TearDown() override {
        reset_stack();
        std::filesystem::remove_all(data_dir_);
    }

    void make_stack() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
    }

    void reset_stack() {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void run_bootstrap() {
        Config cfg = Config::load_defaults();
        auto r = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, cfg, data_dir_);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    void setup_table() {
        exec_ok("CREATE TABLE docs (id INT PRIMARY KEY, body VARCHAR)");
        exec_ok("CREATE INDEX idx_docs_body ON docs(body) USING bm25");
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "[SQL] " << sql << "\n[ERR] " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    /// Execute SQL and expect it to fail.
    bool exec_fails(const std::string& sql) {
        auto result = engine_->execute(sql);
        return !result.has_value();
    }

    /// Get count of rows in docs with the given id.
    int64_t count_by_id(int32_t id) {
        auto qr = exec_ok("SELECT COUNT(*) FROM docs WHERE id = " + std::to_string(id));
        if (qr.rows.empty()) {
            return -1;
        }
        return qr.rows[0][0].as_int64();
    }

    /// Get body of row with given id; returns empty string if not found.
    std::string body_by_id(int32_t id) {
        auto qr = exec_ok("SELECT body FROM docs WHERE id = " + std::to_string(id));
        if (qr.rows.empty()) {
            return "";
        }
        auto& v = qr.rows[0][0];
        return v.is_null() ? "" : v.as_string();
    }

    /// Get the live BM25 index for the docs table.
    Bm25Index* get_bm25() {
        auto def = catalog_->get_index(default_database_id, "idx_docs_body");
        EXPECT_TRUE(def.has_value());
        if (!def.has_value()) {
            return nullptr;
        }
        auto* map = index_manager_->bm25_map();
        auto it = map->find(def->index_id);
        return (it == map->end()) ? nullptr : it->second;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// =============================================================================
// Forced-failure: INSERT
// =============================================================================

// When BM25 add_document fails, INSERT must return an error AND the inserted
// row must be invisible (MVCC abort rolls back the xmin-stamped tuple version).
TEST_F(QA_GDB932, ForcedFailureInsert_ErrorReturnedAndRowNotVisible) {
    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    // Arm the fault injector before the INSERT.
    idx->fault_inject_ = true;

    auto result = engine_->execute("INSERT INTO docs VALUES (1, 'hello world')");

    // Disarm immediately so subsequent queries are not affected.
    idx->fault_inject_ = false;

    // INSERT must have failed.
    ASSERT_FALSE(result.has_value()) << "Expected INSERT to fail due to BM25 maintenance error";

    // The table mutation must have been rolled back: row 1 is not visible.
    EXPECT_EQ(count_by_id(1), 0) << "Row must not be visible after failed INSERT";

    // The BM25 index must not contain any posting for this text.
    auto hits = idx->search("hello", 10);
    EXPECT_TRUE(hits.empty()) << "BM25 must not have indexed the row after a failed INSERT";
}

// =============================================================================
// Forced-failure: DELETE
// =============================================================================

// When BM25 remove_document fails, DELETE must return an error AND the row
// must still be present (MVCC abort undoes the xmax stamp).
TEST_F(QA_GDB932, ForcedFailureDelete_ErrorReturnedAndRowStillPresent) {
    // Seed: insert a row normally.
    exec_ok("INSERT INTO docs VALUES (2, 'database systems')");
    ASSERT_EQ(count_by_id(2), 1) << "Precondition: row 2 must be present before DELETE";

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    idx->fault_inject_ = true;
    auto result = engine_->execute("DELETE FROM docs WHERE id = 2");
    idx->fault_inject_ = false;

    // DELETE must have failed.
    ASSERT_FALSE(result.has_value()) << "Expected DELETE to fail due to BM25 maintenance error";

    // The row must still be visible after the aborted DELETE.
    EXPECT_EQ(count_by_id(2), 1) << "Row must still be present after failed DELETE";

    // The BM25 index still contains the document (remove was not completed).
    auto hits = idx->search("database", 10);
    EXPECT_FALSE(hits.empty()) << "BM25 must still have the posting after failed DELETE";
}

// =============================================================================
// Forced-failure: UPDATE (old-posting removal site)
// =============================================================================

// When BM25 remove_document(old_rid) fails during UPDATE, the statement must
// return an error AND the row must retain its original value.
TEST_F(QA_GDB932, ForcedFailureUpdate_ErrorReturnedAndOldValueKept) {
    // Seed: row 3 with original text.
    exec_ok("INSERT INTO docs VALUES (3, 'original text')");
    ASSERT_EQ(body_by_id(3), "original text") << "Precondition: original text must be present";

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    // Arm fault injector to trigger on remove_document(old_rid) during UPDATE.
    idx->fault_inject_ = true;
    auto result = engine_->execute("UPDATE docs SET body = 'updated text' WHERE id = 3");
    idx->fault_inject_ = false;

    // UPDATE must have failed.
    ASSERT_FALSE(result.has_value()) << "Expected UPDATE to fail due to BM25 maintenance error";

    // The row must retain its original value.
    EXPECT_EQ(body_by_id(3), "original text") << "Row must keep original value after failed UPDATE";

    // BM25 must still reflect original text (no partial update).
    auto orig_hits = idx->search("original", 10);
    EXPECT_FALSE(orig_hits.empty()) << "BM25 must still have original text posting";
    auto upd_hits = idx->search("updated", 10);
    EXPECT_TRUE(upd_hits.empty()) << "BM25 must not have updated text after failed UPDATE";
}

// =============================================================================
// Happy path: INSERT
// =============================================================================

// A normal INSERT with a working BM25 index succeeds and the row is visible.
TEST_F(QA_GDB932, HappyPathInsert_RowVisibleAndBm25Indexed) {
    exec_ok("INSERT INTO docs VALUES (10, 'the quick brown fox')");

    EXPECT_EQ(count_by_id(10), 1) << "Row must be visible after successful INSERT";

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    auto hits = idx->search("quick", 10);
    EXPECT_FALSE(hits.empty()) << "BM25 must have indexed the inserted text";
}

// =============================================================================
// Happy path: DELETE
// =============================================================================

// A normal DELETE with a working BM25 index succeeds and removes the row and
// its BM25 postings.
TEST_F(QA_GDB932, HappyPathDelete_RowGoneAndBm25CleanedUp) {
    exec_ok("INSERT INTO docs VALUES (11, 'machine learning')");

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);
    EXPECT_FALSE(idx->search("machine", 10).empty()) << "Precondition: doc must be indexed";

    exec_ok("DELETE FROM docs WHERE id = 11");

    EXPECT_EQ(count_by_id(11), 0) << "Row must be gone after successful DELETE";
    EXPECT_TRUE(idx->search("machine", 10).empty()) << "BM25 must have removed the posting";
}

// =============================================================================
// Happy path: UPDATE - index reflects new text, not old
// =============================================================================

// A normal UPDATE succeeds; BM25 reflects the new text and no longer matches
// the old text. This is the positive search-consistency check.
TEST_F(QA_GDB932, HappyPathUpdate_Bm25ReflectsNewText) {
    exec_ok("INSERT INTO docs VALUES (12, 'alpha search term')");

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);
    EXPECT_FALSE(idx->search("alpha", 10).empty()) << "Precondition: alpha must be indexed";

    exec_ok("UPDATE docs SET body = 'beta replacement term' WHERE id = 12");

    EXPECT_EQ(body_by_id(12), "beta replacement term")
        << "Row must reflect the updated value after successful UPDATE";

    // BM25 must have the new text but not the old text.
    EXPECT_FALSE(idx->search("beta", 10).empty()) << "BM25 must index the new text";
    EXPECT_TRUE(idx->search("alpha", 10).empty()) << "BM25 must not retain the old text";
}

// =============================================================================
// Benign: NULL text INSERT does not trigger a spurious error
// =============================================================================

// Inserting a row where the indexed text column is NULL must succeed because
// NULL means the row is never submitted to add_document (benign skip).
TEST_F(QA_GDB932, BenignNullTextInsert_SucceedsWithoutIndexing) {
    exec_ok("INSERT INTO docs (id) VALUES (20)");

    // Row must be visible.
    EXPECT_EQ(count_by_id(20), 1) << "NULL-text row must be visible after successful INSERT";

    // BM25 must not contain any entry for this row (nothing was indexed).
    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);
    // Doc count should be 0 because no text was added.
    EXPECT_EQ(idx->doc_count(), 0u);
}

// =============================================================================
// Benign: NULL text UPDATE does not trigger a spurious error
// =============================================================================

// Updating the indexed text column to NULL must succeed. remove_document for
// the new NULL-valued RID is a no-op in BM25 (never indexed = not present).
TEST_F(QA_GDB932, BenignNullTextUpdate_SucceedsAndRemovesOldPosting) {
    exec_ok("INSERT INTO docs VALUES (21, 'some text to nullify')");

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(idx->doc_count(), 1u) << "Precondition: one doc indexed";

    exec_ok("UPDATE docs SET body = NULL WHERE id = 21");

    // Row must still be visible with NULL body.
    EXPECT_EQ(count_by_id(21), 1) << "Row must still be present after UPDATE to NULL";
    auto body = body_by_id(21);
    EXPECT_EQ(body, "") << "Body must be empty (NULL) after UPDATE to NULL";

    // Old posting must be gone (update removed old BM25 entry).
    EXPECT_EQ(idx->doc_count(), 0u) << "BM25 must have removed the old posting on NULL update";
}
