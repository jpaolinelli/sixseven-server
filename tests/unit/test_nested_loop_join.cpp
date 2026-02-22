#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/nested_loop_join.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_helpers.h"

using namespace giodb;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class NestedLoopJoinTest : public ::testing::Test {
protected:
    // -- Schema builders ------------------------------------------------------

    static OutputSchema left_schema() {
        return OutputSchema({
            {"left", "id", TypeId::INT32, false, 1},
            {"left", "name", TypeId::STRING, true, 1},
        });
    }

    static OutputSchema right_schema() {
        return OutputSchema({
            {"right", "id", TypeId::INT32, false, 2},
            {"right", "dept", TypeId::STRING, true, 2},
        });
    }

    static OutputSchema combined_schema() {
        return OutputSchema({
            {"left", "id", TypeId::INT32, false, 1},
            {"left", "name", TypeId::STRING, true, 1},
            {"right", "id", TypeId::INT32, false, 2},
            {"right", "dept", TypeId::STRING, true, 2},
        });
    }

    // For SEMI/ANTI, output is left-side only.
    static OutputSchema semi_anti_schema() { return left_schema(); }

    // -- Tuple builders -------------------------------------------------------

    static Tuple make_left(int32_t id, const std::string& name) {
        return Tuple{{Value(id), Value(name)}, {}};
    }

    static Tuple make_right(int32_t id, const std::string& dept) {
        return Tuple{{Value(id), Value(dept)}, {}};
    }

    // -- Standard test data ---------------------------------------------------

    static std::vector<Tuple> standard_left() {
        return {make_left(1, "alice"), make_left(2, "bob"), make_left(3, "charlie")};
    }

    static std::vector<Tuple> standard_right() {
        return {make_right(1, "eng"), make_right(2, "sales"), make_right(4, "hr")};
    }

    // -- Expression builders --------------------------------------------------

    static ExprPtr col_ref(const std::string& table, const std::string& column) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = table;
        e->column = column;
        return e;
    }

    static ExprPtr binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto e = std::make_unique<BinaryExpr>();
        e->op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    /// Standard join predicate: left.id = right.id
    static ExprPtr equi_predicate() {
        return binary_expr(BinaryOp::EQUAL, col_ref("left", "id"), col_ref("right", "id"));
    }

    // -- Helper to collect all output tuples ----------------------------------

    static std::vector<Tuple> collect_all(Iterator& iter) {
        std::vector<Tuple> result;
        auto open = iter.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        while (true) {
            auto row = iter.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            result.push_back(std::move(row->value()));
        }
        iter.close();
        return result;
    }
};

// ===========================================================================
// INNER JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, InnerJoinBasic) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::INNER,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // left.id=1 matches right.id=1, left.id=2 matches right.id=2
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[1].as_string(), "alice");
    EXPECT_EQ(rows[0].values[2].as_int32(), 1);
    EXPECT_EQ(rows[0].values[3].as_string(), "eng");
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[1].values[3].as_string(), "sales");
}

TEST_F(NestedLoopJoinTest, InnerJoinNoMatches) {
    auto left_data = std::vector<Tuple>{make_left(10, "x")};
    auto right_data = std::vector<Tuple>{make_right(20, "y")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::INNER,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(NestedLoopJoinTest, InnerJoinEmptyLeft) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), standard_right()),
        JoinType::INNER,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(NestedLoopJoinTest, InnerJoinEmptyRight) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left()),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::INNER,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(NestedLoopJoinTest, InnerJoinDuplicateKeys) {
    auto left_data =
        std::vector<Tuple>{make_left(1, "alice"), make_left(1, "alice2"), make_left(2, "bob")};
    auto right_data =
        std::vector<Tuple>{make_right(1, "eng"), make_right(1, "eng2"), make_right(3, "hr")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::INNER,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // alice x eng, alice x eng2, alice2 x eng, alice2 x eng2 = 4
    ASSERT_EQ(rows.size(), 4u);
}

// ===========================================================================
// LEFT JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, LeftJoinBasic) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::LEFT,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // alice matches eng, bob matches sales, charlie has no match → NULL right
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[3].as_string(), "eng");
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[1].values[3].as_string(), "sales");
    EXPECT_EQ(rows[2].values[0].as_int32(), 3);
    EXPECT_EQ(rows[2].values[1].as_string(), "charlie");
    EXPECT_TRUE(rows[2].values[2].is_null());
    EXPECT_TRUE(rows[2].values[3].is_null());
}

TEST_F(NestedLoopJoinTest, LeftJoinNoMatches) {
    auto left_data = std::vector<Tuple>{make_left(10, "x"), make_left(20, "y")};
    auto right_data = std::vector<Tuple>{make_right(30, "z")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::LEFT,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0].values[2].is_null());
    EXPECT_TRUE(rows[1].values[2].is_null());
}

TEST_F(NestedLoopJoinTest, LeftJoinEmptyRight) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left()),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::LEFT,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 3u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[2].is_null());
        EXPECT_TRUE(r.values[3].is_null());
    }
}

// ===========================================================================
// RIGHT JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, RightJoinBasic) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::RIGHT,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // alice matches eng, bob matches sales, hr(id=4) has no match → NULL left
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    // Unmatched right: hr
    EXPECT_TRUE(rows[2].values[0].is_null());
    EXPECT_TRUE(rows[2].values[1].is_null());
    EXPECT_EQ(rows[2].values[2].as_int32(), 4);
    EXPECT_EQ(rows[2].values[3].as_string(), "hr");
}

TEST_F(NestedLoopJoinTest, RightJoinEmptyLeft) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), standard_right()),
        JoinType::RIGHT,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 3u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[0].is_null());
        EXPECT_TRUE(r.values[1].is_null());
    }
}

// ===========================================================================
// FULL OUTER JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, FullJoinBasic) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::FULL,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // alice+eng, bob+sales, charlie+NULL(right), NULL(left)+hr = 4
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[2].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[1].values[2].as_int32(), 2);
    // charlie (unmatched left)
    EXPECT_EQ(rows[2].values[0].as_int32(), 3);
    EXPECT_TRUE(rows[2].values[2].is_null());
    // hr (unmatched right)
    EXPECT_TRUE(rows[3].values[0].is_null());
    EXPECT_EQ(rows[3].values[2].as_int32(), 4);
}

TEST_F(NestedLoopJoinTest, FullJoinBothEmpty) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::FULL,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// CROSS JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, CrossJoinBasic) {
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::CROSS,
                                nullptr,
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // 3 x 3 = 9
    ASSERT_EQ(rows.size(), 9u);
    // First row: alice x eng
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[2].as_int32(), 1);
}

TEST_F(NestedLoopJoinTest, CrossJoinEmptyRight) {
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left()),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::CROSS,
        nullptr,
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(NestedLoopJoinTest, CrossJoinEmptyLeft) {
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), standard_right()),
        JoinType::CROSS,
        nullptr,
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// SEMI JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, SemiJoinBasic) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::SEMI,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // alice(1) matches, bob(2) matches, charlie(3) doesn't
    ASSERT_EQ(rows.size(), 2u);
    // SEMI returns only left columns.
    EXPECT_EQ(rows[0].values.size(), 2u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[1].as_string(), "alice");
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[1].values[1].as_string(), "bob");
}

TEST_F(NestedLoopJoinTest, SemiJoinDuplicateRightMatches) {
    auto left_data = std::vector<Tuple>{make_left(1, "alice")};
    auto right_data =
        std::vector<Tuple>{make_right(1, "eng"), make_right(1, "eng2"), make_right(1, "eng3")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::SEMI,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // Should return alice only once despite 3 right matches.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
}

TEST_F(NestedLoopJoinTest, SemiJoinNoMatches) {
    auto left_data = std::vector<Tuple>{make_left(10, "x")};
    auto right_data = std::vector<Tuple>{make_right(20, "y")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::SEMI,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// ANTI JOIN tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, AntiJoinBasic) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::ANTI,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // alice(1) matches → excluded, bob(2) matches → excluded, charlie(3) no match → included
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 3);
    EXPECT_EQ(rows[0].values[1].as_string(), "charlie");
}

TEST_F(NestedLoopJoinTest, AntiJoinAllMatch) {
    auto left_data = std::vector<Tuple>{make_left(1, "alice"), make_left(2, "bob")};
    auto right_data = std::vector<Tuple>{make_right(1, "eng"), make_right(2, "sales")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::ANTI,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(NestedLoopJoinTest, AntiJoinNoneMatch) {
    auto left_data = std::vector<Tuple>{make_left(10, "x"), make_left(20, "y")};
    auto right_data = std::vector<Tuple>{make_right(30, "z")};

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::ANTI,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 10);
    EXPECT_EQ(rows[1].values[0].as_int32(), 20);
}

TEST_F(NestedLoopJoinTest, AntiJoinEmptyRight) {
    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left()),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::ANTI,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // All left tuples should be returned (no matches possible).
    ASSERT_EQ(rows.size(), 3u);
}

// ===========================================================================
// Output schema test
// ===========================================================================

TEST_F(NestedLoopJoinTest, OutputSchemaCorrect) {
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::INNER,
        nullptr,
        bound,
        combined_schema());

    EXPECT_EQ(join.output_schema().column_count(), 4u);
    EXPECT_EQ(join.output_schema().column(0).table_name, "left");
    EXPECT_EQ(join.output_schema().column(0).name, "id");
    EXPECT_EQ(join.output_schema().column(2).table_name, "right");
    EXPECT_EQ(join.output_schema().column(2).name, "id");
}

// ===========================================================================
// Non-equi join predicate test
// ===========================================================================

TEST_F(NestedLoopJoinTest, InnerJoinNonEquiPredicate) {
    // left.id < right.id
    auto pred = binary_expr(BinaryOp::LESS, col_ref("left", "id"), col_ref("right", "id"));
    BoundStatement bound;

    NestedLoopJoinOperator join(std::make_unique<VectorIterator>(left_schema(), standard_left()),
                                std::make_unique<VectorIterator>(right_schema(), standard_right()),
                                JoinType::INNER,
                                pred.get(),
                                bound,
                                combined_schema());

    auto rows = collect_all(join);
    // left.id=1 < right.id=2,4 → 2 matches
    // left.id=2 < right.id=4 → 1 match
    // left.id=3 < right.id=4 → 1 match
    ASSERT_EQ(rows.size(), 4u);
}

// ===========================================================================
// NULL join key tests
// ===========================================================================

TEST_F(NestedLoopJoinTest, InnerJoinNullKeyNeverMatches) {
    // NULL keys should never match anything, including other NULLs.
    auto left_data = std::vector<Tuple>{
        make_left(1, "alice"),
        Tuple{{Value::make_null(), Value(std::string("nulluser"))}, {}},
    };
    auto right_data = std::vector<Tuple>{
        make_right(1, "eng"),
        Tuple{{Value::make_null(), Value(std::string("nulldept"))}, {}},
    };

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::INNER,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // Only alice(1) matches eng(1). NULL keys don't match.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
}

TEST_F(NestedLoopJoinTest, LeftJoinNullKeyProducesNullRight) {
    auto left_data = std::vector<Tuple>{
        make_left(1, "alice"),
        Tuple{{Value::make_null(), Value(std::string("nulluser"))}, {}},
    };
    auto right_data = std::vector<Tuple>{
        make_right(1, "eng"),
    };

    auto pred = equi_predicate();
    BoundStatement bound;

    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::LEFT,
        pred.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // alice matches eng; nulluser has no match → NULL right side.
    ASSERT_EQ(rows.size(), 2u);
    // Find each row by checking for null key.
    bool found_alice = false;
    bool found_null_user = false;
    for (auto& r : rows) {
        if (!r.values[0].is_null() && r.values[0].as_int32() == 1) {
            EXPECT_EQ(r.values[1].as_string(), "alice");
            EXPECT_EQ(r.values[3].as_string(), "eng");
            found_alice = true;
        }
        if (r.values[0].is_null()) {
            EXPECT_EQ(r.values[1].as_string(), "nulluser");
            EXPECT_TRUE(r.values[2].is_null());
            EXPECT_TRUE(r.values[3].is_null());
            found_null_user = true;
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_null_user);
}
