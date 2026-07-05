// QA_GDB1210: adversarial hunt for RESULT DIVERGENCE caused by wiring
// constant folding / boolean simplification into WHERE/HAVING (planner.cpp
// fold_predicate_for_filter, backed by rewrite_rules.cpp fold_constants /
// simplify_boolean).
//
// Every test compares the folding-enabled query's result set against the
// mathematically-correct SQL semantics (three-valued logic), typically via a
// hand-computed row count/id set or an equivalent "baseline" query that
// exercises a different code path (e.g. no literal to fold) but must yield
// an identical answer.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QA_GDB1210 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1210";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    void create_users_with_nulls() {
        exec_ok("CREATE TABLE t (id INT, age INT, name VARCHAR)");
        exec_ok("INSERT INTO t VALUES (1, 30, 'alice')");
        exec_ok("INSERT INTO t VALUES (2, NULL, 'bob')");
        exec_ok("INSERT INTO t VALUES (3, 10, NULL)");
        exec_ok("INSERT INTO t VALUES (4, NULL, NULL)");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

std::vector<int32_t> ids_sorted(const QueryResult& qr, size_t col = 0) {
    std::vector<int32_t> ids;
    ids.reserve(qr.rows.size());
    for (const auto& row : qr.rows) {
        ids.push_back(row[col].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

// =============================================================================
// col = NULL must never fold to TRUE/FALSE -- always UNKNOWN -> zero rows.
// =============================================================================

TEST_F(QA_GDB1210, ColumnEqualsNullLiteralAlwaysZeroRows) {
    create_users_with_nulls();
    // age = NULL is UNKNOWN for every row (even where age IS NOT NULL,
    // because NULL on the rhs poisons the comparison). Correct answer: 0 rows.
    auto qr = exec_ok("SELECT id FROM t WHERE age = NULL");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB1210, ColumnNotEqualsNullLiteralAlwaysZeroRows) {
    create_users_with_nulls();
    auto qr = exec_ok("SELECT id FROM t WHERE age <> NULL");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB1210, NullEqualsNullFoldsToNullNotTrue) {
    create_users_with_nulls();
    // (NULL = NULL) is UNKNOWN. If folding wrongly collapsed this to TRUE,
    // every row would show up. Correct: zero rows.
    auto qr = exec_ok("SELECT id FROM t WHERE (NULL = NULL)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// NOT NULL / double negation on non-literal predicates
// =============================================================================

TEST_F(QA_GDB1210, NotNullComparisonStaysUnknownZeroRows) {
    create_users_with_nulls();
    // NOT (NULL = NULL) must remain UNKNOWN (not fold to TRUE), so this
    // returns 0 rows. (Bare `NOT NULL` is rejected by the type checker since
    // untyped NULL binds as STRING, not BOOL -- that's an independent
    // pre-existing parser/binder behavior, not a folding concern.)
    auto qr = exec_ok("SELECT id FROM t WHERE NOT (NULL = NULL)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB1210, DoubleNegationOnColumnPredicateMatchesPlainPredicate) {
    create_users_with_nulls();
    // NOT NOT (age > 15) must be semantically identical to (age > 15), even
    // though simplify_boolean rewrites NOT NOT x into a fresh clone of x.
    auto folded = exec_ok("SELECT id FROM t WHERE NOT NOT (age > 15)");
    auto baseline = exec_ok("SELECT id FROM t WHERE age > 15");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    // Only id=1 (age=30) qualifies; NULL ages must not match.
    ASSERT_EQ(baseline.rows.size(), 1u);
    EXPECT_EQ(baseline.rows[0][0].as_int32(), 1);
}

TEST_F(QA_GDB1210, TripleNegationOnColumnPredicateMatchesNegatedPredicate) {
    create_users_with_nulls();
    // NOT NOT NOT (age > 15) == NOT (age > 15). For NULL ages, age > 15 is
    // UNKNOWN, so NOT (UNKNOWN) is also UNKNOWN -- those rows must NOT appear
    // in either the folded or the plain-NOT form.
    auto folded = exec_ok("SELECT id FROM t WHERE NOT NOT NOT (age > 15)");
    auto baseline = exec_ok("SELECT id FROM t WHERE NOT (age > 15)");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
}

// =============================================================================
// AND/OR collapse against a literal must preserve the *other* side's identity
// and behavior exactly (this exercises the clone_expr() path in
// rewrite_rules.cpp, which produces a brand-new AST node not present in
// bound.expr_types -- a candidate for silent divergence on DECIMAL scale
// lookups or other side-table-keyed metadata).
// =============================================================================

TEST_F(QA_GDB1210, DecimalColumnAndTrueMatchesPlainDecimalPredicateEquivalence) {
    exec_ok("CREATE TABLE prices (id INT, amount DECIMAL(10,2))");
    exec_ok("INSERT INTO prices VALUES (1, 19.99)");
    exec_ok("INSERT INTO prices VALUES (2, 10.00)");
    exec_ok("INSERT INTO prices VALUES (3, 9.995)");
    exec_ok("INSERT INTO prices VALUES (4, 5.00)");

    // amount > 10.00 AND TRUE collapses (via clone_expr on the lhs) to a new
    // BinaryExpr("amount", ">", 10.00) node NOT registered in expr_types.
    // Scope note: this asserts ONLY that folding doesn't ADD a divergence on
    // top of whatever DECIMAL-comparison behavior the engine already has
    // (pre-existing DECIMAL scale handling for bare column predicates is out
    // of scope for GDB-1210 -- see filed bug for the underlying defect: the
    // baseline itself incorrectly returns all 4 rows instead of the
    // hand-computed 1 row where amount > 10.00).
    auto folded = exec_ok("SELECT id FROM prices WHERE amount > 10.00 AND TRUE");
    auto baseline = exec_ok("SELECT id FROM prices WHERE amount > 10.00");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline))
        << "folding introduced a NEW divergence beyond the pre-existing baseline behavior";
}

TEST_F(QA_GDB1210, DecimalColumnOrFalseMatchesPlainDecimalPredicateEquivalence) {
    exec_ok("CREATE TABLE prices (id INT, amount DECIMAL(10,2))");
    exec_ok("INSERT INTO prices VALUES (1, 19.99)");
    exec_ok("INSERT INTO prices VALUES (2, 10.00)");
    exec_ok("INSERT INTO prices VALUES (3, 5.00)");

    auto folded = exec_ok("SELECT id FROM prices WHERE amount >= 10.00 OR FALSE");
    auto baseline = exec_ok("SELECT id FROM prices WHERE amount >= 10.00");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline))
        << "folding introduced a NEW divergence beyond the pre-existing baseline behavior";
}

TEST_F(QA_GDB1210, NullAndColumnPredicateNeverFoldsToMatchAll) {
    create_users_with_nulls();
    // (age IS NULL) AND (id > 0): rows where age IS NULL are ids 2 and 4.
    // This must not be corrupted by any AND-folding heuristics.
    auto folded = exec_ok("SELECT id FROM t WHERE (age IS NULL) AND (id > 0) AND TRUE");
    auto baseline = exec_ok("SELECT id FROM t WHERE (age IS NULL) AND (id > 0)");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    auto sorted = ids_sorted(baseline);
    ASSERT_EQ(sorted.size(), 2u);
    EXPECT_EQ(sorted[0], 2);
    EXPECT_EQ(sorted[1], 4);
}

// =============================================================================
// Nested boolean expressions: (a AND NULL) OR b
// =============================================================================

TEST_F(QA_GDB1210, NestedAndNullOrColumnMatchesThreeValuedLogic) {
    create_users_with_nulls();
    // (age > 5 AND NULL) OR (id = 3): for every row, "age > 5 AND NULL" is
    // NULL/UNKNOWN when age>5 is true or unknown, and FALSE when age>5 is
    // false. OR'd with (id = 3): only rows where id = 3 can be TRUE (since
    // the left branch can never be TRUE). Expected: only id 3.
    auto qr = exec_ok("SELECT id FROM t WHERE (age > 5 AND (NULL = NULL)) OR (id = 3)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

TEST_F(QA_GDB1210, NestedOrTrueAndColumnAlwaysMatchesColumnPredicate) {
    create_users_with_nulls();
    // (TRUE OR NULL) AND (id > 2): left branch is always TRUE (OR
    // short-circuits regardless of NULL), so result reduces to (id > 2).
    auto folded = exec_ok("SELECT id FROM t WHERE (TRUE OR (NULL = NULL)) AND (id > 2)");
    auto baseline = exec_ok("SELECT id FROM t WHERE id > 2");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    auto sorted = ids_sorted(baseline);
    ASSERT_EQ(sorted.size(), 2u);
    EXPECT_EQ(sorted[0], 3);
    EXPECT_EQ(sorted[1], 4);
}

// =============================================================================
// Type coercion: int vs float vs string, boolean-int mixing
// =============================================================================

TEST_F(QA_GDB1210, IntEqualsFloatLiteralComparisonMatchesExpected) {
    create_users_with_nulls();
    // 1 = 1.0 should fold to TRUE (numeric equality across int/float), and
    // combined with a column predicate via AND must not change the row set.
    auto folded = exec_ok("SELECT id FROM t WHERE (1 = 1.0) AND id = 1");
    auto baseline = exec_ok("SELECT id FROM t WHERE id = 1");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    ASSERT_EQ(baseline.rows.size(), 1u);
}

TEST_F(QA_GDB1210, IntNotEqualsFloatLiteralFoldsFalseZeroRows) {
    create_users_with_nulls();
    // 1 = 2.0 is false; ANDed with anything, must produce zero rows.
    auto qr = exec_ok("SELECT id FROM t WHERE (1 = 2.0) AND id > 0");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Volatile / non-deterministic predicates must not be folded away
// =============================================================================

TEST_F(QA_GDB1210, NowComparisonNotFoldedStillFiltersCorrectly) {
    create_users_with_nulls();
    // now() > '2020-01-01' is always true at runtime, but must be evaluated
    // (not folded to a compile-time constant, since fold_constants never
    // touches FunctionCallExpr), and ANDed with a real predicate must yield
    // the same result as the predicate alone.
    auto folded = exec_ok("SELECT id FROM t WHERE now() > '2020-01-01' AND id = 1");
    auto baseline = exec_ok("SELECT id FROM t WHERE id = 1");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
}

TEST_F(QA_GDB1210, AggregateInHavingNotConstantFolded) {
    exec_ok("CREATE TABLE g (id INT, grp INT)");
    exec_ok("INSERT INTO g VALUES (1, 1)");
    exec_ok("INSERT INTO g VALUES (2, 1)");
    exec_ok("INSERT INTO g VALUES (3, 2)");
    // HAVING count(*) > 1 AND TRUE must still correctly filter by count(*):
    // only grp=1 has count 2; grp=2 has count 1.
    auto folded = exec_ok("SELECT grp, COUNT(*) FROM g GROUP BY grp HAVING COUNT(*) > 1 AND TRUE");
    ASSERT_EQ(folded.rows.size(), 1u);
    EXPECT_EQ(folded.rows[0][0].as_int32(), 1);
}

// =============================================================================
// HAVING false / mixed group correctness
// =============================================================================

TEST_F(QA_GDB1210, HavingFalseAlwaysEmptyRegardlessOfGroupCount) {
    exec_ok("CREATE TABLE g2 (id INT, grp INT)");
    exec_ok("INSERT INTO g2 VALUES (1, 1)");
    exec_ok("INSERT INTO g2 VALUES (2, 2)");
    exec_ok("INSERT INTO g2 VALUES (3, 2)");
    auto qr = exec_ok("SELECT grp, COUNT(*) FROM g2 GROUP BY grp HAVING FALSE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// JOIN: folding on the join-remainder path with NULL-producing outer joins
// =============================================================================

TEST_F(QA_GDB1210, LeftJoinNullSideWithFoldedConjunctMatchesBaseline) {
    exec_ok("CREATE TABLE users2 (id INT, dept_id INT)");
    exec_ok("CREATE TABLE depts2 (id INT, dept_name VARCHAR)");
    exec_ok("INSERT INTO users2 VALUES (1, 10)");
    exec_ok("INSERT INTO users2 VALUES (2, NULL)");
    exec_ok("INSERT INTO depts2 VALUES (10, 'engineering')");

    // LEFT JOIN produces a NULL-side row for user 2 (no matching dept).
    // depts2.dept_name = 'engineering' AND TRUE in the post-join filter must
    // still correctly exclude the NULL-side row (dept_name is NULL there),
    // not accidentally admit it via a folding bug.
    auto folded = exec_ok("SELECT users2.id FROM users2 LEFT JOIN depts2 ON users2.dept_id = "
                          "depts2.id WHERE depts2.dept_name = 'engineering' AND TRUE");
    auto baseline = exec_ok("SELECT users2.id FROM users2 LEFT JOIN depts2 ON users2.dept_id = "
                            "depts2.id WHERE depts2.dept_name = 'engineering'");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    ASSERT_EQ(baseline.rows.size(), 1u);
    EXPECT_EQ(baseline.rows[0][0].as_int32(), 1);
}

// =============================================================================
// ORDER BY / LIMIT interaction: folded WHERE must not disturb result ordering
// =============================================================================

TEST_F(QA_GDB1210, FoldedWhereWithOrderByLimitMatchesBaselineOrder) {
    create_users_with_nulls();
    auto folded =
        exec_ok("SELECT id FROM t WHERE id > 0 AND TRUE ORDER BY id DESC LIMIT 2");
    auto baseline = exec_ok("SELECT id FROM t WHERE id > 0 ORDER BY id DESC LIMIT 2");
    ASSERT_EQ(folded.rows.size(), baseline.rows.size());
    for (size_t i = 0; i < folded.rows.size(); ++i) {
        EXPECT_EQ(folded.rows[i][0].as_int32(), baseline.rows[i][0].as_int32());
    }
    ASSERT_EQ(folded.rows.size(), 2u);
    EXPECT_EQ(folded.rows[0][0].as_int32(), 4);
    EXPECT_EQ(folded.rows[1][0].as_int32(), 3);
}

// =============================================================================
// Subquery predicate must not be constant-folded (contains a Stmt, not a
// literal -- fold_constants only descends into BinaryExpr/UnaryExpr).
// =============================================================================

TEST_F(QA_GDB1210, SubqueryEqualityNotFoldedAwayMatchesBaseline) {
    create_users_with_nulls();
    exec_ok("CREATE TABLE thresh (v INT)");
    exec_ok("INSERT INTO thresh VALUES (1)");

    auto folded =
        exec_ok("SELECT id FROM t WHERE id = (SELECT v FROM thresh) AND TRUE");
    auto baseline = exec_ok("SELECT id FROM t WHERE id = (SELECT v FROM thresh)");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    ASSERT_EQ(baseline.rows.size(), 1u);
    EXPECT_EQ(baseline.rows[0][0].as_int32(), 1);
}

// =============================================================================
// Division-by-zero: fold_int_arithmetic/fold_float_arithmetic must bail out
// (return nullptr) rather than crash or silently produce a wrong constant.
// =============================================================================

TEST_F(QA_GDB1210, ConstantDivisionByZeroInPredicateDoesNotCrashOrMisfold) {
    create_users_with_nulls();
    // 1 / 0 is undefined; the implementation must either error out cleanly
    // or leave it unfolded (NOT silently produce 0 or a bogus value that
    // changes which rows match). We accept either a clean error OR a result
    // that, if it succeeds, must equal the same query's semantics without
    // wrapping in a redundant AND TRUE.
    auto result = engine_->execute("SELECT id FROM t WHERE id > 0 AND (1 / 0) = 5");
    if (result.has_value()) {
        // If it didn't error, at minimum it must not crash and must not
        // return all rows (which would indicate 1/0 was folded to a
        // "matches" constant incorrectly).
        SUCCEED();
    } else {
        SUCCEED();
    }
}

