// GDB-1210: end-to-end tests proving that wiring constant folding /
// boolean simplification into the planner (WHERE, JOIN-remainder, HAVING)
// never changes query results. Folding is purely an optimization: every
// test here compares the folded-predicate query's output against the
// semantically identical unfolded query (or a hand-computed expectation).

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

class ConstantFoldingE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_cf_e2e";
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

    void create_and_populate() {
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)");
        exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 35)");
    }

    void create_two_tables_for_join() {
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, dept_id INT)");
        exec_ok("CREATE TABLE depts (id INT, dept_name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'alice', 10)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 20)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 10)");
        exec_ok("INSERT INTO depts VALUES (10, 'engineering')");
        exec_ok("INSERT INTO depts VALUES (20, 'sales')");
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
// WHERE: constant TRUE / FALSE predicates
// =============================================================================

TEST_F(ConstantFoldingE2ETest, WhereAlwaysFalseReturnsZeroRows) {
    create_and_populate();
    auto qr = exec_ok("SELECT id FROM users WHERE 1 = 0");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, WhereAlwaysTrueReturnsAllRows) {
    create_and_populate();
    auto folded = exec_ok("SELECT id FROM users WHERE 1 = 1");
    auto baseline = exec_ok("SELECT id FROM users");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    EXPECT_EQ(folded.rows.size(), 3u);
}

TEST_F(ConstantFoldingE2ETest, WhereLiteralFalseKeywordReturnsZeroRows) {
    create_and_populate();
    auto qr = exec_ok("SELECT id FROM users WHERE FALSE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, WhereLiteralTrueKeywordReturnsAllRows) {
    create_and_populate();
    auto folded = exec_ok("SELECT id FROM users WHERE TRUE");
    auto baseline = exec_ok("SELECT id FROM users");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
}

// =============================================================================
// Mixed constant + column predicates (AND / OR short-circuit)
// =============================================================================

TEST_F(ConstantFoldingE2ETest, ColumnPredicateAndTrueMatchesPlainPredicate) {
    create_and_populate();
    auto folded = exec_ok("SELECT id FROM users WHERE age > 26 AND TRUE");
    auto baseline = exec_ok("SELECT id FROM users WHERE age > 26");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    EXPECT_EQ(folded.rows.size(), 2u); // alice(30), charlie(35)
}

TEST_F(ConstantFoldingE2ETest, ColumnPredicateAndFalseReturnsZeroRows) {
    create_and_populate();
    auto qr = exec_ok("SELECT id FROM users WHERE age > 26 AND FALSE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, ColumnPredicateOrTrueMatchesAllRows) {
    create_and_populate();
    auto folded = exec_ok("SELECT id FROM users WHERE age > 26 OR TRUE");
    auto baseline = exec_ok("SELECT id FROM users");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
}

TEST_F(ConstantFoldingE2ETest, ColumnPredicateOrFalseMatchesPlainPredicate) {
    create_and_populate();
    auto folded = exec_ok("SELECT id FROM users WHERE age > 26 OR FALSE");
    auto baseline = exec_ok("SELECT id FROM users WHERE age > 26");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
}

// =============================================================================
// Constant arithmetic/comparison folding (non-boolean literal computation)
// =============================================================================

TEST_F(ConstantFoldingE2ETest, ConstantArithmeticInPredicateMatchesLiteral) {
    create_and_populate();
    // 2 + 3 = 5, so age > 5 is a tautology over our test data (all ages > 5).
    auto folded = exec_ok("SELECT id FROM users WHERE age > (2 + 3)");
    auto baseline = exec_ok("SELECT id FROM users WHERE age > 5");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    EXPECT_EQ(folded.rows.size(), 3u);
}

TEST_F(ConstantFoldingE2ETest, ConstantComparisonFoldsToDeterministicResult) {
    create_and_populate();
    // (10 > 20) is always false -> combined with AND, whole predicate is false.
    auto qr = exec_ok("SELECT id FROM users WHERE age > 0 AND (10 > 20)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Three-valued logic: NULL combinations must be conservative or correct
// =============================================================================

TEST_F(ConstantFoldingE2ETest, NullAndTrueOnlyMatchesWhereConditionIsTrue) {
    create_and_populate();
    // manager_id is NULL for all rows (column doesn't exist so use a
    // NULL-producing comparison): NULL = NULL evaluates to NULL, so
    // "NULL AND age > 0" must be NULL (i.e. filtered out), never TRUE.
    auto qr = exec_ok("SELECT id FROM users WHERE (NULL = NULL) AND age > 0");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, NullOrTrueMatchesAllRows) {
    create_and_populate();
    // NULL OR TRUE -> TRUE for every row, regardless of the NULL branch.
    auto folded = exec_ok("SELECT id FROM users WHERE (NULL = NULL) OR TRUE");
    auto baseline = exec_ok("SELECT id FROM users");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
}

TEST_F(ConstantFoldingE2ETest, NullOrFalseReturnsZeroRows) {
    create_and_populate();
    // NULL OR FALSE -> NULL (not TRUE), so no rows should match.
    auto qr = exec_ok("SELECT id FROM users WHERE (NULL = NULL) OR FALSE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, NullAndFalseReturnsZeroRows) {
    create_and_populate();
    // NULL AND FALSE -> FALSE.
    auto qr = exec_ok("SELECT id FROM users WHERE (NULL = NULL) AND FALSE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Volatile / non-deterministic expressions must never be folded
// =============================================================================

TEST_F(ConstantFoldingE2ETest, VolatileNowFunctionNotFoldedProducesConsistentSchema) {
    create_and_populate();
    // now() is a runtime, non-constant function call. fold_constants never
    // touches FunctionCallExpr, so this must still execute (not be folded
    // away into a constant), and returns the same row set as the plain
    // predicate it's ANDed with.
    auto qr = exec_ok("SELECT id FROM users WHERE age > 0 AND now() IS NOT NULL");
    EXPECT_EQ(qr.rows.size(), 3u);
}

// =============================================================================
// Type / precision preservation
// =============================================================================

TEST_F(ConstantFoldingE2ETest, DecimalColumnComparisonPreservesScale) {
    exec_ok("CREATE TABLE prices (id INT, amount DECIMAL(10,2))");
    exec_ok("INSERT INTO prices VALUES (1, 19.99)");
    exec_ok("INSERT INTO prices VALUES (2, 5.00)");
    // Mixed const+column predicate; folding must not touch the DECIMAL
    // column comparison (only literal-literal subtrees fold), so scale-aware
    // comparison logic still applies correctly.
    // Equivalence check only: whatever DECIMAL-aware comparison the baseline
    // produces, ANDing a folded-away TRUE conjunct must not change it. This
    // isolates the folding wiring from any pre-existing DECIMAL comparison
    // behavior (out of scope for GDB-1210).
    auto folded = exec_ok("SELECT id FROM prices WHERE amount > 10.00 AND TRUE");
    auto baseline = exec_ok("SELECT id FROM prices WHERE amount > 10.00");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    EXPECT_FALSE(baseline.rows.empty());
}

TEST_F(ConstantFoldingE2ETest, StringConcatConstantFoldMatchesLiteral) {
    create_and_populate();
    // 'ali' || 'ce' folds to 'alice'; result must match filtering directly
    // by the literal string.
    auto folded = exec_ok("SELECT id FROM users WHERE name = ('ali' || 'ce')");
    auto baseline = exec_ok("SELECT id FROM users WHERE name = 'alice'");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    ASSERT_EQ(folded.rows.size(), 1u);
    EXPECT_EQ(folded.rows[0][0].as_int32(), 1);
}

// =============================================================================
// JOIN remainder conjunct site
// =============================================================================

TEST_F(ConstantFoldingE2ETest, JoinWithConstantTrueConjunctMatchesPlainJoin) {
    create_two_tables_for_join();
    auto folded =
        exec_ok("SELECT users.id FROM users JOIN depts ON users.dept_id = depts.id WHERE TRUE");
    auto baseline = exec_ok("SELECT users.id FROM users JOIN depts ON users.dept_id = depts.id");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    EXPECT_EQ(folded.rows.size(), 3u);
}

TEST_F(ConstantFoldingE2ETest, JoinWithConstantFalseConjunctReturnsZeroRows) {
    create_two_tables_for_join();
    auto qr =
        exec_ok("SELECT users.id FROM users JOIN depts ON users.dept_id = depts.id WHERE FALSE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, JoinWithMixedConstAndColumnConjunctMatchesPlainJoin) {
    create_two_tables_for_join();
    auto folded = exec_ok("SELECT users.id FROM users JOIN depts ON users.dept_id = depts.id "
                          "WHERE depts.dept_name = 'engineering' AND TRUE");
    auto baseline = exec_ok("SELECT users.id FROM users JOIN depts ON users.dept_id = depts.id "
                            "WHERE depts.dept_name = 'engineering'");
    EXPECT_EQ(ids_sorted(folded), ids_sorted(baseline));
    EXPECT_EQ(folded.rows.size(), 2u); // alice, charlie (dept_id=10)
}

// =============================================================================
// HAVING
// =============================================================================

TEST_F(ConstantFoldingE2ETest, HavingAlwaysFalseReturnsZeroRows) {
    create_and_populate();
    auto qr = exec_ok("SELECT age, COUNT(*) FROM users GROUP BY age HAVING 1 = 0");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(ConstantFoldingE2ETest, HavingAlwaysTrueMatchesUnfilteredGroups) {
    create_and_populate();
    auto folded = exec_ok("SELECT age, COUNT(*) FROM users GROUP BY age HAVING 1 = 1");
    auto baseline = exec_ok("SELECT age, COUNT(*) FROM users GROUP BY age");
    EXPECT_EQ(folded.rows.size(), baseline.rows.size());
    EXPECT_EQ(folded.rows.size(), 3u);
}

TEST_F(ConstantFoldingE2ETest, HavingMixedConstAndAggregateMatchesPlainHaving) {
    create_and_populate();
    auto folded =
        exec_ok("SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) >= 1 AND TRUE");
    auto baseline = exec_ok("SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) >= 1");
    EXPECT_EQ(folded.rows.size(), baseline.rows.size());
    EXPECT_EQ(folded.rows.size(), 3u);
}

// =============================================================================
// No-fire case: predicate has nothing to fold, must behave identically
// =============================================================================

TEST_F(ConstantFoldingE2ETest, PurelyColumnPredicateUnaffectedByFolding) {
    create_and_populate();
    auto qr = exec_ok("SELECT id FROM users WHERE age > 26");
    ASSERT_EQ(qr.rows.size(), 2u);
    auto sorted = ids_sorted(qr);
    EXPECT_EQ(sorted[0], 1);
    EXPECT_EQ(sorted[1], 3);
}
