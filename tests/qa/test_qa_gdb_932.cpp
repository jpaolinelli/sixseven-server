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

// =============================================================================
// Adversarial: retry-after-failure (INSERT)
// =============================================================================

// After a forced-failure rolls back an INSERT, clearing fault_inject_ and
// re-running the same statement must succeed, leaving table and index
// consistent with no leftover state from the aborted transaction.
TEST_F(QA_GDB932, RetryAfterFailureInsert_SucceedsAndLeavesConsistentState) {
    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    // First attempt: forced failure.
    idx->fault_inject_ = true;
    auto fail_result = engine_->execute("INSERT INTO docs VALUES (30, 'retry test text')");
    idx->fault_inject_ = false;
    ASSERT_FALSE(fail_result.has_value()) << "First INSERT must fail under fault injection";

    // Table must be empty (abort rolled back).
    ASSERT_EQ(count_by_id(30), 0) << "Row must not be visible after failed INSERT";
    // Index must be clean.
    ASSERT_TRUE(idx->search("retry", 10).empty())
        << "BM25 must have no orphan posting after failed INSERT";

    // Second attempt: no fault injection -- must succeed.
    exec_ok("INSERT INTO docs VALUES (30, 'retry test text')");

    // Table must now contain the row.
    EXPECT_EQ(count_by_id(30), 1) << "Row must be visible after successful retry INSERT";
    // BM25 must reflect the inserted text.
    auto hits = idx->search("retry", 10);
    EXPECT_FALSE(hits.empty()) << "BM25 must index the text after successful retry INSERT";
    // Exactly one document indexed (no duplicate from the aborted txn).
    EXPECT_EQ(idx->doc_count(), 1u) << "BM25 must contain exactly one document after retry";
}

// =============================================================================
// Adversarial: retry-after-failure (UPDATE)
// =============================================================================

// After a forced-failure rolls back an UPDATE, clearing fault_inject_ and
// re-running the UPDATE must succeed. Table and index must be consistent with
// the new value -- no leftover lock or state from the aborted transaction.
TEST_F(QA_GDB932, RetryAfterFailureUpdate_SucceedsAndLeavesConsistentState) {
    exec_ok("INSERT INTO docs VALUES (31, 'before update text')");

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);
    ASSERT_FALSE(idx->search("before", 10).empty()) << "Precondition: original text indexed";

    // First attempt: forced failure during UPDATE.
    idx->fault_inject_ = true;
    auto fail_result = engine_->execute("UPDATE docs SET body = 'after update text' WHERE id = 31");
    idx->fault_inject_ = false;
    ASSERT_FALSE(fail_result.has_value()) << "First UPDATE must fail under fault injection";

    // Table must retain original value.
    EXPECT_EQ(body_by_id(31), "before update text")
        << "Row must keep original value after failed UPDATE";
    // Index must still reflect original text.
    EXPECT_FALSE(idx->search("before", 10).empty())
        << "BM25 must still have original posting after failed UPDATE";
    EXPECT_TRUE(idx->search("after", 10).empty())
        << "BM25 must not have new posting after failed UPDATE";

    // Second attempt: no fault injection -- must succeed.
    exec_ok("UPDATE docs SET body = 'after update text' WHERE id = 31");

    // Table must have new value.
    EXPECT_EQ(body_by_id(31), "after update text")
        << "Row must reflect new value after successful retry UPDATE";
    // Index must have new text only.
    EXPECT_FALSE(idx->search("after", 10).empty()) << "BM25 must index new text after retry UPDATE";
    EXPECT_TRUE(idx->search("before", 10).empty())
        << "BM25 must not retain old text after retry UPDATE";
}

// =============================================================================
// Adversarial: multi-row DELETE -- ALL rows roll back on Nth failure
// =============================================================================

// When DELETE targets multiple rows and BM25 remove_document fails on the
// first processed row, the whole statement must fail. All matching rows must
// still be present (no partial delete). This verifies there is no
// partial-write inconsistency where rows 1..N-1 are deleted but rows N..M are not.
TEST_F(QA_GDB932, MultiRowDelete_FullRollbackOnFailure) {
    // Seed three rows that will all match the DELETE.
    exec_ok("INSERT INTO docs VALUES (40, 'bulk delete alpha')");
    exec_ok("INSERT INTO docs VALUES (41, 'bulk delete beta')");
    exec_ok("INSERT INTO docs VALUES (42, 'bulk delete gamma')");
    ASSERT_EQ(count_by_id(40), 1) << "Precondition: row 40 present";
    ASSERT_EQ(count_by_id(41), 1) << "Precondition: row 41 present";
    ASSERT_EQ(count_by_id(42), 1) << "Precondition: row 42 present";

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    // Arm fault injector: remove_document will fail on the first call.
    idx->fault_inject_ = true;
    // DELETE matches all three rows by body prefix.
    auto result = engine_->execute("DELETE FROM docs WHERE body LIKE 'bulk delete%'");
    idx->fault_inject_ = false;

    // The DELETE must have returned an error.
    ASSERT_FALSE(result.has_value()) << "Multi-row DELETE must fail when BM25 maintenance fails";

    // All three rows must still be present (full rollback -- no partial delete).
    EXPECT_EQ(count_by_id(40), 1) << "Row 40 must survive after failed multi-row DELETE";
    EXPECT_EQ(count_by_id(41), 1) << "Row 41 must survive after failed multi-row DELETE";
    EXPECT_EQ(count_by_id(42), 1) << "Row 42 must survive after failed multi-row DELETE";

    // BM25 must still contain all three documents.
    EXPECT_EQ(idx->doc_count(), 3u)
        << "BM25 must retain all 3 documents after failed multi-row DELETE";
}

// =============================================================================
// Adversarial: multi-row UPDATE -- ALL rows roll back on Nth failure
// =============================================================================

// When UPDATE targets multiple rows and BM25 maintenance fails, the whole
// statement must fail. All rows must retain their original values. This
// verifies no partial-write: rows processed before the failure are not left
// with the updated value.
TEST_F(QA_GDB932, MultiRowUpdate_FullRollbackOnFailure) {
    // Seed three rows with distinctive non-stopword terms.
    exec_ok("INSERT INTO docs VALUES (50, 'zeta corpus alpha')");
    exec_ok("INSERT INTO docs VALUES (51, 'zeta corpus beta')");
    exec_ok("INSERT INTO docs VALUES (52, 'zeta corpus gamma')");

    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);
    ASSERT_EQ(idx->doc_count(), 3u) << "Precondition: three docs indexed";
    // Verify distinctive terms ARE indexed before the fault.
    ASSERT_FALSE(idx->search("alpha", 10).empty()) << "Precondition: alpha indexed";
    ASSERT_FALSE(idx->search("beta", 10).empty()) << "Precondition: beta indexed";
    ASSERT_FALSE(idx->search("gamma", 10).empty()) << "Precondition: gamma indexed";

    // Arm fault injector: the first add_document or remove_document in the
    // UPDATE loop will fail.
    idx->fault_inject_ = true;
    auto result =
        engine_->execute("UPDATE docs SET body = 'zeta corpus replaced' WHERE body LIKE 'zeta%'");
    idx->fault_inject_ = false;

    // UPDATE must have returned an error.
    ASSERT_FALSE(result.has_value()) << "Multi-row UPDATE must fail when BM25 maintenance fails";

    // All rows must retain their original values (full rollback).
    EXPECT_EQ(body_by_id(50), "zeta corpus alpha")
        << "Row 50 must keep original value after failed multi-row UPDATE";
    EXPECT_EQ(body_by_id(51), "zeta corpus beta")
        << "Row 51 must keep original value after failed multi-row UPDATE";
    EXPECT_EQ(body_by_id(52), "zeta corpus gamma")
        << "Row 52 must keep original value after failed multi-row UPDATE";

    // BM25 must still contain the original texts (no partial re-indexing).
    EXPECT_FALSE(idx->search("alpha", 10).empty())
        << "BM25 must retain 'alpha' posting after failed multi-row UPDATE";
    EXPECT_FALSE(idx->search("beta", 10).empty())
        << "BM25 must retain 'beta' posting after failed multi-row UPDATE";
    EXPECT_FALSE(idx->search("gamma", 10).empty())
        << "BM25 must retain 'gamma' posting after failed multi-row UPDATE";
    // None of the replacement text must appear.
    EXPECT_TRUE(idx->search("replaced", 10).empty())
        << "BM25 must not have 'replaced' posting after failed multi-row UPDATE";
}

// =============================================================================
// Adversarial: post-rollback index/table cross-consistency (INSERT)
// =============================================================================

// After a forced INSERT failure, insert a DIFFERENT row successfully. The
// successful row must be searchable. The failed row's terms must not appear.
// This pins that the index and table are mutually consistent (no orphan
// postings from the aborted transaction polluting the index).
TEST_F(QA_GDB932, PostRollbackIndexTableConsistency_Insert) {
    Bm25Index* idx = get_bm25();
    ASSERT_NE(idx, nullptr);

    // Forced failure for row 60.
    idx->fault_inject_ = true;
    auto fail_result = engine_->execute("INSERT INTO docs VALUES (60, 'orphan posting danger')");
    idx->fault_inject_ = false;
    ASSERT_FALSE(fail_result.has_value());

    // Insert a different row successfully.
    exec_ok("INSERT INTO docs VALUES (61, 'clean insert succeeds')");

    // Table: row 60 absent, row 61 present.
    EXPECT_EQ(count_by_id(60), 0) << "Failed row must not be visible";
    EXPECT_EQ(count_by_id(61), 1) << "Successful row must be visible";

    // Index: only row 61 terms are present, row 60 terms are absent.
    auto orphan_hits = idx->search("orphan", 10);
    EXPECT_TRUE(orphan_hits.empty()) << "BM25 must not contain orphan posting from failed INSERT";
    auto clean_hits = idx->search("clean", 10);
    EXPECT_FALSE(clean_hits.empty()) << "BM25 must have indexed the successful INSERT";

    // Index size must equal the table row count.
    EXPECT_EQ(idx->doc_count(), 1u) << "BM25 doc_count must match visible table row count";
}
