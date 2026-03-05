#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/hash_join.h"
#include "sixseven/executor/nested_loop_join.h"
#include "sixseven/executor/sort_merge_join.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "test_helpers.h"

using namespace sixseven;

// ===========================================================================
// Shared helpers
// ===========================================================================

static OutputSchema make_left_schema() {
    return OutputSchema({
        {"left", "id", TypeId::INT32, true, 1},
        {"left", "name", TypeId::STRING, true, 1},
    });
}

static OutputSchema make_right_schema() {
    return OutputSchema({
        {"right", "id", TypeId::INT32, true, 2},
        {"right", "dept", TypeId::STRING, true, 2},
    });
}

static OutputSchema make_combined_schema() {
    return OutputSchema({
        {"left", "id", TypeId::INT32, true, 1},
        {"left", "name", TypeId::STRING, true, 1},
        {"right", "id", TypeId::INT32, true, 2},
        {"right", "dept", TypeId::STRING, true, 2},
    });
}

static Tuple left_tuple(int32_t id, const std::string& name) {
    return Tuple{{Value(id), Value(name)}, {}};
}

static Tuple right_tuple(int32_t id, const std::string& dept) {
    return Tuple{{Value(id), Value(dept)}, {}};
}

static Tuple left_null_key(const std::string& name) {
    return Tuple{{Value::make_null(), Value(name)}, {}};
}

static Tuple right_null_key(const std::string& dept) {
    return Tuple{{Value::make_null(), Value(dept)}, {}};
}

static ExprPtr col_ref(const std::string& table, const std::string& column) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->table = table;
    e->column = column;
    return e;
}

static ExprPtr eq_pred() {
    auto e = std::make_unique<BinaryExpr>();
    e->op = BinaryOp::EQUAL;
    e->lhs = col_ref("left", "id");
    e->rhs = col_ref("right", "id");
    return e;
}

static std::vector<Tuple>
run_nlj(std::vector<Tuple> left, std::vector<Tuple> right, JoinType type, const Expr* pred) {
    BoundStatement bound;
    NestedLoopJoinOperator join(
        std::make_unique<VectorIterator>(make_left_schema(), std::move(left)),
        std::make_unique<VectorIterator>(make_right_schema(), std::move(right)),
        type,
        pred,
        bound,
        make_combined_schema());
    return collect_all(join);
}

// ===========================================================================
// QA_NestedLoopJoin — Adversarial tests
// ===========================================================================

TEST(QA_NestedLoopJoin, FullJoinEmptyLeft) {
    auto pred = eq_pred();
    auto right = std::vector<Tuple>{right_tuple(1, "eng"), right_tuple(2, "sales")};

    auto rows = run_nlj({}, std::move(right), JoinType::FULL, pred.get());

    // All right tuples should appear with NULL left side.
    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[0].is_null());
        EXPECT_TRUE(r.values[1].is_null());
        EXPECT_FALSE(r.values[2].is_null());
    }
}

TEST(QA_NestedLoopJoin, FullJoinEmptyRight) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_tuple(1, "alice"), left_tuple(2, "bob")};

    auto rows = run_nlj(std::move(left), {}, JoinType::FULL, pred.get());

    // All left tuples should appear with NULL right side.
    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_FALSE(r.values[0].is_null());
        EXPECT_TRUE(r.values[2].is_null());
        EXPECT_TRUE(r.values[3].is_null());
    }
}

TEST(QA_NestedLoopJoin, FullJoinNoMatches) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_tuple(1, "alice"), left_tuple(2, "bob")};
    auto right = std::vector<Tuple>{right_tuple(10, "eng"), right_tuple(20, "sales")};

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::FULL, pred.get());

    // 2 unmatched left + 2 unmatched right = 4
    ASSERT_EQ(rows.size(), 4u);

    // First two: left with NULL right.
    EXPECT_FALSE(rows[0].values[0].is_null());
    EXPECT_TRUE(rows[0].values[2].is_null());
    EXPECT_FALSE(rows[1].values[0].is_null());
    EXPECT_TRUE(rows[1].values[2].is_null());

    // Last two: NULL left with right.
    EXPECT_TRUE(rows[2].values[0].is_null());
    EXPECT_FALSE(rows[2].values[2].is_null());
    EXPECT_TRUE(rows[3].values[0].is_null());
    EXPECT_FALSE(rows[3].values[2].is_null());
}

TEST(QA_NestedLoopJoin, FullJoinMultipleNullKeys) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{
        left_tuple(1, "alice"),
        left_null_key("null1"),
        left_null_key("null2"),
    };
    auto right = std::vector<Tuple>{
        right_tuple(1, "eng"),
        right_null_key("nulldept1"),
        right_null_key("nulldept2"),
    };

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::FULL, pred.get());

    // alice matches eng = 1 match.
    // null1, null2 are unmatched left = 2 rows with NULL right.
    // nulldept1, nulldept2 are unmatched right = 2 rows with NULL left.
    // Total = 5.
    ASSERT_EQ(rows.size(), 5u);
}

TEST(QA_NestedLoopJoin, SemiJoinEmptyRight) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_tuple(1, "alice"), left_tuple(2, "bob")};

    auto rows = run_nlj(std::move(left), {}, JoinType::SEMI, pred.get());

    // No right tuples, so no matches possible.
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_NestedLoopJoin, SemiJoinEmptyLeft) {
    auto pred = eq_pred();
    auto right = std::vector<Tuple>{right_tuple(1, "eng")};

    auto rows = run_nlj({}, std::move(right), JoinType::SEMI, pred.get());

    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_NestedLoopJoin, SemiJoinNullKeysNeverMatch) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_null_key("null1"), left_null_key("null2")};
    auto right = std::vector<Tuple>{right_null_key("nulld1"), right_null_key("nulld2")};

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::SEMI, pred.get());

    // NULL keys never match, so SEMI should return 0.
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_NestedLoopJoin, AntiJoinNullKeysReturnAll) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_null_key("null1"), left_null_key("null2")};
    auto right = std::vector<Tuple>{right_null_key("nulld1")};

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::ANTI, pred.get());

    // NULL keys never match, so ANTI should return all left tuples.
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0].values[0].is_null());
    EXPECT_TRUE(rows[1].values[0].is_null());
}

TEST(QA_NestedLoopJoin, AntiJoinEmptyLeft) {
    auto pred = eq_pred();
    auto right = std::vector<Tuple>{right_tuple(1, "eng")};

    auto rows = run_nlj({}, std::move(right), JoinType::ANTI, pred.get());

    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_NestedLoopJoin, CrossJoinSingleRowEach) {
    auto left = std::vector<Tuple>{left_tuple(1, "alice")};
    auto right = std::vector<Tuple>{right_tuple(2, "eng")};

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::CROSS, nullptr);

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[2].as_int32(), 2);
}

TEST(QA_NestedLoopJoin, CrossJoinStressTest) {
    std::vector<Tuple> left;
    std::vector<Tuple> right;
    for (int32_t i = 0; i < 50; ++i) {
        left.push_back(left_tuple(i, "l" + std::to_string(i)));
        right.push_back(right_tuple(i, "r" + std::to_string(i)));
    }

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::CROSS, nullptr);

    // 50 x 50 = 2500
    EXPECT_EQ(rows.size(), 2500u);
}

TEST(QA_NestedLoopJoin, RightJoinDuplicateRightKeys) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_tuple(1, "alice")};
    auto right = std::vector<Tuple>{
        right_tuple(1, "eng1"),
        right_tuple(1, "eng2"),
        right_tuple(1, "eng3"),
        right_tuple(2, "sales"),
    };

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::RIGHT, pred.get());

    // alice matches 3 eng rows + sales unmatched = 4
    ASSERT_EQ(rows.size(), 4u);

    // 3 matched rows.
    size_t matched = 0;
    size_t unmatched = 0;
    for (auto& r : rows) {
        if (!r.values[0].is_null()) {
            EXPECT_EQ(r.values[0].as_int32(), 1);
            ++matched;
        } else {
            EXPECT_EQ(r.values[2].as_int32(), 2);
            ++unmatched;
        }
    }
    EXPECT_EQ(matched, 3u);
    EXPECT_EQ(unmatched, 1u);
}

TEST(QA_NestedLoopJoin, InnerJoinLargeDataset) {
    std::vector<Tuple> left;
    std::vector<Tuple> right;
    for (int32_t i = 0; i < 500; ++i) {
        left.push_back(left_tuple(i, "l" + std::to_string(i)));
        right.push_back(right_tuple(i, "r" + std::to_string(i)));
    }

    auto pred = eq_pred();
    auto rows = run_nlj(std::move(left), std::move(right), JoinType::INNER, pred.get());

    // All 500 match.
    EXPECT_EQ(rows.size(), 500u);
}

TEST(QA_NestedLoopJoin, LeftJoinAllMatchWithDuplicates) {
    auto pred = eq_pred();
    auto left = std::vector<Tuple>{left_tuple(1, "a"), left_tuple(1, "b"), left_tuple(2, "c")};
    auto right = std::vector<Tuple>{right_tuple(1, "e"), right_tuple(2, "s")};

    auto rows = run_nlj(std::move(left), std::move(right), JoinType::LEFT, pred.get());

    // a matches e, b matches e, c matches s = 3 (all matched, no NULL padding needed).
    ASSERT_EQ(rows.size(), 3u);
    for (auto& r : rows) {
        EXPECT_FALSE(r.values[2].is_null());
    }
}

// ===========================================================================
// QA_HashJoin — Adversarial tests
// ===========================================================================

static ExprPtr probe_key_expr() {
    return col_ref("probe", "id");
}
static ExprPtr build_key_expr() {
    return col_ref("build", "id");
}

static OutputSchema make_probe_schema() {
    return OutputSchema({
        {"probe", "id", TypeId::INT32, true, 1},
        {"probe", "name", TypeId::STRING, true, 1},
    });
}

static OutputSchema make_build_schema() {
    return OutputSchema({
        {"build", "id", TypeId::INT32, true, 2},
        {"build", "dept", TypeId::STRING, true, 2},
    });
}

static OutputSchema make_hj_combined_schema() {
    return OutputSchema({
        {"probe", "id", TypeId::INT32, true, 1},
        {"probe", "name", TypeId::STRING, true, 1},
        {"build", "id", TypeId::INT32, true, 2},
        {"build", "dept", TypeId::STRING, true, 2},
    });
}

static Tuple probe_tuple(int32_t id, const std::string& name) {
    return Tuple{{Value(id), Value(name)}, {}};
}

static Tuple build_tuple(int32_t id, const std::string& dept) {
    return Tuple{{Value(id), Value(dept)}, {}};
}

static Tuple probe_null_key(const std::string& name) {
    return Tuple{{Value::make_null(), Value(name)}, {}};
}

static Tuple build_null_key(const std::string& dept) {
    return Tuple{{Value::make_null(), Value(dept)}, {}};
}

static std::vector<Tuple> run_hash_join(std::vector<Tuple> probe,
                                        std::vector<Tuple> build,
                                        JoinType type,
                                        size_t work_mem = 0) {
    auto pk = probe_key_expr();
    auto bk = build_key_expr();
    BoundStatement bound;
    HashJoinOperator join(std::make_unique<VectorIterator>(make_probe_schema(), std::move(probe)),
                          std::make_unique<VectorIterator>(make_build_schema(), std::move(build)),
                          type,
                          pk.get(),
                          bk.get(),
                          bound,
                          make_hj_combined_schema(),
                          work_mem);
    return collect_all(join);
}

TEST(QA_HashJoin, BothSidesEmpty) {
    auto rows = run_hash_join({}, {}, JoinType::INNER);
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_HashJoin, BothSidesEmptyLeftJoin) {
    auto rows = run_hash_join({}, {}, JoinType::LEFT);
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_HashJoin, EmptyProbeSide) {
    auto build = std::vector<Tuple>{build_tuple(1, "eng")};
    auto rows = run_hash_join({}, std::move(build), JoinType::INNER);
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_HashJoin, EmptyProbeSideLeftJoin) {
    auto build = std::vector<Tuple>{build_tuple(1, "eng")};
    auto rows = run_hash_join({}, std::move(build), JoinType::LEFT);
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_HashJoin, AllSameKey) {
    std::vector<Tuple> probe;
    std::vector<Tuple> build;
    for (int32_t i = 0; i < 5; ++i) {
        probe.push_back(probe_tuple(1, "p" + std::to_string(i)));
        build.push_back(build_tuple(1, "b" + std::to_string(i)));
    }

    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER);

    // 5 x 5 = 25
    EXPECT_EQ(rows.size(), 25u);
}

TEST(QA_HashJoin, NullKeyOnBuildSide) {
    auto probe = std::vector<Tuple>{probe_tuple(1, "alice")};
    auto build = std::vector<Tuple>{build_null_key("nulldept"), build_tuple(1, "eng")};

    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER);

    // Only alice matches eng. NULL build key should not match.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[3].as_string(), "eng");
}

TEST(QA_HashJoin, NullKeysOnBothSides) {
    auto probe = std::vector<Tuple>{probe_null_key("null_p"), probe_tuple(1, "alice")};
    auto build = std::vector<Tuple>{build_null_key("null_b"), build_tuple(1, "eng")};

    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER);

    // Only alice matches eng. NULL keys never match.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
}

TEST(QA_HashJoin, LeftJoinNullKeysOnBothSides) {
    auto probe = std::vector<Tuple>{probe_null_key("null_p"), probe_tuple(1, "alice")};
    auto build = std::vector<Tuple>{build_null_key("null_b"), build_tuple(1, "eng")};

    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::LEFT);

    // alice matches eng. NULL probe key gets NULL build side.
    ASSERT_EQ(rows.size(), 2u);

    bool found_null_probe = false;
    bool found_alice = false;
    for (auto& r : rows) {
        if (r.values[0].is_null()) {
            EXPECT_TRUE(r.values[2].is_null());
            found_null_probe = true;
        } else {
            EXPECT_EQ(r.values[0].as_int32(), 1);
            EXPECT_EQ(r.values[3].as_string(), "eng");
            found_alice = true;
        }
    }
    EXPECT_TRUE(found_null_probe);
    EXPECT_TRUE(found_alice);
}

TEST(QA_HashJoin, WorkMemExactlyAtBuildSize) {
    auto probe = std::vector<Tuple>{probe_tuple(1, "alice"), probe_tuple(2, "bob")};
    auto build =
        std::vector<Tuple>{build_tuple(1, "eng"), build_tuple(2, "sales"), build_tuple(3, "hr")};

    // work_mem=3 means build (3 tuples) does NOT exceed work_mem. In-memory path used.
    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER, 3);

    ASSERT_EQ(rows.size(), 2u);
}

TEST(QA_HashJoin, GraceHashJoinNullKeys) {
    auto probe = std::vector<Tuple>{
        probe_null_key("null_p"), probe_tuple(1, "alice"), probe_tuple(2, "bob")};
    auto build = std::vector<Tuple>{build_null_key("null_b"), build_tuple(1, "eng")};

    // work_mem=1 forces grace hash join.
    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::LEFT, 1);

    // alice matches eng. bob gets NULL build. null_p gets NULL build.
    ASSERT_EQ(rows.size(), 3u);

    size_t matched = 0;
    size_t null_build = 0;
    for (auto& r : rows) {
        if (!r.values[2].is_null()) {
            ++matched;
        } else {
            ++null_build;
        }
    }
    EXPECT_EQ(matched, 1u);
    EXPECT_EQ(null_build, 2u);
}

TEST(QA_HashJoin, GraceHashJoinAllUnmatched) {
    std::vector<Tuple> probe;
    for (int32_t i = 100; i < 110; ++i) {
        probe.push_back(probe_tuple(i, "p" + std::to_string(i)));
    }
    std::vector<Tuple> build;
    for (int32_t i = 0; i < 5; ++i) {
        build.push_back(build_tuple(i, "b" + std::to_string(i)));
    }

    // work_mem=1 forces grace. LEFT join: all probe tuples should get NULL build.
    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::LEFT, 1);

    ASSERT_EQ(rows.size(), 10u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[2].is_null());
        EXPECT_TRUE(r.values[3].is_null());
    }
}

TEST(QA_HashJoin, GraceHashJoinLargeDuplicates) {
    std::vector<Tuple> probe;
    std::vector<Tuple> build;
    // 20 probe with key=1, 10 build with key=1.
    for (int32_t i = 0; i < 20; ++i) {
        probe.push_back(probe_tuple(1, "p" + std::to_string(i)));
    }
    for (int32_t i = 0; i < 10; ++i) {
        build.push_back(build_tuple(1, "b" + std::to_string(i)));
    }

    // work_mem=2 forces grace.
    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER, 2);

    // 20 x 10 = 200
    EXPECT_EQ(rows.size(), 200u);
}

TEST(QA_HashJoin, InnerJoinLargeDataset) {
    std::vector<Tuple> probe;
    std::vector<Tuple> build;
    for (int32_t i = 0; i < 500; ++i) {
        probe.push_back(probe_tuple(i, "p" + std::to_string(i)));
        build.push_back(build_tuple(i, "b" + std::to_string(i)));
    }

    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER);

    EXPECT_EQ(rows.size(), 500u);
}

TEST(QA_HashJoin, GraceHashJoinLargeDataset) {
    std::vector<Tuple> probe;
    std::vector<Tuple> build;
    for (int32_t i = 0; i < 500; ++i) {
        probe.push_back(probe_tuple(i, "p" + std::to_string(i)));
        build.push_back(build_tuple(i, "b" + std::to_string(i)));
    }

    // work_mem=10 forces grace with many partitions.
    auto rows = run_hash_join(std::move(probe), std::move(build), JoinType::INNER, 10);

    EXPECT_EQ(rows.size(), 500u);
}

// ===========================================================================
// QA_SortMergeJoin — Adversarial tests
// ===========================================================================

static std::vector<Tuple>
run_smj(std::vector<Tuple> left, std::vector<Tuple> right, JoinType type) {
    auto lk = col_ref("left", "id");
    auto rk = col_ref("right", "id");
    BoundStatement bound;
    SortMergeJoinOperator join(
        std::make_unique<VectorIterator>(make_left_schema(), std::move(left)),
        std::make_unique<VectorIterator>(make_right_schema(), std::move(right)),
        type,
        lk.get(),
        rk.get(),
        bound,
        make_combined_schema());
    return collect_all(join);
}

TEST(QA_SortMergeJoin, AllSameKeyInner) {
    std::vector<Tuple> left;
    std::vector<Tuple> right;
    for (int32_t i = 0; i < 5; ++i) {
        left.push_back(left_tuple(1, "l" + std::to_string(i)));
        right.push_back(right_tuple(1, "r" + std::to_string(i)));
    }

    auto rows = run_smj(std::move(left), std::move(right), JoinType::INNER);

    // Cross product: 5 x 5 = 25.
    EXPECT_EQ(rows.size(), 25u);
}

TEST(QA_SortMergeJoin, ReverseSortedInput) {
    auto left = std::vector<Tuple>{
        left_tuple(5, "e"),
        left_tuple(4, "d"),
        left_tuple(3, "c"),
        left_tuple(2, "b"),
        left_tuple(1, "a"),
    };
    auto right = std::vector<Tuple>{
        right_tuple(5, "E"),
        right_tuple(3, "C"),
        right_tuple(1, "A"),
    };

    auto rows = run_smj(std::move(left), std::move(right), JoinType::INNER);

    // Matches: 1-A, 3-C, 5-E = 3.
    ASSERT_EQ(rows.size(), 3u);
    // After sorting, output should be in sorted key order.
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[1].values[0].as_int32(), 3);
    EXPECT_EQ(rows[2].values[0].as_int32(), 5);
}

TEST(QA_SortMergeJoin, AllNullsBothSidesInner) {
    auto left = std::vector<Tuple>{left_null_key("n1"), left_null_key("n2")};
    auto right = std::vector<Tuple>{right_null_key("n3"), right_null_key("n4")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::INNER);

    // NULLs never match. INNER returns 0.
    EXPECT_EQ(rows.size(), 0u);
}

TEST(QA_SortMergeJoin, AllNullsBothSidesFull) {
    auto left = std::vector<Tuple>{left_null_key("n1"), left_null_key("n2")};
    auto right = std::vector<Tuple>{right_null_key("n3"), right_null_key("n4")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::FULL);

    // NULLs never match. FULL: 2 unmatched left + 2 unmatched right = 4.
    ASSERT_EQ(rows.size(), 4u);

    // First two: left with NULL right.
    size_t null_right = 0;
    size_t null_left = 0;
    for (auto& r : rows) {
        if (!r.values[1].is_null() && r.values[2].is_null()) {
            ++null_right;
        }
        if (r.values[0].is_null() && !r.values[3].is_null()) {
            ++null_left;
        }
    }
    EXPECT_EQ(null_right, 2u);
    EXPECT_EQ(null_left, 2u);
}

TEST(QA_SortMergeJoin, RightSideAllNullsLeftJoin) {
    auto left = std::vector<Tuple>{left_tuple(1, "alice"), left_tuple(2, "bob")};
    auto right = std::vector<Tuple>{right_null_key("n1"), right_null_key("n2")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::LEFT);

    // NULLs never match. All left tuples appear with NULL right.
    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[2].is_null());
        EXPECT_TRUE(r.values[3].is_null());
    }
}

TEST(QA_SortMergeJoin, LeftSideAllNullsRightJoin) {
    auto left = std::vector<Tuple>{left_null_key("n1"), left_null_key("n2")};
    auto right = std::vector<Tuple>{right_tuple(1, "eng"), right_tuple(2, "sales")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::RIGHT);

    // NULLs never match. All right tuples appear with NULL left.
    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[0].is_null());
        EXPECT_TRUE(r.values[1].is_null());
    }
}

TEST(QA_SortMergeJoin, RightJoinEmptyLeft) {
    auto right = std::vector<Tuple>{right_tuple(1, "eng"), right_tuple(2, "sales")};

    auto rows = run_smj({}, std::move(right), JoinType::RIGHT);

    // All right appear with NULL left.
    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[0].is_null());
        EXPECT_FALSE(r.values[2].is_null());
    }
}

TEST(QA_SortMergeJoin, FullJoinEmptyLeft) {
    auto right = std::vector<Tuple>{right_tuple(1, "eng"), right_tuple(2, "sales")};

    auto rows = run_smj({}, std::move(right), JoinType::FULL);

    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_TRUE(r.values[0].is_null());
        EXPECT_FALSE(r.values[2].is_null());
    }
}

TEST(QA_SortMergeJoin, FullJoinEmptyRight) {
    auto left = std::vector<Tuple>{left_tuple(1, "alice"), left_tuple(2, "bob")};

    auto rows = run_smj(std::move(left), {}, JoinType::FULL);

    ASSERT_EQ(rows.size(), 2u);
    for (auto& r : rows) {
        EXPECT_FALSE(r.values[0].is_null());
        EXPECT_TRUE(r.values[2].is_null());
    }
}

TEST(QA_SortMergeJoin, SingleElementEachSideMatching) {
    auto left = std::vector<Tuple>{left_tuple(1, "alice")};
    auto right = std::vector<Tuple>{right_tuple(1, "eng")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::INNER);

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_EQ(rows[0].values[2].as_int32(), 1);
}

TEST(QA_SortMergeJoin, SingleElementEachSideNotMatchingFull) {
    auto left = std::vector<Tuple>{left_tuple(1, "alice")};
    auto right = std::vector<Tuple>{right_tuple(2, "eng")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::FULL);

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
    EXPECT_TRUE(rows[0].values[2].is_null());
    EXPECT_TRUE(rows[1].values[0].is_null());
    EXPECT_EQ(rows[1].values[2].as_int32(), 2);
}

TEST(QA_SortMergeJoin, LargeDatasetStressTest) {
    std::vector<Tuple> left;
    std::vector<Tuple> right;
    for (int32_t i = 0; i < 500; ++i) {
        left.push_back(left_tuple(i, "l" + std::to_string(i)));
        right.push_back(right_tuple(i, "r" + std::to_string(i)));
    }

    auto rows = run_smj(std::move(left), std::move(right), JoinType::INNER);

    EXPECT_EQ(rows.size(), 500u);
}

TEST(QA_SortMergeJoin, LargeDuplicateGroups) {
    std::vector<Tuple> left;
    std::vector<Tuple> right;
    // 10 left with key=1, 10 right with key=1.
    for (int32_t i = 0; i < 10; ++i) {
        left.push_back(left_tuple(1, "l" + std::to_string(i)));
        right.push_back(right_tuple(1, "r" + std::to_string(i)));
    }

    auto rows = run_smj(std::move(left), std::move(right), JoinType::INNER);

    // Cross product: 10 x 10 = 100.
    EXPECT_EQ(rows.size(), 100u);
}

TEST(QA_SortMergeJoin, InterspersedNullsAndValuesFull) {
    auto left = std::vector<Tuple>{
        left_tuple(1, "alice"),
        left_null_key("null1"),
        left_tuple(3, "charlie"),
        left_null_key("null2"),
    };
    auto right = std::vector<Tuple>{
        right_null_key("nulld"),
        right_tuple(1, "eng"),
        right_tuple(5, "hr"),
    };

    auto rows = run_smj(std::move(left), std::move(right), JoinType::FULL);

    // After sorting by key (NULLs last):
    // Left: [1, 3, NULL, NULL], Right: [1, 5, NULL]
    // Matches: (1,1)
    // Unmatched left: 3, NULL, NULL = 3
    // Unmatched right: 5, NULL = 2
    // Total: 1 + 3 + 2 = 6
    ASSERT_EQ(rows.size(), 6u);
}

TEST(QA_SortMergeJoin, LeftJoinLargeUnmatched) {
    std::vector<Tuple> left;
    for (int32_t i = 0; i < 100; ++i) {
        left.push_back(left_tuple(i, "l" + std::to_string(i)));
    }
    // Only a few right tuples match.
    auto right = std::vector<Tuple>{right_tuple(0, "r0"), right_tuple(99, "r99")};

    auto rows = run_smj(std::move(left), std::move(right), JoinType::LEFT);

    // 100 left tuples, 2 match, 98 unmatched.
    ASSERT_EQ(rows.size(), 100u);

    size_t matched = 0;
    size_t null_right = 0;
    for (auto& r : rows) {
        if (r.values[2].is_null()) {
            ++null_right;
        } else {
            ++matched;
        }
    }
    EXPECT_EQ(matched, 2u);
    EXPECT_EQ(null_right, 98u);
}

// ===========================================================================
// Cross-operator consistency tests
// ===========================================================================

TEST(QA_JoinConsistency, InnerJoinAllThreeOperatorsAgree) {
    auto left = std::vector<Tuple>{
        left_tuple(1, "alice"),
        left_tuple(2, "bob"),
        left_tuple(3, "charlie"),
        left_tuple(5, "eve"),
    };
    auto right = std::vector<Tuple>{
        right_tuple(2, "sales"),
        right_tuple(3, "eng"),
        right_tuple(4, "hr"),
        right_tuple(5, "legal"),
    };

    auto pred = eq_pred();

    // NLJ
    auto nlj_rows =
        run_nlj(std::vector<Tuple>(left), std::vector<Tuple>(right), JoinType::INNER, pred.get());

    // Hash Join
    // Rename for hash join schema.
    auto probe_data = std::vector<Tuple>{
        probe_tuple(1, "alice"),
        probe_tuple(2, "bob"),
        probe_tuple(3, "charlie"),
        probe_tuple(5, "eve"),
    };
    auto build_data = std::vector<Tuple>{
        build_tuple(2, "sales"),
        build_tuple(3, "eng"),
        build_tuple(4, "hr"),
        build_tuple(5, "legal"),
    };
    auto hj_rows = run_hash_join(std::move(probe_data), std::move(build_data), JoinType::INNER);

    // SMJ
    auto smj_rows = run_smj(std::vector<Tuple>(left), std::vector<Tuple>(right), JoinType::INNER);

    // All three should produce 3 matches (keys 2, 3, 5).
    EXPECT_EQ(nlj_rows.size(), 3u);
    EXPECT_EQ(hj_rows.size(), 3u);
    EXPECT_EQ(smj_rows.size(), 3u);

    // Verify the same keys are matched.
    std::set<int32_t> nlj_keys;
    std::set<int32_t> hj_keys;
    std::set<int32_t> smj_keys;
    for (auto& r : nlj_rows) {
        nlj_keys.insert(r.values[0].as_int32());
    }
    for (auto& r : hj_rows) {
        hj_keys.insert(r.values[0].as_int32());
    }
    for (auto& r : smj_rows) {
        smj_keys.insert(r.values[0].as_int32());
    }
    EXPECT_EQ(nlj_keys, hj_keys);
    EXPECT_EQ(nlj_keys, smj_keys);
}

TEST(QA_JoinConsistency, LeftJoinNljAndSmjAgree) {
    auto left = std::vector<Tuple>{
        left_tuple(1, "alice"),
        left_tuple(2, "bob"),
        left_tuple(3, "charlie"),
    };
    auto right = std::vector<Tuple>{
        right_tuple(2, "sales"),
    };

    auto pred = eq_pred();

    auto nlj_rows =
        run_nlj(std::vector<Tuple>(left), std::vector<Tuple>(right), JoinType::LEFT, pred.get());
    auto smj_rows = run_smj(std::vector<Tuple>(left), std::vector<Tuple>(right), JoinType::LEFT);

    EXPECT_EQ(nlj_rows.size(), smj_rows.size());
    EXPECT_EQ(nlj_rows.size(), 3u);

    // Both should have 1 matched + 2 unmatched.
    size_t nlj_null_right = 0;
    size_t smj_null_right = 0;
    for (auto& r : nlj_rows) {
        if (r.values[2].is_null()) {
            ++nlj_null_right;
        }
    }
    for (auto& r : smj_rows) {
        if (r.values[2].is_null()) {
            ++smj_null_right;
        }
    }
    EXPECT_EQ(nlj_null_right, 2u);
    EXPECT_EQ(smj_null_right, 2u);
}

TEST(QA_JoinConsistency, FullJoinNljAndSmjAgree) {
    auto left = std::vector<Tuple>{
        left_tuple(1, "alice"),
        left_tuple(3, "charlie"),
    };
    auto right = std::vector<Tuple>{
        right_tuple(2, "sales"),
        right_tuple(3, "eng"),
    };

    auto pred = eq_pred();

    auto nlj_rows =
        run_nlj(std::vector<Tuple>(left), std::vector<Tuple>(right), JoinType::FULL, pred.get());
    auto smj_rows = run_smj(std::vector<Tuple>(left), std::vector<Tuple>(right), JoinType::FULL);

    // (1, NULL), (3, eng), (NULL, sales) = 3
    EXPECT_EQ(nlj_rows.size(), 3u);
    EXPECT_EQ(smj_rows.size(), 3u);
}
