#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/sort_merge_join.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SortMergeJoinTest : public ::testing::Test {
protected:
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

    static Tuple make_left(int32_t id, const std::string& name) {
        return Tuple{{Value(id), Value(name)}, {}};
    }

    static Tuple make_right(int32_t id, const std::string& dept) {
        return Tuple{{Value(id), Value(dept)}, {}};
    }

    // Pre-sorted standard data.
    static std::vector<Tuple> standard_left_sorted() {
        return {make_left(1, "alice"), make_left(2, "bob"), make_left(3, "charlie")};
    }

    static std::vector<Tuple> standard_right_sorted() {
        return {make_right(1, "eng"), make_right(2, "sales"), make_right(4, "hr")};
    }

    // Unsorted standard data.
    static std::vector<Tuple> standard_left_unsorted() {
        return {make_left(3, "charlie"), make_left(1, "alice"), make_left(2, "bob")};
    }

    static std::vector<Tuple> standard_right_unsorted() {
        return {make_right(4, "hr"), make_right(1, "eng"), make_right(2, "sales")};
    }

    static ExprPtr col_ref(const std::string& table, const std::string& column) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = table;
        e->column = column;
        return e;
    }

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

TEST_F(SortMergeJoinTest, InnerJoinSorted) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left_sorted()),
        std::make_unique<VectorIterator>(right_schema(), standard_right_sorted()),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[3].as_string(), "eng");
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[1].values[3].as_string(), "sales");
}

TEST_F(SortMergeJoinTest, InnerJoinUnsorted) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left_unsorted()),
        std::make_unique<VectorIterator>(right_schema(), standard_right_unsorted()),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // Auto-sorts both sides, should still find 2 matches.
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
}

TEST_F(SortMergeJoinTest, InnerJoinNoMatches) {
    auto left_data = std::vector<Tuple>{make_left(10, "x")};
    auto right_data = std::vector<Tuple>{make_right(20, "y")};

    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(SortMergeJoinTest, InnerJoinDuplicates) {
    auto left_data =
        std::vector<Tuple>{make_left(1, "alice"), make_left(1, "alice2"), make_left(2, "bob")};
    auto right_data =
        std::vector<Tuple>{make_right(1, "eng"), make_right(1, "eng2"), make_right(3, "hr")};

    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // alice x eng, alice x eng2, alice2 x eng, alice2 x eng2 = 4
    ASSERT_EQ(rows.size(), 4u);
}

TEST_F(SortMergeJoinTest, InnerJoinEmptyInputs) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), standard_right_sorted()),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// LEFT JOIN tests
// ===========================================================================

TEST_F(SortMergeJoinTest, LeftJoinBasic) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left_sorted()),
        std::make_unique<VectorIterator>(right_schema(), standard_right_sorted()),
        JoinType::LEFT,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[3].as_string(), "eng");
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[1].values[3].as_string(), "sales");
    // charlie has no match.
    EXPECT_EQ(rows[2].values[0].as_int32(), 3);
    EXPECT_TRUE(rows[2].values[2].is_null());
    EXPECT_TRUE(rows[2].values[3].is_null());
}

TEST_F(SortMergeJoinTest, LeftJoinEmptyRight) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left_sorted()),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::LEFT,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 3u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[2].is_null());
    }
}

// ===========================================================================
// RIGHT JOIN tests
// ===========================================================================

TEST_F(SortMergeJoinTest, RightJoinBasic) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left_sorted()),
        std::make_unique<VectorIterator>(right_schema(), standard_right_sorted()),
        JoinType::RIGHT,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    // hr(id=4) has no left match.
    EXPECT_TRUE(rows[2].values[0].is_null());
    EXPECT_EQ(rows[2].values[2].as_int32(), 4);
    EXPECT_EQ(rows[2].values[3].as_string(), "hr");
}

// ===========================================================================
// FULL OUTER JOIN tests
// ===========================================================================

TEST_F(SortMergeJoinTest, FullJoinBasic) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), standard_left_sorted()),
        std::make_unique<VectorIterator>(right_schema(), standard_right_sorted()),
        JoinType::FULL,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // alice+eng, bob+sales, charlie+NULL, NULL+hr = 4
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 2);
    EXPECT_EQ(rows[2].values[0].as_int32(), 3);
    EXPECT_TRUE(rows[2].values[2].is_null());
    EXPECT_TRUE(rows[3].values[0].is_null());
    EXPECT_EQ(rows[3].values[2].as_int32(), 4);
}

TEST_F(SortMergeJoinTest, FullJoinBothEmpty) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::FULL,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// Output schema test
// ===========================================================================

// ===========================================================================
// NULL join key tests
// ===========================================================================

TEST_F(SortMergeJoinTest, InnerJoinNullKeyNeverMatches) {
    auto left_data = std::vector<Tuple>{
        make_left(1, "alice"),
        Tuple{{Value::make_null(), Value(std::string("nulluser"))}, {}},
    };
    auto right_data = std::vector<Tuple>{
        make_right(1, "eng"),
        Tuple{{Value::make_null(), Value(std::string("nulldept"))}, {}},
    };

    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // Only alice(1) matches eng(1). NULL keys don't match.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
}

TEST_F(SortMergeJoinTest, LeftJoinNullKeyProducesNullRight) {
    auto left_data = std::vector<Tuple>{
        make_left(1, "alice"),
        Tuple{{Value::make_null(), Value(std::string("nulluser"))}, {}},
    };
    auto right_data = std::vector<Tuple>{
        make_right(1, "eng"),
    };

    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::move(left_data)),
        std::make_unique<VectorIterator>(right_schema(), std::move(right_data)),
        JoinType::LEFT,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    auto rows = collect_all(join);
    // alice matches eng; nulluser has no match → NULL right side.
    ASSERT_EQ(rows.size(), 2u);
    // Sort order: NULL sorts before 1, so nulluser comes first.
    // Check that the null-key row has NULL right side.
    bool found_null_left_key = false;
    for (auto& r : rows) {
        if (r.values[0].is_null()) {
            EXPECT_TRUE(r.values[2].is_null());
            EXPECT_TRUE(r.values[3].is_null());
            found_null_left_key = true;
        }
    }
    EXPECT_TRUE(found_null_left_key);
}

// ===========================================================================
// Output schema test
// ===========================================================================

TEST_F(SortMergeJoinTest, OutputSchemaCorrect) {
    auto left_key = col_ref("left", "id");
    auto right_key = col_ref("right", "id");
    BoundStatement bound;

    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(left_schema(), std::vector<Tuple>{}),
        std::make_unique<VectorIterator>(right_schema(), std::vector<Tuple>{}),
        JoinType::INNER,
        left_key.get(),
        right_key.get(),
        bound,
        combined_schema());

    EXPECT_EQ(join.output_schema().column_count(), 4u);
    EXPECT_EQ(join.output_schema().column(0).table_name, "left");
    EXPECT_EQ(join.output_schema().column(2).table_name, "right");
}
