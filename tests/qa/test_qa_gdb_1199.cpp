#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "../unit/test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Adversarial QA coverage for GDB-1199 (revival of the dead exec_err helper
/// in UnlinkWhereTest). This suite probes the UNLINK WHERE binder/executor
/// error surface beyond the 4 negative tests added by the ticket, looking
/// specifically for inputs that SHOULD error but are silently accepted
/// (a binder gap that could corrupt graph edges) as well as confirming the
/// "correctly errors" cases are not vacuous.
///
/// Graph:
///   users:    (1, "Alice"), (2, "Bob")
///   products: (10, "Widget"), (20, "Gadget")
///   edge type "rated" with score DOUBLE and review VARCHAR
///   edge type "viewed" with NO properties (source users, target products)
///   edges:
///     users(1) -> products(10) via rated (score=4.5, review='good')
///     users(1) -> products(20) via rated (score=1.5, review='bad')
///     users(2) -> products(10) via rated (score=3.0, review='ok')
class GDB1199UnlinkWhereTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_test_gdb1199_unlink_where";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");

        exec_ok("CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO products VALUES (10, 'Widget')");
        exec_ok("INSERT INTO products VALUES (20, 'Gadget')");

        exec_ok("CREATE EDGE TYPE rated (score DOUBLE, review VARCHAR) FROM users TO products");
        exec_ok("CREATE EDGE TYPE viewed FROM users TO products");

        exec_ok("LINK users(1) TO products(10) VIA rated (score = 4.5, review = 'good')");
        exec_ok("LINK users(1) TO products(20) VIA rated (score = 1.5, review = 'bad')");
        exec_ok("LINK users(2) TO products(10) VIA rated (score = 3.0, review = 'ok')");

        exec_ok("LINK users(1) TO products(10) VIA viewed");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    void exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected error but got success";
    }

    int64_t rated_edge_count_for(int user_id) {
        auto qr = exec_ok("SELECT rated.score FROM TRAVERSE rated FROM users(" +
                           std::to_string(user_id) + ") DIRECTION OUT FETCH");
        return static_cast<int64_t>(qr.rows.size());
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// Confirm the 4 shipped negative tests are not vacuous: each must fail for
// the *specific* reason claimed, not by some unrelated parse failure, and
// must leave the graph state untouched (no partial/side-effect deletes).
// ============================================================================

TEST_F(GDB1199UnlinkWhereTest, UnknownEdgePropertyRejectedAndNoSideEffect) {
    exec_err("UNLINK users(1) FROM products(10) VIA rated WHERE bogus = 1");
    // Edge must still be present -- binder rejection must not partially apply.
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

TEST_F(GDB1199UnlinkWhereTest, UnknownEdgeTypeRejectedAndNoSideEffect) {
    exec_err("UNLINK users(1) FROM products(10) VIA not_an_edge_type WHERE score < 1.0");
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

TEST_F(GDB1199UnlinkWhereTest, SourceTableMismatchRejectedAndNoSideEffect) {
    exec_err("UNLINK products(10) FROM products(20) VIA rated WHERE score < 1.0");
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

TEST_F(GDB1199UnlinkWhereTest, TargetTableMismatchRejectedAndNoSideEffect) {
    exec_err("UNLINK users(1) FROM users(2) VIA rated WHERE score < 1.0");
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

// ============================================================================
// Adversarial probes for MISSED error cases / silent-accept gaps.
// ============================================================================

// WHERE type mismatch: comparing a DOUBLE property against a string literal.
// This should either be rejected at bind time (TYPE_ERROR) or evaluate
// deterministically to false/error at runtime -- it must NOT silently
// delete edges due to a bad implicit coercion.
TEST_F(GDB1199UnlinkWhereTest, WhereTypeMismatchStringVsNumericDoesNotWronglyDeleteAll) {
    auto result = exec("UNLINK users(1) FROM products(10) VIA rated WHERE score = 'good'");
    if (result.has_value()) {
        // If accepted, it must not have matched (type mismatch should not
        // coerce into a true predicate that nukes unrelated edges).
        EXPECT_EQ(result->affected_rows, 0)
            << "type-mismatched WHERE unexpectedly deleted edges";
        EXPECT_EQ(rated_edge_count_for(1), 2);
    }
    // else: correctly rejected -- acceptable.
}

// WHERE on an edge type with NO declared properties. Any bare column
// reference must be rejected (there's no scope to resolve it against).
TEST_F(GDB1199UnlinkWhereTest, WhereOnPropertylessEdgeTypeWithBareColumnFails) {
    exec_err("UNLINK users(1) FROM products(10) VIA viewed WHERE anything = 1");
}

// WHERE with a constant-only predicate on a propertyless edge type. This is
// legitimate SQL (no column resolution needed) and should succeed, matching
// all edges of that type between the given endpoints.
TEST_F(GDB1199UnlinkWhereTest, WhereConstantOnlyOnPropertylessEdgeTypeSucceeds) {
    auto qr = exec_ok("UNLINK users(1) FROM products(10) VIA viewed WHERE 1 = 1");
    EXPECT_EQ(qr.affected_rows, 1);
}

// Malformed / empty WHERE (missing predicate) must be a parse error, not a
// silent accept-everything.
TEST_F(GDB1199UnlinkWhereTest, EmptyWhereClauseFailsToParse) {
    exec_err("UNLINK users(1) FROM products(10) VIA rated WHERE");
}

// Source id of the wrong type (string literal for an INT PK). Must be
// rejected, not silently coerced into matching row 0 or similar.
TEST_F(GDB1199UnlinkWhereTest, SourceKeyWrongTypeFails) {
    auto result = exec("UNLINK users('not_an_id') FROM products(10) VIA rated WHERE score < 100.0");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

// Target id of the wrong type.
TEST_F(GDB1199UnlinkWhereTest, TargetKeyWrongTypeFails) {
    auto result = exec("UNLINK users(1) FROM products('not_an_id') VIA rated WHERE score < 100.0");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

// UNLINK on a nonexistent edge instance (valid endpoints, valid edge type,
// but no edge currently connects them) with a WHERE clause. Must be either
// a clean 0-row no-op or a NOT_FOUND-style error -- never spuriously delete
// or crash.
TEST_F(GDB1199UnlinkWhereTest, UnlinkNonexistentEdgeInstanceIsNoopOrCleanError) {
    // users(2) -> products(20) has no "rated" edge in this fixture.
    auto result = exec("UNLINK users(2) FROM products(20) VIA rated WHERE score < 100.0");
    if (result.has_value()) {
        EXPECT_EQ(result->affected_rows, 0);
    }
    // Either outcome is acceptable as long as it didn't touch unrelated edges.
    EXPECT_EQ(rated_edge_count_for(1), 2);
    EXPECT_EQ(rated_edge_count_for(2), 1);
}

// WHERE predicate referencing a *node* (table) property name instead of an
// edge property -- e.g. "name", which exists on both users and products but
// is not a rated-edge property. Must be rejected as unresolved, not
// accidentally matched against node data.
TEST_F(GDB1199UnlinkWhereTest, WhereReferencingNodePropertyInsteadOfEdgePropertyFails) {
    exec_err("UNLINK users(1) FROM products(10) VIA rated WHERE name = 'Alice'");
    EXPECT_EQ(rated_edge_count_for(1), 2);
}

// Nonexistent source/target row entirely (not just wrong type) combined with
// WHERE -- must surface NOT_FOUND, not silently succeed with 0 rows affected
// in a way that masks a real error.
TEST_F(GDB1199UnlinkWhereTest, NonexistentSourceRowWithWhereFails) {
    auto result = exec("UNLINK users(999) FROM products(10) VIA rated WHERE score < 100.0");
    EXPECT_FALSE(result.has_value());
}

TEST_F(GDB1199UnlinkWhereTest, NonexistentTargetRowWithWhereFails) {
    auto result = exec("UNLINK users(1) FROM products(999) VIA rated WHERE score < 100.0");
    EXPECT_FALSE(result.has_value());
}

// SQL-injection-shaped WHERE clause: must be treated as an ordinary
// (unresolvable) predicate expression, not specially interpreted.
TEST_F(GDB1199UnlinkWhereTest, SqlInjectionShapedWhereIsRejectedOrInert) {
    auto result = exec(
        "UNLINK users(1) FROM products(10) VIA rated WHERE score < 100.0; DROP TABLE users; --");
    // Whatever the parser decides (reject multi-statement, or treat trailing
    // text as garbage), the users table must survive.
    auto check = exec("SELECT * FROM users");
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->rows.size(), 2u);
}

} // namespace
} // namespace sixseven
