/// @file test_qa_gdb_1254_nullable_matrix.cpp
/// @brief Focused adversarial matrix for GDB-1254 re-verification. The fix
/// replaced the `!owner->is_base` nullable-side guard with an EFFECTIVE
/// nullability flag on each candidate input. These probes exercise the full
/// RIGHT / FULL / LEFT cross-product on BOTH the base (left-most) side and the
/// joined (right) side, asserting that NEAREST is REJECTED only when its owning
/// input is genuinely NULL-padded, and ACCEPTED on every preserved position —
/// in particular the easy-to-regress case of a preserved-side NEAREST under a
/// RIGHT join. The goal is to catch over-rejection (a preserved side wrongly
/// blocked) and under-rejection (a nullable side wrongly allowed).
///
/// Effective-nullability rule under test (from plan_select):
///   * base side  is nullable  iff some join is RIGHT or FULL.
///   * joined side is nullable iff its own join is LEFT or FULL.
/// Everything else is preserved and must be accepted.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

/// Two vector tables (`books` and `notes`) plus a non-vector table (`reviews`),
/// so a NEAREST can target an EMBEDDING column that sits on EITHER join side.
class QA_GDB1254_NullableMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1254_matrix";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        seed();
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << sql << ": " << result.error().message;
    }

    void register_embedding(table_id_t table_id, int32_t col_id, int32_t dim) {
        EmbeddingColumnDef emb;
        emb.table_id = table_id;
        emb.column_id = col_id;
        emb.dimension = dim;
        emb.source_expr = "title";
        emb.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;
    }

    void seed() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4);
        exec_ok("INSERT INTO books VALUES (1, 'Alpha',  [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (2, 'Apex',   [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (3, 'Gamma',  [0.0, 1.0, 0.0, 0.0])");

        exec_ok("CREATE TABLE notes (id INT PRIMARY KEY, book_id INT, note_vec EMBEDDING)");
        auto notes = catalog_.get_table(default_database_id, "notes");
        ASSERT_TRUE(notes.has_value());
        register_embedding(notes->table_id, 2, 4);
        exec_ok("INSERT INTO notes VALUES (10, 1, [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO notes VALUES (20, 2, [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO notes VALUES (30, 3, [0.0, 1.0, 0.0, 0.0])");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (100, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (200, 2, 3)");
    }

    /// Returns true if the query was accepted (planned + executed) without error.
    bool accepted(const std::string& sql) {
        auto result = engine_->execute(sql);
        return result.has_value();
    }

    /// Asserts the query is rejected with INVALID_ARGUMENT and a "nullable side"
    /// message — the canonical v1 OUTER-join rejection.
    void expect_nullable_rejection(const std::string& sql) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << sql << ": unexpectedly accepted";
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
        EXPECT_NE(result.error().message.find("nullable side"), std::string::npos)
            << sql << ": wrong error -> " << result.error().message;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

constexpr const char* kVec = "[1.0, 0.0, 0.0, 0.0]";

// ───────────────────────── BASE-SIDE NEAREST ──────────────────────────────
// Base = left-most table (`books b`). Nullable iff some join is RIGHT/FULL.

// LEFT join: base is preserved -> ACCEPT. (Regression guard against
// over-rejection: the new effective-nullability flag must still treat the base
// as preserved under a LEFT join.)
TEST_F(QA_GDB1254_NullableMatrix, BaseSideNearest_LeftJoin_Accepted) {
    EXPECT_TRUE(accepted(std::string("SELECT b.id FROM books b "
                                     "LEFT JOIN reviews r ON r.book_id = b.id "
                                     "WHERE NEAREST(b.description_vec, 2) TO ") +
                         kVec));
}

// INNER join: base preserved -> ACCEPT.
TEST_F(QA_GDB1254_NullableMatrix, BaseSideNearest_InnerJoin_Accepted) {
    EXPECT_TRUE(accepted(std::string("SELECT b.id FROM books b "
                                     "INNER JOIN reviews r ON r.book_id = b.id "
                                     "WHERE NEAREST(b.description_vec, 2) TO ") +
                         kVec));
}

// RIGHT join: base is the NULLABLE side -> REJECT (the core GDB-1254 defect).
TEST_F(QA_GDB1254_NullableMatrix, BaseSideNearest_RightJoin_Rejected) {
    expect_nullable_rejection(std::string("SELECT b.id FROM books b "
                                          "RIGHT JOIN reviews r ON r.book_id = b.id "
                                          "WHERE NEAREST(b.description_vec, 2) TO ") +
                              kVec);
}

// FULL join: base is nullable -> REJECT.
TEST_F(QA_GDB1254_NullableMatrix, BaseSideNearest_FullJoin_Rejected) {
    expect_nullable_rejection(std::string("SELECT b.id FROM books b "
                                          "FULL JOIN reviews r ON r.book_id = b.id "
                                          "WHERE NEAREST(b.description_vec, 2) TO ") +
                              kVec);
}

// ──────────────────────── JOINED-SIDE NEAREST ─────────────────────────────
// Joined = right table. Nullable iff its own join is LEFT/FULL. We put the
// EMBEDDING column (`notes.note_vec`) on the RIGHT side so NEAREST can own it.

// RIGHT join: the joined side is the PRESERVED side -> ACCEPT. This is the
// reviewer-flagged under-/over-rejection trap: a preserved-side NEAREST under a
// RIGHT join must still be allowed even though the join is OUTER.
TEST_F(QA_GDB1254_NullableMatrix, JoinedSideNearest_RightJoin_Accepted) {
    EXPECT_TRUE(accepted(std::string("SELECT n.id FROM reviews r "
                                     "RIGHT JOIN notes n ON n.book_id = r.book_id "
                                     "WHERE NEAREST(n.note_vec, 2) TO ") +
                         kVec))
        << "preserved (right) side of a RIGHT JOIN must accept NEAREST";
}

// INNER join: joined side preserved -> ACCEPT.
TEST_F(QA_GDB1254_NullableMatrix, JoinedSideNearest_InnerJoin_Accepted) {
    EXPECT_TRUE(accepted(std::string("SELECT n.id FROM reviews r "
                                     "INNER JOIN notes n ON n.book_id = r.book_id "
                                     "WHERE NEAREST(n.note_vec, 2) TO ") +
                         kVec));
}

// LEFT join: joined side is NULL-padded -> REJECT.
TEST_F(QA_GDB1254_NullableMatrix, JoinedSideNearest_LeftJoin_Rejected) {
    expect_nullable_rejection(std::string("SELECT n.id FROM reviews r "
                                          "LEFT JOIN notes n ON n.book_id = r.book_id "
                                          "WHERE NEAREST(n.note_vec, 2) TO ") +
                              kVec);
}

// FULL join: joined side nullable -> REJECT.
TEST_F(QA_GDB1254_NullableMatrix, JoinedSideNearest_FullJoin_Rejected) {
    expect_nullable_rejection(std::string("SELECT n.id FROM reviews r "
                                          "FULL JOIN notes n ON n.book_id = r.book_id "
                                          "WHERE NEAREST(n.note_vec, 2) TO ") +
                              kVec);
}

// ─────────── Both sides nullable under FULL JOIN: base owner rejected ───────
// A FULL join makes BOTH inputs nullable. NEAREST on the base vector column
// must be rejected regardless of which side it names.
TEST_F(QA_GDB1254_NullableMatrix, FullJoinBothSidesNullable_BaseOwnerRejected) {
    expect_nullable_rejection(std::string("SELECT b.id FROM books b "
                                          "FULL JOIN notes n ON n.book_id = b.id "
                                          "WHERE NEAREST(b.description_vec, 2) TO ") +
                              kVec);
}

// ─────────── Preserved RIGHT-join NEAREST returns CORRECT rows ──────────────
// Beyond "accepted", verify the preserved-side scan under a RIGHT join actually
// returns the k nearest preserved rows (i.e. the guard change did not silently
// break the happy path it must keep allowing). `notes` is preserved on the
// right; kVec is closest to note 10 then 20.
TEST_F(QA_GDB1254_NullableMatrix, PreservedRightJoinNearestReturnsTopK) {
    auto result = engine_->execute(std::string("SELECT n.id FROM reviews r "
                                               "RIGHT JOIN notes n ON n.book_id = r.book_id "
                                               "WHERE NEAREST(n.note_vec, 2) TO ") +
                                   kVec);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{10, 20}))
        << "preserved RIGHT-join NEAREST must return the 2 nearest notes";
}

} // namespace
