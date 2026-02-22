#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/hash_join.h"
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

class HashJoinTest : public ::testing::Test {
protected:
    static OutputSchema probe_schema() {
        return OutputSchema({
            {"probe", "id", TypeId::INT32, false, 1},
            {"probe", "name", TypeId::STRING, true, 1},
        });
    }

    static OutputSchema build_schema() {
        return OutputSchema({
            {"build", "id", TypeId::INT32, false, 2},
            {"build", "dept", TypeId::STRING, true, 2},
        });
    }

    static OutputSchema combined_schema() {
        return OutputSchema({
            {"probe", "id", TypeId::INT32, false, 1},
            {"probe", "name", TypeId::STRING, true, 1},
            {"build", "id", TypeId::INT32, false, 2},
            {"build", "dept", TypeId::STRING, true, 2},
        });
    }

    static Tuple make_probe(int32_t id, const std::string& name) {
        return Tuple{{Value(id), Value(name)}, {}};
    }

    static Tuple make_build(int32_t id, const std::string& dept) {
        return Tuple{{Value(id), Value(dept)}, {}};
    }

    static std::vector<Tuple> standard_probe() {
        return {make_probe(1, "alice"), make_probe(2, "bob"), make_probe(3, "charlie")};
    }

    static std::vector<Tuple> standard_build() {
        return {make_build(1, "eng"), make_build(2, "sales"), make_build(4, "hr")};
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
// In-memory hash join tests
// ===========================================================================

TEST_F(HashJoinTest, InnerJoinBasic) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), standard_probe()),
                          std::make_unique<VectorIterator>(build_schema(), standard_build()),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 2u);
    // alice matches eng, bob matches sales
    // Order may vary due to hash table, so check by content.
    bool found_alice = false;
    bool found_bob = false;
    for (auto& r : rows) {
        if (r.values[0].as_int32() == 1 && r.values[1].as_string() == "alice") {
            EXPECT_EQ(r.values[2].as_int32(), 1);
            EXPECT_EQ(r.values[3].as_string(), "eng");
            found_alice = true;
        }
        if (r.values[0].as_int32() == 2 && r.values[1].as_string() == "bob") {
            EXPECT_EQ(r.values[2].as_int32(), 2);
            EXPECT_EQ(r.values[3].as_string(), "sales");
            found_bob = true;
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);
}

TEST_F(HashJoinTest, InnerJoinNoMatches) {
    auto probe_data = std::vector<Tuple>{make_probe(10, "x")};
    auto build_data = std::vector<Tuple>{make_build(20, "y")};

    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::move(probe_data)),
                          std::make_unique<VectorIterator>(build_schema(), std::move(build_data)),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(HashJoinTest, InnerJoinEmptyBuild) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), standard_probe()),
                          std::make_unique<VectorIterator>(build_schema(), std::vector<Tuple>{}),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(HashJoinTest, InnerJoinDuplicateKeys) {
    auto probe_data =
        std::vector<Tuple>{make_probe(1, "alice"), make_probe(1, "alice2"), make_probe(2, "bob")};
    auto build_data =
        std::vector<Tuple>{make_build(1, "eng"), make_build(1, "eng2"), make_build(3, "hr")};

    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::move(probe_data)),
                          std::make_unique<VectorIterator>(build_schema(), std::move(build_data)),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    // alice x eng, alice x eng2, alice2 x eng, alice2 x eng2 = 4
    ASSERT_EQ(rows.size(), 4u);
}

// ===========================================================================
// LEFT JOIN tests
// ===========================================================================

TEST_F(HashJoinTest, LeftJoinBasic) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), standard_probe()),
                          std::make_unique<VectorIterator>(build_schema(), standard_build()),
                          JoinType::LEFT,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    // alice matches eng, bob matches sales, charlie has no match → NULL build
    ASSERT_EQ(rows.size(), 3u);

    bool found_null_build = false;
    for (auto& r : rows) {
        if (r.values[0].as_int32() == 3) {
            EXPECT_TRUE(r.values[2].is_null());
            EXPECT_TRUE(r.values[3].is_null());
            found_null_build = true;
        }
    }
    EXPECT_TRUE(found_null_build);
}

TEST_F(HashJoinTest, LeftJoinEmptyBuild) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), standard_probe()),
                          std::make_unique<VectorIterator>(build_schema(), std::vector<Tuple>{}),
                          JoinType::LEFT,
                          probe_key.get(),
                          build_key.get(),
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
// Grace hash join tests (triggered by work_mem)
// ===========================================================================

TEST_F(HashJoinTest, GraceHashJoinInner) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    // work_mem=1 forces grace hash join since build has 3 tuples.
    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), standard_probe()),
                          std::make_unique<VectorIterator>(build_schema(), standard_build()),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema(),
                          1);

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 2u);

    bool found_alice = false;
    bool found_bob = false;
    for (auto& r : rows) {
        if (r.values[0].as_int32() == 1) {
            found_alice = true;
        }
        if (r.values[0].as_int32() == 2) {
            found_bob = true;
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);
}

TEST_F(HashJoinTest, GraceHashJoinLeft) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    // work_mem=1 forces grace hash join.
    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), standard_probe()),
                          std::make_unique<VectorIterator>(build_schema(), standard_build()),
                          JoinType::LEFT,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema(),
                          1);

    auto rows = collect_all(join);
    // All 3 probe rows should appear (charlie with NULLs).
    ASSERT_EQ(rows.size(), 3u);
}

TEST_F(HashJoinTest, GraceHashJoinSkewedData) {
    // All build tuples have same key → all land in one partition.
    std::vector<Tuple> probe_data;
    for (int32_t i = 1; i <= 10; ++i) {
        probe_data.push_back(make_probe(i, "p" + std::to_string(i)));
    }
    std::vector<Tuple> build_data;
    for (int32_t i = 0; i < 5; ++i) {
        build_data.push_back(make_build(1, "b" + std::to_string(i)));
    }

    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    // work_mem=2 forces grace hash join.
    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::move(probe_data)),
                          std::make_unique<VectorIterator>(build_schema(), std::move(build_data)),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema(),
                          2);

    auto rows = collect_all(join);
    // probe id=1 matches all 5 build tuples.
    ASSERT_EQ(rows.size(), 5u);
    for (auto& r : rows) {
        EXPECT_EQ(r.values[0].as_int32(), 1);
    }
}

TEST_F(HashJoinTest, GraceHashJoinLargeDataset) {
    std::vector<Tuple> probe_data;
    std::vector<Tuple> build_data;
    for (int32_t i = 0; i < 100; ++i) {
        probe_data.push_back(make_probe(i, "p" + std::to_string(i)));
        build_data.push_back(make_build(i, "b" + std::to_string(i)));
    }

    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    // work_mem=10 forces grace hash join with many partitions.
    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::move(probe_data)),
                          std::make_unique<VectorIterator>(build_schema(), std::move(build_data)),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema(),
                          10);

    auto rows = collect_all(join);
    // All 100 should match (same keys on both sides).
    ASSERT_EQ(rows.size(), 100u);
}

// ===========================================================================
// Output schema test
// ===========================================================================

// ===========================================================================
// NULL join key tests
// ===========================================================================

TEST_F(HashJoinTest, InnerJoinNullKeyNeverMatches) {
    auto probe_data = std::vector<Tuple>{
        make_probe(1, "alice"),
        Tuple{{Value::make_null(), Value(std::string("nulluser"))}, {}},
    };
    auto build_data = std::vector<Tuple>{
        make_build(1, "eng"),
        Tuple{{Value::make_null(), Value(std::string("nulldept"))}, {}},
    };

    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::move(probe_data)),
                          std::make_unique<VectorIterator>(build_schema(), std::move(build_data)),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    // Only alice(1) matches eng(1). NULL keys don't match.
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int32(), 1);
}

TEST_F(HashJoinTest, LeftJoinNullKeyProducesNullBuild) {
    auto probe_data = std::vector<Tuple>{
        make_probe(1, "alice"),
        Tuple{{Value::make_null(), Value(std::string("nulluser"))}, {}},
    };
    auto build_data = std::vector<Tuple>{
        make_build(1, "eng"),
    };

    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::move(probe_data)),
                          std::make_unique<VectorIterator>(build_schema(), std::move(build_data)),
                          JoinType::LEFT,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    auto rows = collect_all(join);
    ASSERT_EQ(rows.size(), 2u);
    // Find the null-key row and verify it got NULL build side.
    bool found_null_probe = false;
    for (auto& r : rows) {
        if (r.values[0].is_null()) {
            EXPECT_TRUE(r.values[2].is_null());
            EXPECT_TRUE(r.values[3].is_null());
            found_null_probe = true;
        }
    }
    EXPECT_TRUE(found_null_probe);
}

// ===========================================================================
// Output schema test
// ===========================================================================

TEST_F(HashJoinTest, OutputSchemaCorrect) {
    auto probe_key = col_ref("probe", "id");
    auto build_key = col_ref("build", "id");
    BoundStatement bound;

    HashJoinOperator join(std::make_unique<VectorIterator>(probe_schema(), std::vector<Tuple>{}),
                          std::make_unique<VectorIterator>(build_schema(), std::vector<Tuple>{}),
                          JoinType::INNER,
                          probe_key.get(),
                          build_key.get(),
                          bound,
                          combined_schema());

    EXPECT_EQ(join.output_schema().column_count(), 4u);
    EXPECT_EQ(join.output_schema().column(0).table_name, "probe");
    EXPECT_EQ(join.output_schema().column(2).table_name, "build");
}
