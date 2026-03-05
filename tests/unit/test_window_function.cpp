#include "sixseven/common/value.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/executor/window_function.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace sixseven {
namespace {

// ===========================================================================
// Test fixture
// ===========================================================================

class WindowFunctionTest : public ::testing::Test {
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

    static std::vector<Tuple> data_with_ties() {
        return {
            make_emp("eng", "alice", 100),
            make_emp("eng", "bob", 100),
            make_emp("eng", "charlie", 120),
            make_emp("sales", "dave", 90),
            make_emp("sales", "eve", 90),
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

    // -- Window operator builder --------------------------------------------

    struct WinTestBuilder {
        std::vector<const Expr*> partition_by_ptrs;
        std::vector<SortKey> order_by_keys;
        std::vector<WindowFunctionDescriptor> descriptors;
        std::vector<OutputColumn> out_cols;
        std::vector<ExprPtr> owned;

        void add_partition_by(ExprPtr expr) {
            partition_by_ptrs.push_back(expr.get());
            owned.push_back(std::move(expr));
        }

        void add_order_by(ExprPtr expr, SortDirection dir = SortDirection::ASC) {
            SortKey key;
            key.expr = expr.get();
            key.direction = dir;
            order_by_keys.push_back(key);
            owned.push_back(std::move(expr));
        }

        void add_window_func(WindowFunc func,
                             ExprPtr arg = nullptr,
                             WindowFrameSpec frame = {},
                             int64_t offset = 1,
                             Value default_val = Value::make_null(),
                             int64_t ntile_buckets = 1) {
            WindowFunctionDescriptor desc;
            desc.func = func;
            desc.offset = offset;
            desc.default_value = std::move(default_val);
            desc.ntile_buckets = ntile_buckets;
            desc.frame = frame;

            std::string col_name = "__win_" + std::to_string(descriptors.size());
            TypeId type = TypeId::INT64;
            bool nullable = true;

            switch (func) {
            case WindowFunc::ROW_NUMBER:
            case WindowFunc::RANK:
            case WindowFunc::DENSE_RANK:
            case WindowFunc::NTILE:
                type = TypeId::INT64;
                nullable = false;
                break;
            case WindowFunc::LAG:
            case WindowFunc::LEAD:
            case WindowFunc::FIRST_VALUE:
            case WindowFunc::LAST_VALUE:
                type = TypeId::INT32;
                break;
            case WindowFunc::WIN_SUM:
                type = TypeId::INT64;
                break;
            case WindowFunc::WIN_AVG:
                type = TypeId::FLOAT64;
                break;
            case WindowFunc::WIN_COUNT:
                type = TypeId::INT64;
                nullable = false;
                break;
            case WindowFunc::WIN_MIN:
            case WindowFunc::WIN_MAX:
                type = TypeId::INT32;
                break;
            }

            if (arg) {
                desc.arg = arg.get();
                owned.push_back(std::move(arg));
            }

            descriptors.push_back(std::move(desc));
            out_cols.push_back({"", col_name, type, nullable, 0});
        }

        std::unique_ptr<WindowOperator> build(std::unique_ptr<Iterator> child,
                                              const BoundStatement& bound,
                                              const OutputSchema& child_schema) {
            // Build output schema: child columns + window result columns.
            std::vector<OutputColumn> all_cols;
            for (size_t i = 0; i < child_schema.column_count(); ++i) {
                all_cols.push_back(child_schema.column(i));
            }
            for (auto& c : out_cols) {
                all_cols.push_back(c);
            }

            return std::make_unique<WindowOperator>(std::move(child),
                                                    std::move(partition_by_ptrs),
                                                    std::move(order_by_keys),
                                                    std::move(descriptors),
                                                    bound,
                                                    OutputSchema(std::move(all_cols)));
        }
    };

    // -- Result helpers -----------------------------------------------------

    static std::vector<Tuple> run(Iterator& iter) { return collect_all(iter); }

    /// Find all rows where column 0 matches a given string (for identifying partitions).
    static std::vector<const Tuple*> find_partition(const std::vector<Tuple>& rows,
                                                    const std::string& key) {
        std::vector<const Tuple*> result;
        for (auto& row : rows) {
            if (!row.values.empty() && !row.values[0].is_null() &&
                row.values[0].type_id() == TypeId::STRING && row.values[0].as_string() == key) {
                result.push_back(&row);
            }
        }
        return result;
    }

    /// Get the window result column value by name pattern from a tuple.
    /// Window columns start after the child schema columns.
    static const Value& win_col(const Tuple& t, size_t child_cols, size_t win_idx) {
        return t.values[child_cols + win_idx];
    }
};

// ===========================================================================
// ROW_NUMBER tests
// ===========================================================================

TEST_F(WindowFunctionTest, RowNumberNoPartition) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // All rows should get row numbers. Since order is by salary ASC:
    // frank(80)=1, dave(90)=2, eve(95)=3, alice(100)=4, charlie(110)=5, bob(120)=6
    // But since we preserve original order in output, verify each row got a number.
    for (auto& row : rows) {
        int64_t rn = win_col(row, 3, 0).as_int64();
        EXPECT_GE(rn, 1);
        EXPECT_LE(rn, 6);
    }
}

TEST_F(WindowFunctionTest, RowNumberWithPartition) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // eng partition: alice(100)=1, charlie(110)=2, bob(120)=3
    auto eng = find_partition(rows, "eng");
    ASSERT_EQ(eng.size(), 3u);
    // Collect row numbers for eng partition.
    std::vector<int64_t> eng_rn;
    for (auto* r : eng) {
        eng_rn.push_back(win_col(*r, 3, 0).as_int64());
    }
    std::sort(eng_rn.begin(), eng_rn.end());
    EXPECT_EQ(eng_rn, (std::vector<int64_t>{1, 2, 3}));

    // sales partition: dave(90)=1, eve(95)=2
    auto sales = find_partition(rows, "sales");
    ASSERT_EQ(sales.size(), 2u);
    std::vector<int64_t> sales_rn;
    for (auto* r : sales) {
        sales_rn.push_back(win_col(*r, 3, 0).as_int64());
    }
    std::sort(sales_rn.begin(), sales_rn.end());
    EXPECT_EQ(sales_rn, (std::vector<int64_t>{1, 2}));

    // hr partition: frank(80)=1
    auto hr = find_partition(rows, "hr");
    ASSERT_EQ(hr.size(), 1u);
    EXPECT_EQ(win_col(*hr[0], 3, 0).as_int64(), 1);
}

// ===========================================================================
// RANK tests
// ===========================================================================

TEST_F(WindowFunctionTest, RankWithTies) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::RANK);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, data_with_ties());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 5u);

    // eng: alice(100), bob(100), charlie(120) -> ranks: 1, 1, 3
    auto eng = find_partition(rows, "eng");
    ASSERT_EQ(eng.size(), 3u);
    std::vector<int64_t> eng_ranks;
    for (auto* r : eng) {
        eng_ranks.push_back(win_col(*r, 3, 0).as_int64());
    }
    std::sort(eng_ranks.begin(), eng_ranks.end());
    EXPECT_EQ(eng_ranks, (std::vector<int64_t>{1, 1, 3}));

    // sales: dave(90), eve(90) -> ranks: 1, 1
    auto sales = find_partition(rows, "sales");
    ASSERT_EQ(sales.size(), 2u);
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 1);
    }
}

TEST_F(WindowFunctionTest, RankNoTies) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::RANK);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: alice(100)=1, charlie(110)=2, bob(120)=3
    auto eng = find_partition(rows, "eng");
    ASSERT_EQ(eng.size(), 3u);
    std::vector<int64_t> eng_ranks;
    for (auto* r : eng) {
        eng_ranks.push_back(win_col(*r, 3, 0).as_int64());
    }
    std::sort(eng_ranks.begin(), eng_ranks.end());
    EXPECT_EQ(eng_ranks, (std::vector<int64_t>{1, 2, 3}));
}

// ===========================================================================
// DENSE_RANK tests
// ===========================================================================

TEST_F(WindowFunctionTest, DenseRankWithTies) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::DENSE_RANK);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, data_with_ties());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: alice(100), bob(100), charlie(120) -> dense_ranks: 1, 1, 2
    auto eng = find_partition(rows, "eng");
    ASSERT_EQ(eng.size(), 3u);
    std::vector<int64_t> eng_dr;
    for (auto* r : eng) {
        eng_dr.push_back(win_col(*r, 3, 0).as_int64());
    }
    std::sort(eng_dr.begin(), eng_dr.end());
    EXPECT_EQ(eng_dr, (std::vector<int64_t>{1, 1, 2}));

    // sales: dave(90), eve(90) -> dense_ranks: 1, 1
    auto sales = find_partition(rows, "sales");
    ASSERT_EQ(sales.size(), 2u);
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 1);
    }
}

// ===========================================================================
// NTILE tests
// ===========================================================================

TEST_F(WindowFunctionTest, NtileEvenDistribution) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::NTILE, nullptr, {}, 1, Value::make_null(), 3);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // 6 rows, 3 buckets: each bucket gets 2 rows.
    std::vector<int64_t> buckets;
    for (auto& row : rows) {
        buckets.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(buckets.begin(), buckets.end());
    EXPECT_EQ(buckets, (std::vector<int64_t>{1, 1, 2, 2, 3, 3}));
}

TEST_F(WindowFunctionTest, NtileUnevenDistribution) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::NTILE, nullptr, {}, 1, Value::make_null(), 4);

    auto schema = employee_schema();
    // 5 rows, 4 buckets: first bucket gets 2, rest get 1 each.
    auto child = std::make_unique<VectorIterator>(schema, data_with_ties());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 5u);

    std::vector<int64_t> buckets;
    for (auto& row : rows) {
        buckets.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(buckets.begin(), buckets.end());
    // 5 rows, 4 buckets: first 1 bucket gets 2 rows (5%4=1 extra), rest get 1.
    EXPECT_EQ(buckets, (std::vector<int64_t>{1, 1, 2, 3, 4}));
}

// ===========================================================================
// LAG tests
// ===========================================================================

TEST_F(WindowFunctionTest, LagDefault) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"));

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // eng sorted by salary: alice(100), charlie(110), bob(120)
    // LAG(salary,1) = NULL, 100, 110
    auto eng = find_partition(rows, "eng");
    ASSERT_EQ(eng.size(), 3u);

    // Find alice (salary=100) -> LAG should be NULL.
    for (auto* r : eng) {
        if (r->values[2].as_int32() == 100) {
            EXPECT_TRUE(win_col(*r, 3, 0).is_null());
        }
    }
}

TEST_F(WindowFunctionTest, LagWithDefaultValue) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"), {}, 1, Value(int32_t(-1)));

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // eng: alice(100) has no preceding row -> default = -1
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        if (r->values[2].as_int32() == 100) {
            EXPECT_EQ(win_col(*r, 3, 0).as_int32(), -1);
        }
    }
}

TEST_F(WindowFunctionTest, LagOffset2) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"), {}, 2);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng sorted by salary: alice(100), charlie(110), bob(120)
    // LAG(salary, 2) = NULL, NULL, 100
    auto eng = find_partition(rows, "eng");
    ASSERT_EQ(eng.size(), 3u);

    for (auto* r : eng) {
        if (r->values[2].as_int32() == 120) {
            // bob: LAG(2) = alice's salary = 100
            EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 100);
        } else {
            // alice and charlie: LAG(2) = NULL
            EXPECT_TRUE(win_col(*r, 3, 0).is_null());
        }
    }
}

// ===========================================================================
// LEAD tests
// ===========================================================================

TEST_F(WindowFunctionTest, LeadDefault) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LEAD, col_ref("emp", "salary"));

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // eng sorted by salary: alice(100), charlie(110), bob(120)
    // LEAD(salary,1) = 110, 120, NULL
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        if (r->values[2].as_int32() == 100) {
            EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 110); // charlie's salary
        }
        if (r->values[2].as_int32() == 110) {
            EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 120); // bob's salary
        }
        if (r->values[2].as_int32() == 120) {
            EXPECT_TRUE(win_col(*r, 3, 0).is_null()); // No next row
        }
    }
}

TEST_F(WindowFunctionTest, LeadWithDefaultValue) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LEAD, col_ref("emp", "salary"), {}, 1, Value(int32_t(999)));

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: bob(120) is last -> default = 999
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        if (r->values[2].as_int32() == 120) {
            EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 999);
        }
    }
}

// ===========================================================================
// FIRST_VALUE tests
// ===========================================================================

TEST_F(WindowFunctionTest, FirstValueUnboundedPreceding) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::FIRST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng sorted by salary: alice(100), charlie(110), bob(120)
    // FIRST_VALUE = 100 for all rows in eng partition.
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 100);
    }

    // sales sorted: dave(90), eve(95) -> FIRST_VALUE = 90
    auto sales = find_partition(rows, "sales");
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 90);
    }
}

// ===========================================================================
// LAST_VALUE tests
// ===========================================================================

TEST_F(WindowFunctionTest, LastValueUnboundedFollowing) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::LAST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: LAST_VALUE with UNBOUNDED FOLLOWING = 120 (bob, highest salary)
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 120);
    }

    // hr: single row, LAST_VALUE = 80
    auto hr = find_partition(rows, "hr");
    ASSERT_EQ(hr.size(), 1u);
    EXPECT_EQ(win_col(*hr[0], 3, 0).as_int32(), 80);
}

TEST_F(WindowFunctionTest, LastValueCurrentRow) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::LAST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng sorted by salary: alice(100), charlie(110), bob(120)
    // LAST_VALUE with CURRENT ROW = current row's salary
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int32(), r->values[2].as_int32());
    }
}

// ===========================================================================
// Window aggregate SUM tests
// ===========================================================================

TEST_F(WindowFunctionTest, WinSumUnboundedFrame) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng total: 100+110+120=330
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 330);
    }

    // sales total: 90+95=185
    auto sales = find_partition(rows, "sales");
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 185);
    }
}

TEST_F(WindowFunctionTest, WinSumRunningTotal) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng sorted by salary: alice(100), charlie(110), bob(120)
    // Running sum: 100, 210, 330
    auto eng = find_partition(rows, "eng");
    std::vector<int64_t> running_sums;
    for (auto* r : eng) {
        running_sums.push_back(win_col(*r, 3, 0).as_int64());
    }
    std::sort(running_sums.begin(), running_sums.end());
    EXPECT_EQ(running_sums, (std::vector<int64_t>{100, 210, 330}));
}

// ===========================================================================
// Window aggregate AVG tests
// ===========================================================================

TEST_F(WindowFunctionTest, WinAvgUnboundedFrame) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_AVG, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng avg: (100+110+120)/3 = 110
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_DOUBLE_EQ(win_col(*r, 3, 0).as_float64(), 110.0);
    }
}

// ===========================================================================
// Window aggregate COUNT tests
// ===========================================================================

TEST_F(WindowFunctionTest, WinCountUnboundedFrame) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_COUNT, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng count: 3
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 3);
    }

    // hr count: 1
    auto hr = find_partition(rows, "hr");
    for (auto* r : hr) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 1);
    }
}

TEST_F(WindowFunctionTest, WinCountSkipsNulls) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_COUNT, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, data_with_nulls());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: alice(100), bob(NULL) -> count of non-null = 1
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 1);
    }

    // sales: charlie(90), dave(NULL), eve(NULL) -> count of non-null = 1
    auto sales = find_partition(rows, "sales");
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 1);
    }
}

// ===========================================================================
// Window aggregate MIN/MAX tests
// ===========================================================================

TEST_F(WindowFunctionTest, WinMinMaxUnboundedFrame) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_MIN, col_ref("emp", "salary"), frame);
    builder.add_window_func(WindowFunc::WIN_MAX, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: min=100, max=120
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 100); // MIN
        EXPECT_EQ(win_col(*r, 3, 1).as_int32(), 120); // MAX
    }

    // sales: min=90, max=95
    auto sales = find_partition(rows, "sales");
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int32(), 90);
        EXPECT_EQ(win_col(*r, 3, 1).as_int32(), 95);
    }
}

// ===========================================================================
// Frame specification tests
// ===========================================================================

TEST_F(WindowFunctionTest, FrameNPrecedingNFollowing) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_PRECEDING;
    frame.start_offset = 1;
    frame.end_bound = FrameBound::N_FOLLOWING;
    frame.end_offset = 1;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // Sorted by salary: frank(80), dave(90), eve(95), alice(100), charlie(110), bob(120)
    // SUM(salary) ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING:
    // frank: 80+90 = 170
    // dave: 80+90+95 = 265
    // eve: 90+95+100 = 285
    // alice: 95+100+110 = 305
    // charlie: 100+110+120 = 330
    // bob: 110+120 = 230
    std::vector<int64_t> sums;
    for (auto& row : rows) {
        sums.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(sums.begin(), sums.end());
    EXPECT_EQ(sums, (std::vector<int64_t>{170, 230, 265, 285, 305, 330}));
}

TEST_F(WindowFunctionTest, FrameCurrentRowOnly) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::CURRENT_ROW;
    frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // Each row's SUM should equal its own salary.
    for (auto& row : rows) {
        if (!row.values[2].is_null()) {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(),
                      static_cast<int64_t>(row.values[2].as_int32()));
        }
    }
}

// ===========================================================================
// Multiple window functions in one query
// ===========================================================================

TEST_F(WindowFunctionTest, MultipleWindowFunctions) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);
    builder.add_window_func(WindowFunc::RANK);

    WindowFrameSpec full_frame;
    full_frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    full_frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), full_frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // eng: alice(100)=rn1,rank1,sum330; charlie(110)=rn2,rank2,sum330; bob(120)=rn3,rank3,sum330
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        // SUM should be 330 for all rows.
        EXPECT_EQ(win_col(*r, 3, 2).as_int64(), 330);

        // ROW_NUMBER and RANK should be 1-3.
        int64_t rn = win_col(*r, 3, 0).as_int64();
        int64_t rank = win_col(*r, 3, 1).as_int64();
        EXPECT_GE(rn, 1);
        EXPECT_LE(rn, 3);
        EXPECT_EQ(rn, rank); // No ties, so RANK == ROW_NUMBER.
    }
}

// ===========================================================================
// Empty input
// ===========================================================================

TEST_F(WindowFunctionTest, EmptyInput) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, std::vector<Tuple>{});
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    EXPECT_EQ(rows.size(), 0u);
}

// ===========================================================================
// Single row input
// ===========================================================================

TEST_F(WindowFunctionTest, SingleRowInput) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LEAD, col_ref("emp", "salary"));

    auto schema = employee_schema();
    std::vector<Tuple> data = {make_emp("eng", "alice", 100)};
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(win_col(rows[0], 3, 0).as_int64(), 1); // ROW_NUMBER = 1
    EXPECT_TRUE(win_col(rows[0], 3, 1).is_null());   // LAG = NULL
    EXPECT_TRUE(win_col(rows[0], 3, 2).is_null());   // LEAD = NULL
}

// ===========================================================================
// Volcano iterator protocol
// ===========================================================================

TEST_F(WindowFunctionTest, IteratorProtocol) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    std::vector<Tuple> data = {make_emp("eng", "alice", 100), make_emp("eng", "bob", 120)};
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto open_result = win->open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    auto row1 = win->next();
    ASSERT_TRUE(row1.has_value()) << row1.error().message;
    ASSERT_TRUE(row1->has_value());

    auto row2 = win->next();
    ASSERT_TRUE(row2.has_value()) << row2.error().message;
    ASSERT_TRUE(row2->has_value());

    auto row3 = win->next();
    ASSERT_TRUE(row3.has_value()) << row3.error().message;
    ASSERT_FALSE(row3->has_value()); // End of stream.

    win->close();
}

TEST_F(WindowFunctionTest, OutputSchemaCorrect) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"));

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto& out = win->output_schema();
    ASSERT_EQ(out.column_count(), 5u); // 3 child cols + 2 window cols
    EXPECT_EQ(out.column(0).name, "dept");
    EXPECT_EQ(out.column(1).name, "name");
    EXPECT_EQ(out.column(2).name, "salary");
    EXPECT_EQ(out.column(3).name, "__win_0");
    EXPECT_EQ(out.column(4).name, "__win_1");
}

// ===========================================================================
// No PARTITION BY (entire input is one partition)
// ===========================================================================

TEST_F(WindowFunctionTest, NoPartitionByGlobalWindow) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, standard_data());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 6u);

    // Total salary: 100+120+110+90+95+80 = 595
    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 595);
    }
}

// ===========================================================================
// Window aggregates with NULLs in frame
// ===========================================================================

TEST_F(WindowFunctionTest, WinSumSkipsNulls) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, data_with_nulls());
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);

    // eng: alice(100), bob(NULL) -> SUM = 100
    auto eng = find_partition(rows, "eng");
    for (auto* r : eng) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 100);
    }

    // sales: charlie(90), dave(NULL), eve(NULL) -> SUM = 90
    auto sales = find_partition(rows, "sales");
    for (auto* r : sales) {
        EXPECT_EQ(win_col(*r, 3, 0).as_int64(), 90);
    }
}

TEST_F(WindowFunctionTest, WinSumAllNullsReturnsNull) {
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp_null_salary("eng", "bob"),
    };

    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);
    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null());
    }
}

// ===========================================================================
// Preserves original input order in output
// ===========================================================================

TEST_F(WindowFunctionTest, PreservesOriginalOrder) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    auto data = standard_data();
    // Record original names in order.
    std::vector<std::string> original_names;
    for (auto& t : data) {
        original_names.push_back(t.values[1].as_string());
    }

    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), original_names.size());

    // Output should preserve original input order.
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].values[1].as_string(), original_names[i]);
    }
}

} // namespace
} // namespace sixseven
