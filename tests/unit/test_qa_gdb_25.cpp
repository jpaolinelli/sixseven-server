#include "giodb/common/value.h"
#include "giodb/executor/tuple.h"
#include "giodb/executor/window_function.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace giodb {
namespace {

// ===========================================================================
// QA test fixture for GDB-25 — Window Functions
// ===========================================================================

class QA_WindowFunction : public ::testing::Test {
protected:
    // -- Schema builders ----------------------------------------------------

    static OutputSchema employee_schema() {
        return OutputSchema({
            {"emp", "dept", TypeId::STRING, false, 1},
            {"emp", "name", TypeId::STRING, false, 1},
            {"emp", "salary", TypeId::INT32, true, 1},
        });
    }

    static OutputSchema int64_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::INT64, true, 1},
        });
    }

    // -- Tuple builders -----------------------------------------------------

    static Tuple make_emp(const std::string& dept, const std::string& name, int32_t salary) {
        return Tuple{{Value(dept), Value(name), Value(salary)}, {}};
    }

    static Tuple make_emp_null_salary(const std::string& dept, const std::string& name) {
        return Tuple{{Value(dept), Value(name), Value::make_null()}, {}};
    }

    static Tuple make_int64_row(const std::string& grp, int64_t val) {
        return Tuple{{Value(grp), Value(val)}, {}};
    }

    static Tuple make_int64_row_null(const std::string& grp) {
        return Tuple{{Value(grp), Value::make_null()}, {}};
    }

    // -- Expression builders ------------------------------------------------

    static ExprPtr col_ref(const std::string& table, const std::string& column) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = table;
        e->column = column;
        return e;
    }

    // -- Window operator builder (same as existing test) --------------------

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

    static const Value& win_col(const Tuple& t, size_t child_cols, size_t win_idx) {
        return t.values[child_cols + win_idx];
    }

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
};

// ===========================================================================
// FIRST_VALUE / LAST_VALUE — empty frame bug
// ===========================================================================

TEST_F(QA_WindowFunction, FirstValueInvertedFrameShouldReturnNull) {
    // Frame: ROWS BETWEEN 5 FOLLOWING AND CURRENT ROW
    // For any row, frame_start > frame_end -> empty frame.
    // SQL spec: FIRST_VALUE on empty frame should return NULL.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_FOLLOWING;
    frame.start_offset = 5;
    frame.end_bound = FrameBound::CURRENT_ROW;
    frame.end_offset = 0;
    builder.add_window_func(WindowFunc::FIRST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // With an inverted frame (start > end), FIRST_VALUE should return NULL.
    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null())
            << "FIRST_VALUE with empty/inverted frame should be NULL, got: "
            << win_col(row, 3, 0).as_int32();
    }
}

TEST_F(QA_WindowFunction, LastValueInvertedFrameShouldReturnNull) {
    // Frame: ROWS BETWEEN 5 FOLLOWING AND 1 PRECEDING
    // For most rows, frame_start > frame_end -> empty frame.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_FOLLOWING;
    frame.start_offset = 5;
    frame.end_bound = FrameBound::N_PRECEDING;
    frame.end_offset = 1;
    builder.add_window_func(WindowFunc::LAST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // For row 0 (pos=0): start=resolve(N_FOLLOWING,5,0,3)=2, end=resolve(N_PRECEDING,1,0,3)=0
    // start(2) > end(0) -> empty -> LAST_VALUE should be NULL
    // Row 0 should be NULL; others depend on clamping but most are inverted too.
    EXPECT_TRUE(win_col(rows[0], 3, 0).is_null())
        << "LAST_VALUE with inverted frame should be NULL";
}

// ===========================================================================
// WIN_SUM / WIN_COUNT — empty frame verification (should work correctly)
// ===========================================================================

TEST_F(QA_WindowFunction, AggregateSumInvertedFrameReturnsNull) {
    // Frame: ROWS BETWEEN 5 FOLLOWING AND 5 PRECEDING
    // This is inverted for ALL rows -> empty frame for all.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_FOLLOWING;
    frame.start_offset = 5;
    frame.end_bound = FrameBound::N_PRECEDING;
    frame.end_offset = 5;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);
    builder.add_window_func(WindowFunc::WIN_COUNT, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null()) << "SUM on empty frame should be NULL";
        EXPECT_EQ(win_col(row, 3, 1).as_int64(), 0) << "COUNT on empty frame should be 0";
    }
}

// ===========================================================================
// LAG / LEAD — offset edge cases
// ===========================================================================

TEST_F(QA_WindowFunction, LagOffset0ReturnsCurrent) {
    // LAG(salary, 0) should return the current row's value.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"), {}, 0);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int32(), row.values[2].as_int32())
            << "LAG(col, 0) should return current row value";
    }
}

TEST_F(QA_WindowFunction, LeadOffset0ReturnsCurrent) {
    // LEAD(salary, 0) should return the current row's value.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LEAD, col_ref("emp", "salary"), {}, 0);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int32(), row.values[2].as_int32())
            << "LEAD(col, 0) should return current row value";
    }
}

TEST_F(QA_WindowFunction, LagVeryLargeOffsetReturnsDefault) {
    // LAG(salary, 9999) should return default for all rows.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(
        WindowFunc::LAG, col_ref("emp", "salary"), {}, 9999, Value(int32_t(-999)));

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int32(), -999)
            << "LAG with huge offset should return default";
    }
}

TEST_F(QA_WindowFunction, LeadVeryLargeOffsetReturnsDefault) {
    // LEAD(salary, 9999) should return default (NULL) for all rows.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LEAD, col_ref("emp", "salary"), {}, 9999);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null())
            << "LEAD with huge offset should return NULL default";
    }
}

// ===========================================================================
// NTILE — boundary cases
// ===========================================================================

TEST_F(QA_WindowFunction, NtileBuckets1AllInSameBucket) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::NTILE, nullptr, {}, 1, Value::make_null(), 1);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1)
            << "NTILE(1) should put all rows in bucket 1";
    }
}

TEST_F(QA_WindowFunction, NtileBucketsEqualToRows) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::NTILE, nullptr, {}, 1, Value::make_null(), 3);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    std::vector<int64_t> buckets;
    for (auto& row : rows) {
        buckets.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(buckets.begin(), buckets.end());
    EXPECT_EQ(buckets, (std::vector<int64_t>{1, 2, 3}))
        << "NTILE(N) with N=rows should give one row per bucket";
}

TEST_F(QA_WindowFunction, NtileBucketsMoreThanRows) {
    // NTILE(10) with only 3 rows: each row gets a unique bucket (1, 2, 3).
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::NTILE, nullptr, {}, 1, Value::make_null(), 10);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    std::vector<int64_t> buckets;
    for (auto& row : rows) {
        buckets.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(buckets.begin(), buckets.end());
    EXPECT_EQ(buckets, (std::vector<int64_t>{1, 2, 3}))
        << "NTILE(10) with 3 rows should give buckets 1,2,3";
}

TEST_F(QA_WindowFunction, NtileSingleRow) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::NTILE, nullptr, {}, 1, Value::make_null(), 5);

    auto schema = employee_schema();
    std::vector<Tuple> data = {make_emp("eng", "alice", 100)};
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(win_col(rows[0], 3, 0).as_int64(), 1);
}

// ===========================================================================
// RANK / DENSE_RANK — all ties
// ===========================================================================

TEST_F(QA_WindowFunction, RankAllTiesGiveRank1) {
    // All rows have the same salary -> all get RANK=1.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 100),
        make_emp("eng", "charlie", 100),
        make_emp("eng", "dave", 100),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 4u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1) << "All ties should have RANK=1";
    }
}

TEST_F(QA_WindowFunction, DenseRankAllTiesGiveRank1) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::DENSE_RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 100),
        make_emp("eng", "charlie", 100),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1) << "All ties should have DENSE_RANK=1";
    }
}

TEST_F(QA_WindowFunction, RankNoOrderByAllPeers) {
    // No ORDER BY -> all rows are peers -> RANK=1 for all.
    BoundStatement bound;
    WinTestBuilder builder;
    // No add_order_by call.
    builder.add_window_func(WindowFunc::RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1) << "No ORDER BY -> all peers -> RANK=1";
    }
}

TEST_F(QA_WindowFunction, DenseRankNoOrderByAllPeers) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_window_func(WindowFunc::DENSE_RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1);
    }
}

// ===========================================================================
// NULL partition keys — NULLs should group together
// ===========================================================================

TEST_F(QA_WindowFunction, NullPartitionKeysGroupTogether) {
    // Rows with NULL dept should all land in the same partition.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_COUNT, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        Tuple{{Value::make_null(), Value("alice"), Value(int32_t(100))}, {}},
        Tuple{{Value::make_null(), Value("bob"), Value(int32_t(120))}, {}},
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // NULL partition should have 2 rows -> COUNT=2.
    // eng partition should have 1 row -> COUNT=1.
    for (auto& row : rows) {
        if (row.values[0].is_null()) {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 2)
                << "NULL partition should group together";
        } else {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1);
        }
    }
}

// ===========================================================================
// DESC sort direction
// ===========================================================================

TEST_F(QA_WindowFunction, DescOrderByAffectsRanking) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"), SortDirection::DESC);
    builder.add_window_func(WindowFunc::ROW_NUMBER);
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"));

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // DESC order: bob(120)=rn1, charlie(110)=rn2, alice(100)=rn3
    // LAG: bob=NULL, charlie=120, alice=110
    for (auto& row : rows) {
        if (row.values[2].as_int32() == 120) {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1); // ROW_NUMBER
            EXPECT_TRUE(win_col(row, 3, 1).is_null());    // LAG = NULL (first in desc)
        }
        if (row.values[2].as_int32() == 110) {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 2);
            EXPECT_EQ(win_col(row, 3, 1).as_int32(), 120); // LAG = bob's salary
        }
        if (row.values[2].as_int32() == 100) {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 3);
            EXPECT_EQ(win_col(row, 3, 1).as_int32(), 110); // LAG = charlie's salary
        }
    }
}

// ===========================================================================
// Large dataset stress test
// ===========================================================================

TEST_F(QA_WindowFunction, StressTest1000Rows) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "dept"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    WindowFrameSpec full_frame;
    full_frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    full_frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), full_frame);
    builder.add_window_func(WindowFunc::WIN_COUNT, col_ref("emp", "salary"), full_frame);

    auto schema = employee_schema();
    std::vector<Tuple> data;
    // 10 departments, 100 employees each.
    for (int dept = 0; dept < 10; ++dept) {
        std::string dept_name = "dept_" + std::to_string(dept);
        for (int emp = 0; emp < 100; ++emp) {
            std::string name = "emp_" + std::to_string(dept * 100 + emp);
            data.push_back(make_emp(dept_name, name, static_cast<int32_t>(1000 + emp)));
        }
    }

    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 1000u);

    // Verify each partition has COUNT=100 and correct SUM.
    // SUM for each dept: sum(1000..1099) = 100*1000 + sum(0..99) = 100000 + 4950 = 104950
    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 2).as_int64(), 100) << "Each dept should have COUNT=100";
        EXPECT_EQ(win_col(row, 3, 1).as_int64(), 104950) << "Each dept SUM should be 104950";

        int64_t rn = win_col(row, 3, 0).as_int64();
        EXPECT_GE(rn, 1);
        EXPECT_LE(rn, 100);
    }
}

// ===========================================================================
// Many partitions with single row each
// ===========================================================================

TEST_F(QA_WindowFunction, ManySmallPartitions) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_partition_by(col_ref("emp", "name"));
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::LEAD, col_ref("emp", "salary"));

    auto schema = employee_schema();
    std::vector<Tuple> data;
    for (int i = 0; i < 50; ++i) {
        data.push_back(make_emp("dept", "person_" + std::to_string(i), static_cast<int32_t>(i)));
    }
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 50u);

    // Each person is in their own partition -> ROW_NUMBER=1, LAG=NULL, LEAD=NULL.
    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 1);
        EXPECT_TRUE(win_col(row, 3, 1).is_null()) << "LAG in single-row partition should be NULL";
        EXPECT_TRUE(win_col(row, 3, 2).is_null()) << "LEAD in single-row partition should be NULL";
    }
}

// ===========================================================================
// Window aggregate — all NULLs in frame
// ===========================================================================

TEST_F(QA_WindowFunction, WinMinAllNullsReturnsNull) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_MIN, col_ref("emp", "salary"), frame);
    builder.add_window_func(WindowFunc::WIN_MAX, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp_null_salary("eng", "bob"),
        make_emp_null_salary("eng", "charlie"),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null()) << "MIN of all NULLs should be NULL";
        EXPECT_TRUE(win_col(row, 3, 1).is_null()) << "MAX of all NULLs should be NULL";
    }
}

TEST_F(QA_WindowFunction, WinAvgAllNullsReturnsNull) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_AVG, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp_null_salary("eng", "bob"),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null()) << "AVG of all NULLs should be NULL";
    }
}

TEST_F(QA_WindowFunction, WinCountAllNullsReturnsZero) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_COUNT, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp_null_salary("eng", "bob"),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 0) << "COUNT of all NULLs should be 0";
    }
}

// ===========================================================================
// FIRST_VALUE / LAST_VALUE — NULL at boundary
// ===========================================================================

TEST_F(QA_WindowFunction, FirstValueNullAtStart) {
    // FIRST_VALUE should return NULL if the first row in frame has NULL.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name")); // alphabetical: alice, bob, charlie

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::FIRST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // FIRST_VALUE = alice's salary = NULL for all rows.
    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null())
            << "FIRST_VALUE should be NULL when first row has NULL salary";
    }
}

TEST_F(QA_WindowFunction, LastValueNullAtEnd) {
    // LAST_VALUE should return NULL if the last row in frame has NULL.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name")); // alphabetical: alice, bob, charlie

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::LAST_VALUE, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp_null_salary("eng", "charlie"),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // LAST_VALUE = charlie's salary = NULL for all rows.
    for (auto& row : rows) {
        EXPECT_TRUE(win_col(row, 3, 0).is_null())
            << "LAST_VALUE should be NULL when last row has NULL salary";
    }
}

// ===========================================================================
// Re-open after close
// ===========================================================================

TEST_F(QA_WindowFunction, ReopenAfterCloseProducesSameResults) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    // First run.
    auto rows1 = run(*win);
    ASSERT_EQ(rows1.size(), 2u);

    // Second run (re-open).
    auto rows2 = run(*win);
    ASSERT_EQ(rows2.size(), 2u);

    // Results should be identical.
    for (size_t i = 0; i < rows1.size(); ++i) {
        EXPECT_EQ(rows1[i].values[1].as_string(), rows2[i].values[1].as_string());
        EXPECT_EQ(win_col(rows1[i], 3, 0).as_int64(), win_col(rows2[i], 3, 0).as_int64());
    }
}

// ===========================================================================
// Multiple window functions with different frames
// ===========================================================================

TEST_F(QA_WindowFunction, MultipleWindowFunctionsDifferentFrames) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    // Function 1: SUM over entire partition.
    WindowFrameSpec full_frame;
    full_frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    full_frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), full_frame);

    // Function 2: SUM over CURRENT ROW only.
    WindowFrameSpec single_frame;
    single_frame.start_bound = FrameBound::CURRENT_ROW;
    single_frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), single_frame);

    // Function 3: ROW_NUMBER.
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // Full frame SUM = 100+110+120 = 330 for all.
    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 330);
    }

    // Single row SUM = own salary.
    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 1).as_int64(),
                  static_cast<int64_t>(row.values[2].as_int32()));
    }

    // ROW_NUMBER 1-3.
    std::vector<int64_t> rns;
    for (auto& row : rows) {
        rns.push_back(win_col(row, 3, 2).as_int64());
    }
    std::sort(rns.begin(), rns.end());
    EXPECT_EQ(rns, (std::vector<int64_t>{1, 2, 3}));
}

// ===========================================================================
// Window SUM sliding window: N PRECEDING to CURRENT ROW
// ===========================================================================

TEST_F(QA_WindowFunction, WinSumSlidingWindow2Preceding) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_PRECEDING;
    frame.start_offset = 2;
    frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    // Sorted: 80, 90, 100, 110, 120
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
        make_emp("eng", "dave", 90),
        make_emp("eng", "frank", 80),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 5u);

    // Sorted: frank(80), dave(90), alice(100), charlie(110), bob(120)
    // SUM(salary) ROWS BETWEEN 2 PRECEDING AND CURRENT ROW:
    // frank: 80
    // dave: 80+90 = 170
    // alice: 80+90+100 = 270
    // charlie: 90+100+110 = 300
    // bob: 100+110+120 = 330
    std::vector<int64_t> sums;
    for (auto& row : rows) {
        sums.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(sums.begin(), sums.end());
    EXPECT_EQ(sums, (std::vector<int64_t>{80, 170, 270, 300, 330}));
}

// ===========================================================================
// RANK with NULL order-by values
// ===========================================================================

TEST_F(QA_WindowFunction, RankWithNullOrderByValues) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp_null_salary("eng", "bob"),
        make_emp_null_salary("eng", "charlie"),
        make_emp("eng", "dave", 90),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 4u);

    // NULLs sort together (either first or last depending on direction).
    // With ASC, NULLs sort as highest (DESC behavior in sort comparator),
    // so sorted: dave(90), alice(100), bob(NULL), charlie(NULL)
    // OR NULLs sort last: dave(90)=1, alice(100)=2, bob(NULL)=3, charlie(NULL)=3
    // Either way, all rows should have valid RANK values.
    for (auto& row : rows) {
        int64_t rank = win_col(row, 3, 0).as_int64();
        EXPECT_GE(rank, 1);
        EXPECT_LE(rank, 4);
    }

    // The two NULL-salary rows should have the same rank (they're peers).
    std::vector<int64_t> null_ranks;
    for (auto& row : rows) {
        if (row.values[2].is_null()) {
            null_ranks.push_back(win_col(row, 3, 0).as_int64());
        }
    }
    ASSERT_EQ(null_ranks.size(), 2u);
    EXPECT_EQ(null_ranks[0], null_ranks[1]) << "NULL-salary rows should be peers with same RANK";
}

// ===========================================================================
// LAG / LEAD with NULL argument values
// ===========================================================================

TEST_F(QA_WindowFunction, LagReturnsNullFromNullRow) {
    // LAG should return the actual NULL value from the previous row.
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name"));
    builder.add_window_func(WindowFunc::LAG, col_ref("emp", "salary"));

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp_null_salary("eng", "alice"), // pos 0
        make_emp("eng", "bob", 120),          // pos 1 -> LAG=alice's NULL salary
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 2u);

    // bob: LAG(salary, 1) = alice's salary = NULL.
    for (auto& row : rows) {
        if (row.values[1].as_string() == "bob") {
            EXPECT_TRUE(win_col(row, 3, 0).is_null())
                << "LAG should return NULL from previous row's NULL value";
        }
    }
}

// ===========================================================================
// Window SUM with mix of NULLs and non-NULLs in sliding frame
// ===========================================================================

TEST_F(QA_WindowFunction, WinSumSlidingFrameWithNulls) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_PRECEDING;
    frame.start_offset = 1;
    frame.end_bound = FrameBound::CURRENT_ROW;
    builder.add_window_func(WindowFunc::WIN_SUM, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    // Sorted by name: alice, bob, charlie
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp_null_salary("eng", "bob"),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // alice: frame=[alice] -> SUM=100
    // bob: frame=[alice, bob] -> SUM=100 (bob NULL skipped)
    // charlie: frame=[bob, charlie] -> SUM=110 (bob NULL skipped)
    for (auto& row : rows) {
        if (row.values[1].as_string() == "alice") {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 100);
        }
        if (row.values[1].as_string() == "bob") {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 100);
        }
        if (row.values[1].as_string() == "charlie") {
            EXPECT_EQ(win_col(row, 3, 0).as_int64(), 110);
        }
    }
}

// ===========================================================================
// Window COUNT(*) — no arg, counts all rows including NULLs
// ===========================================================================

TEST_F(QA_WindowFunction, WinCountStarCountsAllRows) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "name"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::UNBOUNDED_PRECEDING;
    frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
    // COUNT(*) — no arg expression.
    builder.add_window_func(WindowFunc::WIN_COUNT, nullptr, frame);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp_null_salary("eng", "bob"),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // COUNT(*) should count all rows including NULLs = 3.
    for (auto& row : rows) {
        EXPECT_EQ(win_col(row, 3, 0).as_int64(), 3) << "COUNT(*) should count all rows";
    }
}

// ===========================================================================
// DENSE_RANK correctness with multiple groups
// ===========================================================================

TEST_F(QA_WindowFunction, DenseRankMultipleGroups) {
    // Values: 10, 10, 20, 20, 30 -> DENSE_RANK: 1,1,2,2,3
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::DENSE_RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "a", 10),
        make_emp("eng", "b", 10),
        make_emp("eng", "c", 20),
        make_emp("eng", "d", 20),
        make_emp("eng", "e", 30),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 5u);

    std::vector<int64_t> ranks;
    for (auto& row : rows) {
        ranks.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(ranks.begin(), ranks.end());
    EXPECT_EQ(ranks, (std::vector<int64_t>{1, 1, 2, 2, 3}));
}

// ===========================================================================
// RANK correctness with gaps
// ===========================================================================

TEST_F(QA_WindowFunction, RankWithGaps) {
    // Values: 10, 10, 20, 30, 30 -> RANK: 1,1,3,4,4
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::RANK);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "a", 10),
        make_emp("eng", "b", 10),
        make_emp("eng", "c", 20),
        make_emp("eng", "d", 30),
        make_emp("eng", "e", 30),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 5u);

    std::vector<int64_t> ranks;
    for (auto& row : rows) {
        ranks.push_back(win_col(row, 3, 0).as_int64());
    }
    std::sort(ranks.begin(), ranks.end());
    EXPECT_EQ(ranks, (std::vector<int64_t>{1, 1, 3, 4, 4}));
}

// ===========================================================================
// Window MIN/MAX with sliding frame
// ===========================================================================

TEST_F(QA_WindowFunction, WinMinMaxSlidingFrame) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));

    WindowFrameSpec frame;
    frame.start_bound = FrameBound::N_PRECEDING;
    frame.start_offset = 1;
    frame.end_bound = FrameBound::N_FOLLOWING;
    frame.end_offset = 1;
    builder.add_window_func(WindowFunc::WIN_MIN, col_ref("emp", "salary"), frame);
    builder.add_window_func(WindowFunc::WIN_MAX, col_ref("emp", "salary"), frame);

    auto schema = employee_schema();
    // Sorted: 10, 20, 30
    std::vector<Tuple> data = {
        make_emp("eng", "a", 10),
        make_emp("eng", "b", 20),
        make_emp("eng", "c", 30),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto rows = run(*win);
    ASSERT_EQ(rows.size(), 3u);

    // row 0 (10): frame=[10,20] -> MIN=10, MAX=20
    // row 1 (20): frame=[10,20,30] -> MIN=10, MAX=30
    // row 2 (30): frame=[20,30] -> MIN=20, MAX=30
    std::vector<std::pair<int32_t, int32_t>> min_max;
    for (auto& row : rows) {
        min_max.emplace_back(win_col(row, 3, 0).as_int32(), win_col(row, 3, 1).as_int32());
    }
    std::sort(min_max.begin(), min_max.end());
    EXPECT_EQ(min_max[0], std::make_pair(10, 20));
    EXPECT_EQ(min_max[1], std::make_pair(10, 30));
    EXPECT_EQ(min_max[2], std::make_pair(20, 30));
}

// ===========================================================================
// Open, partial drain, close — no crash
// ===========================================================================

TEST_F(QA_WindowFunction, PartialDrainThenClose) {
    BoundStatement bound;
    WinTestBuilder builder;
    builder.add_order_by(col_ref("emp", "salary"));
    builder.add_window_func(WindowFunc::ROW_NUMBER);

    auto schema = employee_schema();
    std::vector<Tuple> data = {
        make_emp("eng", "alice", 100),
        make_emp("eng", "bob", 120),
        make_emp("eng", "charlie", 110),
    };
    auto child = std::make_unique<VectorIterator>(schema, std::move(data));
    auto win = builder.build(std::move(child), bound, schema);

    auto open_result = win->open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    // Read only one row, then close.
    auto row1 = win->next();
    ASSERT_TRUE(row1.has_value()) << row1.error().message;
    ASSERT_TRUE(row1->has_value());

    // Close without draining all rows — should not crash.
    win->close();
}

} // namespace
} // namespace giodb
