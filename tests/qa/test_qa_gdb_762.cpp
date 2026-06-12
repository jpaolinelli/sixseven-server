/// @file test_qa_gdb_762.cpp
/// QA regression tests for GDB-762: Binder quantifier rejection tests.
///
/// Verifies that the Binder (not just the parser) enforces all variable-length
/// quantifier invariants declared in binder.cpp:619-638:
///   - {0,0} is rejected ("variable-length pattern {0,0} would match zero hops")
///   - {max < min} is rejected ("max_hops ... must be >= min_hops")
///   - {0,N} for N >= 1 is accepted (zero-minimum range is legal)
///   - {1,1}, {1,5}, {2,2} are accepted (well-formed positive bounds)
///   - {N} for N == 0 produces the {0,0} error
///   - {N} for N >= 1 is accepted (exact-hop shorthand)
///
/// These tests cover the project-wide gap identified in GDB-762:
/// quantifier validation was entirely untested at the Binder level.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <string>

namespace sixseven {
namespace {

// ============================================================================
// Fixture: minimal catalog with one node table and one self-referencing edge
// ============================================================================

class QA_GDB762_Binder : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        auto db = catalog_->create_database("testdb_gdb762");
        ASSERT_TRUE(db.has_value()) << db.error().message;
        db_id_ = *db;

        // Single node table "n" — reused for source and target.
        TableSchema ts;
        ts.name = "n";
        CatalogColumnDef col;
        col.ordinal = 0;
        col.name = "id";
        col.type_id = TypeId::INT64;
        col.nullable = false;
        ts.columns.push_back(col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(db_id_, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        node_tid_ = *tid;

        // Self-referencing edge type "e".
        EdgeTypeDef def;
        def.name = "e";
        def.database_id = db_id_;
        def.source_table_id = node_tid_;
        def.target_table_id = node_tid_;
        auto eid = catalog_->create_edge_type(db_id_, std::move(def));
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    /// Lex, parse and bind a single SQL statement.
    Result<BoundStatement> bind_sql(const std::string& sql) {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens)
            return tl::unexpected(tokens.error());
        Parser parser(std::move(*tokens));
        auto stmts = parser.parse_all();
        if (!stmts)
            return tl::unexpected(stmts.error());
        if (stmts->empty())
            return make_error(StatusCode::PARSE_ERROR, "no statements parsed");
        Binder binder(*catalog_, db_id_);
        return binder.bind(*stmts->front());
    }

    std::unique_ptr<Catalog> catalog_;
    database_id_t db_id_ = 0;
    table_id_t node_tid_ = 0;
};

// ============================================================================
// Rejection cases
// ============================================================================

TEST_F(QA_GDB762_Binder, RejectsZeroZeroRange) {
    // Explicit {0,0}: both bounds are zero — no hop can ever be matched.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{0,0}(b:n)");

    ASSERT_FALSE(result.has_value()) << "Binder should reject {0,0}; got success";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
    EXPECT_NE(result.error().message.find("zero hops"), std::string::npos)
        << "Expected 'zero hops' substring, got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, RejectsZeroExactShorthand) {
    // {0} is shorthand for exactly 0 hops (i.e. {0,0}) — must also be rejected.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{0}(b:n)");

    ASSERT_FALSE(result.has_value()) << "Binder should reject {0} (== {0,0}); got success";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
    EXPECT_NE(result.error().message.find("zero hops"), std::string::npos)
        << "Expected 'zero hops' substring, got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, RejectsMaxLessThanMin_5_2) {
    // {5,2}: max_hops (2) < min_hops (5).
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{5,2}(b:n)");

    ASSERT_FALSE(result.has_value()) << "Binder should reject {5,2}; got success";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
    EXPECT_NE(result.error().message.find("max_hops"), std::string::npos)
        << "Expected 'max_hops' in message, got: " << result.error().message;
    EXPECT_NE(result.error().message.find("min_hops"), std::string::npos)
        << "Expected 'min_hops' in message, got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, RejectsMaxLessThanMin_3_1) {
    // {3,1}: max_hops (1) < min_hops (3) — different values, same rule.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{3,1}(b:n)");

    ASSERT_FALSE(result.has_value()) << "Binder should reject {3,1}; got success";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
    EXPECT_NE(result.error().message.find("max_hops"), std::string::npos)
        << "Expected 'max_hops' in message, got: " << result.error().message;
}

// ============================================================================
// Acceptance cases — the Binder must NOT reject these
// ============================================================================

TEST_F(QA_GDB762_Binder, AcceptsExactOne) {
    // {1} is well-formed exact one-hop traversal.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{1}(b:n)");

    ASSERT_TRUE(result.has_value()) << "Binder should accept {1}; got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, AcceptsOneOne) {
    // {1,1}: exactly one hop.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{1,1}(b:n)");

    ASSERT_TRUE(result.has_value())
        << "Binder should accept {1,1}; got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, AcceptsOneFive) {
    // {1,5}: one to five hops.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{1,5}(b:n)");

    ASSERT_TRUE(result.has_value())
        << "Binder should accept {1,5}; got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, AcceptsTwoTwo) {
    // {2,2}: exactly two hops.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{2,2}(b:n)");

    ASSERT_TRUE(result.has_value())
        << "Binder should accept {2,2}; got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, AcceptsZeroFive) {
    // {0,5}: zero to five hops. Zero-minimum is legal as long as max >= 1.
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{0,5}(b:n)");

    ASSERT_TRUE(result.has_value())
        << "Binder should accept {0,5}; got: " << result.error().message;
}

TEST_F(QA_GDB762_Binder, AcceptsZeroOne) {
    // {0,1}: optional single hop (like ? in regex).
    auto result = bind_sql("SELECT a.id FROM MATCH (a:n)-[r:e]->{0,1}(b:n)");

    ASSERT_TRUE(result.has_value())
        << "Binder should accept {0,1}; got: " << result.error().message;
}

} // namespace
} // namespace sixseven
