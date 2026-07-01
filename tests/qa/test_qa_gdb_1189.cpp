/// @file test_qa_gdb_1189.cpp
/// @brief QA regression tests for GDB-1189.
///
/// GDB-1189 strengthened two existing dev tests
/// (SubqueryTest.CorrelatedExistsDecorrelatedToSemiJoin and
/// SubqueryTest.CorrelatedNotExistsDecorrelatedToAntiJoin in
/// tests/unit/test_subquery.cpp) so the correlated-subquery predicate
/// (`orders.amount >= 300` instead of the non-discriminating `>= 200`)
/// actually discriminates between users, meaning a dropped or mishandled
/// extra correlated conjunct during EXISTS/NOT EXISTS decorrelation into a
/// SEMI/ANTI join would now cause the test to fail.
///
/// This is a test-only change (no production code touched). QA here focuses
/// on independently confirming, with a fresh set of adversarial fixtures, that
/// the decorrelated SEMI/ANTI join path correctly honors an *additional*
/// correlated conjunct beyond the join equality -- including boundary values
/// right at the threshold, NULL amounts, ties, and users with zero matching
/// rows -- so a regression in rewrite_subquery_predicates() (src/executor/
/// planner.cpp) would be caught by an independent test, not just the one the
/// ticket edited.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QAGdb1189Test : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1189";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
        exec_ok("CREATE TABLE orders (id INT, user_id INT, amount INT)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\nError: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    std::unordered_set<std::string> collect_column_strings(const QueryResult& qr, size_t col) {
        std::unordered_set<std::string> result;
        for (const auto& row : qr.rows) {
            result.insert(row[col].as_string());
        }
        return result;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Boundary values right at the >= 300 threshold used by the strengthened tests
// =============================================================================

TEST_F(QAGdb1189Test, ExistsSemiJoinExactBoundaryIncluded) {
    // A single order at exactly the threshold must satisfy `>= 300` (inclusive).
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, 300)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

TEST_F(QAGdb1189Test, ExistsSemiJoinOneBelowBoundaryExcluded) {
    // One unit below the threshold must NOT satisfy `>= 300`.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, 299)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QAGdb1189Test, NotExistsAntiJoinExactBoundaryExcludesUser) {
    // A qualifying order at exactly the threshold means NOT EXISTS must be false
    // for that user (they are excluded from the anti-join result).
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, 300)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QAGdb1189Test, NotExistsAntiJoinOneBelowBoundaryIncludesUser) {
    // An order one unit below the threshold does not qualify, so NOT EXISTS is
    // true and the user IS included in the anti-join result.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, 299)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

// =============================================================================
// Mixed qualifying / non-qualifying orders for the same user
// =============================================================================

TEST_F(QAGdb1189Test, ExistsSemiJoinOneQualifyingOrderAmongMany) {
    // alice has 3 orders; only one (exactly 300) qualifies. EXISTS should still
    // report true, and the SEMI join must not duplicate alice's row despite
    // multiple matching underlying rows (semi join is existence-only).
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, 50)");
    exec_ok("INSERT INTO orders VALUES (101, 1, 300)");
    exec_ok("INSERT INTO orders VALUES (102, 1, 999)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

TEST_F(QAGdb1189Test, ExistsSemiJoinNoQualifyingOrderAmongMany) {
    // bob has 3 orders, all below the threshold -- EXISTS must be false.
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("INSERT INTO orders VALUES (200, 2, 10)");
    exec_ok("INSERT INTO orders VALUES (201, 2, 299)");
    exec_ok("INSERT INTO orders VALUES (202, 2, 0)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Users with zero orders at all (not merely zero qualifying orders)
// =============================================================================

TEST_F(QAGdb1189Test, NotExistsAntiJoinUserWithNoOrdersAtAllIncluded) {
    exec_ok("INSERT INTO users VALUES (3, 'charlie')");
    // No orders inserted for charlie at all.

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(QAGdb1189Test, ExistsSemiJoinEmptyOrdersTableReturnsEmpty) {
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    // orders table has zero rows.

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// NULL handling in the correlated conjunct
// =============================================================================

TEST_F(QAGdb1189Test, ExistsSemiJoinNullAmountDoesNotQualify) {
    // A NULL amount must not satisfy `>= 300` (three-valued logic: comparison
    // against NULL is UNKNOWN, not TRUE), so EXISTS must be false for a user
    // whose only order has a NULL amount.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QAGdb1189Test, NotExistsAntiJoinNullAmountUserIsIncluded) {
    // Mirror of the above via NOT EXISTS: a NULL-amount-only user has no
    // qualifying order, so NOT EXISTS is true and they belong in the anti-join
    // result.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, 1, NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

TEST_F(QAGdb1189Test, ExistsSemiJoinNullUserIdNeverMatches) {
    // An order row with a NULL user_id must never join to any user via the
    // equality correlation, regardless of amount.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO orders VALUES (100, NULL, 999)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 300)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Adversarial: prove the discriminating threshold actually distinguishes from
// the old non-discriminating >= 200 predicate the ticket flagged as vacuous.
// =============================================================================

TEST_F(QAGdb1189Test, ThresholdChoiceMustChangeResultRelativeToNonDiscriminatingBaseline) {
    // Reproduces the audit's exact concern: with a non-discriminating predicate
    // (every order satisfies it), EXISTS collapses to the unfiltered ExistsBasic
    // result. With a discriminating predicate, the result set must differ.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (3, 'charlie')");
    exec_ok("INSERT INTO orders VALUES (100, 1, 500)");
    exec_ok("INSERT INTO orders VALUES (101, 1, 300)");
    exec_ok("INSERT INTO orders VALUES (102, 3, 200)");

    // Non-discriminating: every seeded order satisfies `>= 200`.
    auto qr_baseline =
        exec_ok("SELECT users.name FROM users "
                "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                "orders.amount >= 200)");
    auto names_baseline = collect_column_strings(qr_baseline, 0);
    EXPECT_EQ(names_baseline.size(), 2u);
    EXPECT_TRUE(names_baseline.count("alice"));
    EXPECT_TRUE(names_baseline.count("charlie"));

    // Discriminating: only alice's orders satisfy `>= 300`.
    auto qr_discriminating =
        exec_ok("SELECT users.name FROM users "
                "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                "orders.amount >= 300)");
    auto names_discriminating = collect_column_strings(qr_discriminating, 0);
    EXPECT_EQ(names_discriminating.size(), 1u);
    EXPECT_TRUE(names_discriminating.count("alice"));
    EXPECT_FALSE(names_discriminating.count("charlie"));

    // The whole point of the ticket: these two result sets must NOT be equal.
    EXPECT_NE(names_baseline, names_discriminating);
}

// =============================================================================
// Compound correlated predicates: three-way AND, to ensure decorrelation
// preserves *all* conjuncts, not just the first two.
// =============================================================================

TEST_F(QAGdb1189Test, ExistsSemiJoinThreeWayAndAllConjunctsHonored) {
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    // alice: qualifying order (id=101 in range, amount >= 300).
    exec_ok("INSERT INTO orders VALUES (101, 1, 300)");
    // bob: amount qualifies but id does not (id < 100 fails the third conjunct).
    exec_ok("INSERT INTO orders VALUES (50, 2, 500)");

    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
        "orders.amount >= 300 AND orders.id >= 100)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_FALSE(names.count("bob"));
}

} // namespace
