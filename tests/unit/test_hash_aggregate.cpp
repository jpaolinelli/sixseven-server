#include "giodb/common/coercion.h"
#include "giodb/common/value.h"
#include "giodb/executor/expr_evaluator.h"
#include "giodb/executor/filter.h"
#include "giodb/executor/hash_aggregate.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace giodb {
namespace {

// ===========================================================================
// Test fixture
// ===========================================================================

class HashAggregateTest : public ::testing::Test {
protected:
    // -- Schema builders ----------------------------------------------------

    static OutputSchema employee_schema() {
        return OutputSchema({
            {"emp", "dept", TypeId::STRING, false, 1},
            {"emp", "name", TypeId::STRING, false, 1},
            {"emp", "salary", TypeId::INT32, true, 1},
        });
    }

    // -- Tuple builders -----------------------------------------------------

    static Tuple make_emp(const std::string& dept, const std::string& name, int32_t salary) {
        return Tuple{{Value(dept), Value(name), Value(salary)}, {}};
    }

    static Tuple make_emp_null_salary(const std::string& dept, const std::string& name) {
        return Tuple{{Value(dept), Value(name), Value::make_null()}, {}};
    }

    // -- Standard test data -------------------------------------------------

    static std::vector<Tuple> standard_data() {
        return {
            make_emp("eng", "alice", 100),
            make_emp("eng", "bob", 120),
            make_emp("eng", "charlie", 110),
            make_emp("sales", "dave", 90),
            make_emp("sales", "eve", 95),
            make_emp("hr", "frank", 80),
        };
    }

    static std::vector<Tuple> data_with_nulls() {
        return {
            make_emp("eng", "alice", 100),
            make_emp_null_salary("eng", "bob"),
            make_emp("sales", "charlie", 90),
            make_emp_null_salary("sales", "dave"),
            make_emp_null_salary("sales", "eve"),
        };
    }

    // -- Expression builders ------------------------------------------------

    static ExprPtr col_ref(const std::string& table, const std::string& column) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = table;
        e->column = column;
        return e;
    }

    static ExprPtr col_ref(const std::string& column) { return col_ref("", column); }

    static ExprPtr star_ref() { return col_ref("", "*"); }

    static ExprPtr lit_int(int64_t v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::INTEGER;
        e->value = std::to_string(v);
        return e;
    }

    static ExprPtr lit_str(const std::string& v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::STRING;
        e->value = v;
        return e;
    }

    static ExprPtr make_agg(const std::string& name, ExprPtr arg = nullptr, bool distinct = false) {
        auto e = std::make_unique<FunctionCallExpr>();
        e->name = name;
        e->distinct = distinct;
        if (arg) {
            e->args.push_back(std::move(arg));
        }
        return e;
    }

    static ExprPtr make_string_agg(ExprPtr arg, ExprPtr separator) {
        auto e = std::make_unique<FunctionCallExpr>();
        e->name = "STRING_AGG";
        e->args.push_back(std::move(arg));
        e->args.push_back(std::move(separator));
        return e;
    }

    static ExprPtr binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto e = std::make_unique<BinaryExpr>();
        e->op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    // -- Aggregate operator builder -----------------------------------------

    struct AggTestBuilder {
        std::vector<const Expr*> group_by_ptrs;
        std::vector<AggregateDescriptor> descriptors;
        std::vector<OutputColumn> out_cols;
        std::vector<ExprPtr> owned; // Keep expression ownership

        void add_group_by(ExprPtr expr, const OutputColumn& col) {
            group_by_ptrs.push_back(expr.get());
            out_cols.push_back(col);
            owned.push_back(std::move(expr));
        }

        void add_agg(AggFunc func, ExprPtr arg = nullptr, const std::string& sep = "") {
            AggregateDescriptor desc;
            desc.func = func;
            desc.separator = sep;

            std::string col_name = "__agg_" + std::to_string(descriptors.size());
            TypeId type = TypeId::INT64;
            bool nullable = true;

            switch (func) {
            case AggFunc::COUNT_STAR:
            case AggFunc::COUNT:
            case AggFunc::COUNT_DISTINCT:
                type = TypeId::INT64;
                nullable = false;
                break;
            case AggFunc::SUM:
                type = TypeId::INT64;
                break;
            case AggFunc::AVG:
                type = TypeId::FLOAT64;
                break;
            case AggFunc::MIN:
            case AggFunc::MAX:
                type = TypeId::INT32; // Depends on input, but INT32 for salary tests
                break;
            case AggFunc::STRING_AGG:
                type = TypeId::STRING;
                break;
            }

            if (arg) {
                desc.arg = arg.get();
                owned.push_back(std::move(arg));
            }

            descriptors.push_back(std::move(desc));
            out_cols.push_back({"", col_name, type, nullable, 0});
        }

        std::unique_ptr<HashAggregateOperator> build(std::unique_ptr<Iterator> child,
                                                     const BoundStatement& bound) {
            return std::make_unique<HashAggregateOperator>(std::move(child),
                                                           std::move(group_by_ptrs),
                                                           std::move(descriptors),
                                                           bound,
                                                           OutputSchema(std::move(out_cols)));
        }
    };

    // -- Result helpers -----------------------------------------------------

    static std::vector<Tuple> run(Iterator& iter) {
        auto result = collect_all(iter);
        return result;
    }

    /// Find a row where the first column matches a given string.
    static const Tuple* find_group(const std::vector<Tuple>& rows, const std::string& key) {
        for (auto& row : rows) {
            if (!row.values.empty() && !row.values[0].is_null() &&
                row.values[0].type_id() == TypeId::STRING && row.values[0].as_string() == key) {
                return &row;
            }
        }
        return nullptr;
    }
};

// ===========================================================================
// COUNT(*) tests
// ===========================================================================

TEST_F(HashAggregateTest, CountStarGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 3u);

    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    auto* hr = find_group(rows, "hr");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);
    ASSERT_NE(hr, nullptr);

    EXPECT_EQ(eng->values[1].as_int64(), 3);
    EXPECT_EQ(sales->values[1].as_int64(), 2);
    EXPECT_EQ(hr->values[1].as_int64(), 1);
}

TEST_F(HashAggregateTest, CountStarGlobalNoGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int64(), 6);
}

TEST_F(HashAggregateTest, CountStarEmptyInput) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::vector<Tuple>{});
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u); // Global agg produces one row even with empty input.
    EXPECT_EQ(rows[0].values[0].as_int64(), 0);
}

TEST_F(HashAggregateTest, CountStarEmptyInputWithGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::vector<Tuple>{});
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 0u); // GROUP BY with empty input = no groups.
}

// ===========================================================================
// COUNT(col) tests
// ===========================================================================

TEST_F(HashAggregateTest, CountColSkipsNulls) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), data_with_nulls());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 2u);

    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);

    EXPECT_EQ(eng->values[1].as_int64(), 1);   // alice has salary, bob is NULL
    EXPECT_EQ(sales->values[1].as_int64(), 1); // charlie has salary, dave+eve are NULL
}

TEST_F(HashAggregateTest, CountStarCountsNulls) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), data_with_nulls());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);

    EXPECT_EQ(eng->values[1].as_int64(), 2);   // alice + bob (even though bob.salary is NULL)
    EXPECT_EQ(sales->values[1].as_int64(), 3); // charlie + dave + eve
}

// ===========================================================================
// COUNT(DISTINCT col) tests
// ===========================================================================

TEST_F(HashAggregateTest, CountDistinct) {
    // Data with duplicate department values.
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 100),
        make_emp("eng", "charlie", 120),
        make_emp("sales", "dave", 90),
        make_emp("sales", "eve", 90),
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);

    EXPECT_EQ(eng->values[1].as_int64(), 2);   // 100 and 120
    EXPECT_EQ(sales->values[1].as_int64(), 1); // only 90
}

TEST_F(HashAggregateTest, CountDistinctSkipsNulls) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), data_with_nulls());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[0].as_int64(), 2); // 100 and 90 (NULLs excluded)
}

// ===========================================================================
// SUM tests
// ===========================================================================

TEST_F(HashAggregateTest, SumGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    auto* hr = find_group(rows, "hr");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);
    ASSERT_NE(hr, nullptr);

    EXPECT_EQ(eng->values[1].as_int64(), 330);   // 100+120+110
    EXPECT_EQ(sales->values[1].as_int64(), 185); // 90+95
    EXPECT_EQ(hr->values[1].as_int64(), 80);
}

TEST_F(HashAggregateTest, SumSkipsNulls) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), data_with_nulls());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);

    EXPECT_EQ(eng->values[1].as_int64(), 100);  // alice=100, bob=NULL → skipped
    EXPECT_EQ(sales->values[1].as_int64(), 90); // charlie=90, dave+eve=NULL → skipped
}

TEST_F(HashAggregateTest, SumAllNullsReturnsNull) {
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp_null_salary("eng", "bob"),
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);
    EXPECT_TRUE(eng->values[1].is_null()); // SUM over all NULLs = NULL
}

// ===========================================================================
// AVG tests
// ===========================================================================

TEST_F(HashAggregateTest, AvgGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::AVG, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    auto* hr = find_group(rows, "hr");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);
    ASSERT_NE(hr, nullptr);

    EXPECT_DOUBLE_EQ(eng->values[1].as_float64(), 110.0);  // (100+120+110)/3
    EXPECT_DOUBLE_EQ(sales->values[1].as_float64(), 92.5); // (90+95)/2
    EXPECT_DOUBLE_EQ(hr->values[1].as_float64(), 80.0);
}

TEST_F(HashAggregateTest, AvgSkipsNulls) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::AVG, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), data_with_nulls());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);
    EXPECT_DOUBLE_EQ(eng->values[1].as_float64(), 100.0); // Only alice=100
}

TEST_F(HashAggregateTest, AvgAllNullsReturnsNull) {
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp_null_salary("eng", "bob"),
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::AVG, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);
    EXPECT_TRUE(eng->values[1].is_null());
}

// ===========================================================================
// MIN tests
// ===========================================================================

TEST_F(HashAggregateTest, MinGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);

    EXPECT_EQ(eng->values[1].as_int32(), 100);
    EXPECT_EQ(sales->values[1].as_int32(), 90);
}

TEST_F(HashAggregateTest, MinSkipsNulls) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), data_with_nulls());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);
    EXPECT_EQ(eng->values[1].as_int32(), 100); // alice=100, bob=NULL → skipped
}

TEST_F(HashAggregateTest, MinAllNullsReturnsNull) {
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);
    EXPECT_TRUE(eng->values[1].is_null());
}

// ===========================================================================
// MAX tests
// ===========================================================================

TEST_F(HashAggregateTest, MaxGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MAX, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    auto* sales = find_group(rows, "sales");
    ASSERT_NE(eng, nullptr);
    ASSERT_NE(sales, nullptr);

    EXPECT_EQ(eng->values[1].as_int32(), 120);
    EXPECT_EQ(sales->values[1].as_int32(), 95);
}

// ===========================================================================
// STRING_AGG tests
// ===========================================================================

TEST_F(HashAggregateTest, StringAggGroupBy) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});

    AggregateDescriptor desc;
    desc.func = AggFunc::STRING_AGG;
    auto name_expr = col_ref("emp", "name");
    desc.arg = name_expr.get();
    desc.separator = ",";
    builder.owned.push_back(std::move(name_expr));
    builder.descriptors.push_back(std::move(desc));
    builder.out_cols.push_back({"", "__agg_0", TypeId::STRING, true, 0});

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* hr = find_group(rows, "hr");
    ASSERT_NE(hr, nullptr);
    EXPECT_EQ(hr->values[1].as_string(), "frank");

    // eng has 3 names — order depends on input order.
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);
    EXPECT_EQ(eng->values[1].as_string(), "alice,bob,charlie");
}

TEST_F(HashAggregateTest, StringAggSkipsNulls) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("grp")), Value(std::string("a")), Value(int32_t(1))}, {}},
        Tuple{{Value(std::string("grp")), Value::make_null(), Value(int32_t(2))}, {}},
        Tuple{{Value(std::string("grp")), Value(std::string("c")), Value(int32_t(3))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});

    AggregateDescriptor desc;
    desc.func = AggFunc::STRING_AGG;
    auto name_expr = col_ref("emp", "name");
    desc.arg = name_expr.get();
    desc.separator = "-";
    builder.owned.push_back(std::move(name_expr));
    builder.descriptors.push_back(std::move(desc));
    builder.out_cols.push_back({"", "__agg_0", TypeId::STRING, true, 0});

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_string(), "a-c"); // NULL skipped
}

// ===========================================================================
// Multiple aggregates in one query
// ===========================================================================

TEST_F(HashAggregateTest, MultipleAggregates) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::AVG, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::MIN, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::MAX, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    auto* eng = find_group(rows, "eng");
    ASSERT_NE(eng, nullptr);

    // dept=eng: alice(100), bob(120), charlie(110)
    EXPECT_EQ(eng->values[1].as_int64(), 3);              // COUNT(*)
    EXPECT_EQ(eng->values[2].as_int64(), 330);            // SUM
    EXPECT_DOUBLE_EQ(eng->values[3].as_float64(), 110.0); // AVG
    EXPECT_EQ(eng->values[4].as_int32(), 100);            // MIN
    EXPECT_EQ(eng->values[5].as_int32(), 120);            // MAX
}

// ===========================================================================
// Multi-column GROUP BY
// ===========================================================================

TEST_F(HashAggregateTest, MultiColumnGroupBy) {
    // Schema: dept, region, salary
    OutputSchema schema({
        {"t", "dept", TypeId::STRING, false, 1},
        {"t", "region", TypeId::STRING, false, 1},
        {"t", "salary", TypeId::INT32, false, 1},
    });

    std::vector<Tuple> data = {
        Tuple{{Value(std::string("eng")), Value(std::string("west")), Value(int32_t(100))}, {}},
        Tuple{{Value(std::string("eng")), Value(std::string("west")), Value(int32_t(120))}, {}},
        Tuple{{Value(std::string("eng")), Value(std::string("east")), Value(int32_t(110))}, {}},
        Tuple{{Value(std::string("sales")), Value(std::string("west")), Value(int32_t(90))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "dept"), {"t", "dept", TypeId::STRING, false, 1});
    builder.add_group_by(col_ref("t", "region"), {"t", "region", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("t", "salary"));

    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 3u); // eng/west, eng/east, sales/west

    // Find eng/west group.
    const Tuple* eng_west = nullptr;
    for (auto& row : rows) {
        if (row.values[0].as_string() == "eng" && row.values[1].as_string() == "west") {
            eng_west = &row;
        }
    }
    ASSERT_NE(eng_west, nullptr);
    EXPECT_EQ(eng_west->values[2].as_int64(), 2);   // COUNT(*)
    EXPECT_EQ(eng_west->values[3].as_int64(), 220); // SUM(100+120)
}

// ===========================================================================
// Global aggregation (no GROUP BY)
// ===========================================================================

TEST_F(HashAggregateTest, GlobalAggregation) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::AVG, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::MIN, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::MAX, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(rows[0].values[0].as_int64(), 6);
    EXPECT_EQ(rows[0].values[1].as_int64(), 595); // 100+120+110+90+95+80
    double expected_avg = 595.0 / 6.0;
    EXPECT_DOUBLE_EQ(rows[0].values[2].as_float64(), expected_avg);
    EXPECT_EQ(rows[0].values[3].as_int32(), 80);
    EXPECT_EQ(rows[0].values[4].as_int32(), 120);
}

// ===========================================================================
// HAVING clause (via FilterOperator on aggregate output)
// ===========================================================================

TEST_F(HashAggregateTest, HavingFilter) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    // HAVING COUNT(*) > 1 — filter to keep only groups with count > 1.
    // The aggregate output schema is [dept, __agg_0].
    // Build: __agg_0 > 1
    auto having_lhs = col_ref("", "__agg_0");
    auto having_rhs = lit_int(1);
    auto having_pred = binary_expr(BinaryOp::GREATER, std::move(having_lhs), std::move(having_rhs));

    FilterOperator filter(std::move(agg), *having_pred, bound);

    auto rows = run(filter);
    ASSERT_EQ(rows.size(), 2u); // eng(3) and sales(2) pass, hr(1) filtered out.

    for (auto& row : rows) {
        EXPECT_TRUE(row.values[0].as_string() == "eng" || row.values[0].as_string() == "sales");
    }
}

// ===========================================================================
// NULL group key
// ===========================================================================

TEST_F(HashAggregateTest, NullGroupKey) {
    OutputSchema schema({
        {"t", "grp", TypeId::STRING, true, 1},
        {"t", "val", TypeId::INT32, false, 1},
    });

    std::vector<Tuple> data = {
        Tuple{{Value(std::string("a")), Value(int32_t(10))}, {}},
        Tuple{{Value::make_null(), Value(int32_t(20))}, {}},
        Tuple{{Value::make_null(), Value(int32_t(30))}, {}},
        Tuple{{Value(std::string("a")), Value(int32_t(40))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, true, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 2u); // "a" and NULL

    // Find the NULL group.
    const Tuple* null_group = nullptr;
    const Tuple* a_group = nullptr;
    for (auto& row : rows) {
        if (row.values[0].is_null()) {
            null_group = &row;
        } else {
            a_group = &row;
        }
    }
    ASSERT_NE(null_group, nullptr);
    ASSERT_NE(a_group, nullptr);

    EXPECT_EQ(null_group->values[1].as_int64(), 50); // 20+30
    EXPECT_EQ(a_group->values[1].as_int64(), 50);    // 10+40
}

// ===========================================================================
// Volcano iterator protocol
// ===========================================================================

TEST_F(HashAggregateTest, IteratorProtocol) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto open_result = agg->open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    auto row1 = agg->next();
    ASSERT_TRUE(row1.has_value()) << row1.error().message;
    ASSERT_TRUE(row1->has_value());
    EXPECT_EQ(row1->value().values[0].as_int64(), 6);

    auto row2 = agg->next();
    ASSERT_TRUE(row2.has_value()) << row2.error().message;
    ASSERT_FALSE(row2->has_value()); // End of stream.

    agg->close();
}

TEST_F(HashAggregateTest, OutputSchemaCorrect) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));

    auto child = std::make_unique<VectorIterator>(employee_schema(), standard_data());
    auto agg = builder.build(std::move(child), bound);

    auto& schema = agg->output_schema();
    ASSERT_EQ(schema.column_count(), 3u);
    EXPECT_EQ(schema.column(0).name, "dept");
    EXPECT_EQ(schema.column(0).type_id, TypeId::STRING);
    EXPECT_EQ(schema.column(1).name, "__agg_0");
    EXPECT_EQ(schema.column(1).type_id, TypeId::INT64);
    EXPECT_EQ(schema.column(2).name, "__agg_1");
    EXPECT_EQ(schema.column(2).type_id, TypeId::INT64);
}

// ===========================================================================
// Single-row groups
// ===========================================================================

TEST_F(HashAggregateTest, SingleRowPerGroup) {
    std::vector<Tuple> data = {
        make_emp("a", "alice", 100),
        make_emp("b", "bob", 200),
        make_emp("c", "charlie", 300),
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("emp", "dept"), {"emp", "dept", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("emp", "salary"));
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(employee_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 3u);

    for (auto& row : rows) {
        EXPECT_EQ(row.values[2].as_int64(), 1); // COUNT(*) = 1 for each
    }
}

} // namespace
} // namespace giodb
