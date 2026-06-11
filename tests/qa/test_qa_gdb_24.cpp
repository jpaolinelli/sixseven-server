/// @file test_qa_gdb_24.cpp
/// QA adversarial tests for GDB-24: Aggregation Operators.
///
/// Tests target: HashAggregateOperator (direct), end-to-end SQL aggregation
/// via QueryEngine, and edge cases in the aggregation pipeline.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/filter.h"
#include "sixseven/executor/hash_aggregate.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_helpers.h"
#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ===========================================================================
// Direct HashAggregateOperator fixture
// ===========================================================================

class QA_HashAggregate : public ::testing::Test {
protected:
    static OutputSchema int_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::INT32, true, 1},
        });
    }

    static OutputSchema int64_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::INT64, true, 1},
        });
    }

    static OutputSchema float_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::FLOAT64, true, 1},
        });
    }

    static OutputSchema string_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::STRING, true, 1},
        });
    }

    static OutputSchema multi_key_schema() {
        return OutputSchema({
            {"t", "k1", TypeId::STRING, true, 1},
            {"t", "k2", TypeId::INT32, true, 1},
            {"t", "val", TypeId::INT32, false, 1},
        });
    }

    static ExprPtr col_ref(const std::string& table, const std::string& column) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = table;
        e->column = column;
        return e;
    }

    static ExprPtr lit_int(int64_t v) {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::INTEGER;
        e->value = std::to_string(v);
        return e;
    }

    static ExprPtr binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto e = std::make_unique<BinaryExpr>();
        e->op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    struct AggTestBuilder {
        std::vector<const Expr*> group_by_ptrs;
        std::vector<AggregateDescriptor> descriptors;
        std::vector<OutputColumn> out_cols;
        std::vector<ExprPtr> owned;

        void add_group_by(ExprPtr expr, const OutputColumn& col) {
            group_by_ptrs.push_back(expr.get());
            out_cols.push_back(col);
            owned.push_back(std::move(expr));
        }

        void add_agg(AggFunc func,
                     ExprPtr arg = nullptr,
                     const std::string& sep = "",
                     TypeId out_type = TypeId::INT64,
                     bool nullable = true) {
            AggregateDescriptor desc;
            desc.func = func;
            desc.separator = sep;

            std::string col_name = "__agg_" + std::to_string(descriptors.size());

            if (arg) {
                desc.arg = arg.get();
                owned.push_back(std::move(arg));
            }

            descriptors.push_back(std::move(desc));
            out_cols.push_back({"", col_name, out_type, nullable, 0});
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

    static std::vector<Tuple> run(Iterator& iter) { return collect_all(iter); }

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
// End-to-end SQL fixture
// ===========================================================================

class QA_AggregateE2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb24";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

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
        EXPECT_TRUE(result.has_value()) << "SQL failed: " << sql << "\n" << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    void setup_employees() {
        exec_ok("CREATE TABLE emp (id INT, dept VARCHAR, salary INT)");
        exec_ok("INSERT INTO emp VALUES (1, 'eng', 100)");
        exec_ok("INSERT INTO emp VALUES (2, 'eng', 120)");
        exec_ok("INSERT INTO emp VALUES (3, 'eng', 110)");
        exec_ok("INSERT INTO emp VALUES (4, 'sales', 90)");
        exec_ok("INSERT INTO emp VALUES (5, 'sales', 95)");
        exec_ok("INSERT INTO emp VALUES (6, 'hr', 80)");
    }

    void setup_with_nulls() {
        exec_ok("CREATE TABLE data (id INT, grp VARCHAR, val INT NULL)");
        exec_ok("INSERT INTO data VALUES (1, 'a', 10)");
        exec_ok("INSERT INTO data VALUES (2, 'a', NULL)");
        exec_ok("INSERT INTO data VALUES (3, 'a', 30)");
        exec_ok("INSERT INTO data VALUES (4, 'b', NULL)");
        exec_ok("INSERT INTO data VALUES (5, 'b', NULL)");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ===========================================================================
// BOUNDARY VALUE TESTS — Direct HashAggregateOperator
// ===========================================================================

// SUM with all negative values
TEST_F(QA_HashAggregate, SumAllNegative) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(-10))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(-20))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(-30))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), -60);
}

// SUM with mixed positive and negative values cancelling out to zero
TEST_F(QA_HashAggregate, SumCancelsToZero) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(50))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(-50))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 0);
}

// SUM with single row
TEST_F(QA_HashAggregate, SumSingleRow) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(42))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 42);
}

// SUM with float values
TEST_F(QA_HashAggregate, SumFloat) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(1.5)}, {}},
        Tuple{{Value(std::string("x")), Value(2.5)}, {}},
        Tuple{{Value(std::string("x")), Value(3.0)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"), "", TypeId::FLOAT64);

    auto child = std::make_unique<VectorIterator>(float_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].values[1].as_float64(), 7.0);
}

// AVG with single row
TEST_F(QA_HashAggregate, AvgSingleRow) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(42))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::AVG, col_ref("t", "val"), "", TypeId::FLOAT64);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].values[1].as_float64(), 42.0);
}

// AVG with float values
TEST_F(QA_HashAggregate, AvgFloat) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(1.0)}, {}},
        Tuple{{Value(std::string("x")), Value(2.0)}, {}},
        Tuple{{Value(std::string("x")), Value(3.0)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::AVG, col_ref("t", "val"), "", TypeId::FLOAT64);

    auto child = std::make_unique<VectorIterator>(float_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].values[1].as_float64(), 2.0);
}

// MIN/MAX with all identical values
TEST_F(QA_HashAggregate, MinMaxAllIdentical) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(42))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(42))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(42))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::INT32);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::INT32);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int32(), 42); // MIN
    EXPECT_EQ(rows[0].values[2].as_int32(), 42); // MAX
}

// MIN/MAX with negative values
TEST_F(QA_HashAggregate, MinMaxNegative) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(-5))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(-100))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(-1))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::INT32);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::INT32);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int32(), -100); // MIN
    EXPECT_EQ(rows[0].values[2].as_int32(), -1);   // MAX
}

// MIN/MAX with single row
TEST_F(QA_HashAggregate, MinMaxSingleRow) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(7))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::INT32);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::INT32);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int32(), 7); // MIN
    EXPECT_EQ(rows[0].values[2].as_int32(), 7); // MAX
}

// MIN with string values
TEST_F(QA_HashAggregate, MinMaxStringValues) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(std::string("banana"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("apple"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("cherry"))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::STRING);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::STRING);

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_string(), "apple");  // MIN
    EXPECT_EQ(rows[0].values[2].as_string(), "cherry"); // MAX
}

// ===========================================================================
// NULL HANDLING TESTS
// ===========================================================================

// COUNT(DISTINCT) with all identical values
TEST_F(QA_HashAggregate, CountDistinctAllIdentical) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(5))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(5))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(5))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(5))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 1);
}

// COUNT(DISTINCT) with all NULLs
TEST_F(QA_HashAggregate, CountDistinctAllNulls) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 0); // NULLs excluded from DISTINCT
}

// COUNT(DISTINCT) with string values
TEST_F(QA_HashAggregate, CountDistinctStrings) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(std::string("a"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("b"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("a"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("c"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("b"))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 3); // a, b, c
}

// Multiple aggregates with ALL NULLs — verify correct NULL results
TEST_F(QA_HashAggregate, AllAggregatesAllNulls) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::COUNT, col_ref("t", "val"));
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));
    builder.add_agg(AggFunc::AVG, col_ref("t", "val"), "", TypeId::FLOAT64);
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::INT32);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::INT32);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(rows[0].values[1].as_int64(), 2); // COUNT(*) counts all rows
    EXPECT_EQ(rows[0].values[2].as_int64(), 0); // COUNT(col) = 0 (all NULL)
    EXPECT_TRUE(rows[0].values[3].is_null());   // SUM = NULL
    EXPECT_TRUE(rows[0].values[4].is_null());   // AVG = NULL
    EXPECT_TRUE(rows[0].values[5].is_null());   // MIN = NULL
    EXPECT_TRUE(rows[0].values[6].is_null());   // MAX = NULL
}

// Global aggregation (no GROUP BY) with empty input
TEST_F(QA_HashAggregate, GlobalAggEmptyInput) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::COUNT, col_ref("t", "val"));
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));
    builder.add_agg(AggFunc::AVG, col_ref("t", "val"), "", TypeId::FLOAT64);
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::INT32);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::INT32);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::vector<Tuple>{});
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    // Per SQL standard, global aggregation on empty table still returns one row.
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(rows[0].values[0].as_int64(), 0); // COUNT(*) = 0
    EXPECT_EQ(rows[0].values[1].as_int64(), 0); // COUNT(col) = 0
    EXPECT_TRUE(rows[0].values[2].is_null());   // SUM = NULL
    EXPECT_TRUE(rows[0].values[3].is_null());   // AVG = NULL
    EXPECT_TRUE(rows[0].values[4].is_null());   // MIN = NULL
    EXPECT_TRUE(rows[0].values[5].is_null());   // MAX = NULL
}

// Global aggregation with all NULLs
TEST_F(QA_HashAggregate, GlobalAggAllNulls) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));
    builder.add_agg(AggFunc::AVG, col_ref("t", "val"), "", TypeId::FLOAT64);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(rows[0].values[0].as_int64(), 3); // COUNT(*) counts all rows
    EXPECT_TRUE(rows[0].values[1].is_null());   // SUM = NULL (all skipped)
    EXPECT_TRUE(rows[0].values[2].is_null());   // AVG = NULL
}

// ===========================================================================
// STRING_AGG EDGE CASES
// ===========================================================================

// STRING_AGG with empty strings
TEST_F(QA_HashAggregate, StringAggEmptyStrings) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(std::string(""))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string(""))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string(""))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});

    AggregateDescriptor desc;
    desc.func = AggFunc::STRING_AGG;
    auto arg = col_ref("t", "val");
    desc.arg = arg.get();
    desc.separator = ",";
    builder.owned.push_back(std::move(arg));
    builder.descriptors.push_back(std::move(desc));
    builder.out_cols.push_back({"", "__agg_0", TypeId::STRING, true, 0});

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    // Three empty strings joined by comma = ",,"
    EXPECT_EQ(rows[0].values[1].as_string(), ",,");
}

// STRING_AGG with empty separator
TEST_F(QA_HashAggregate, StringAggEmptySeparator) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(std::string("a"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("b"))}, {}},
        Tuple{{Value(std::string("x")), Value(std::string("c"))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});

    AggregateDescriptor desc;
    desc.func = AggFunc::STRING_AGG;
    auto arg = col_ref("t", "val");
    desc.arg = arg.get();
    desc.separator = ""; // Empty separator
    builder.owned.push_back(std::move(arg));
    builder.descriptors.push_back(std::move(desc));
    builder.out_cols.push_back({"", "__agg_0", TypeId::STRING, true, 0});

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_string(), "abc");
}

// STRING_AGG with single value
TEST_F(QA_HashAggregate, StringAggSingleValue) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(std::string("only"))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});

    AggregateDescriptor desc;
    desc.func = AggFunc::STRING_AGG;
    auto arg = col_ref("t", "val");
    desc.arg = arg.get();
    desc.separator = ",";
    builder.owned.push_back(std::move(arg));
    builder.descriptors.push_back(std::move(desc));
    builder.out_cols.push_back({"", "__agg_0", TypeId::STRING, true, 0});

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_string(), "only"); // No separator for single value
}

// STRING_AGG with all NULLs
TEST_F(QA_HashAggregate, StringAggAllNulls) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
        Tuple{{Value(std::string("x")), Value::make_null()}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});

    AggregateDescriptor desc;
    desc.func = AggFunc::STRING_AGG;
    auto arg = col_ref("t", "val");
    desc.arg = arg.get();
    desc.separator = ",";
    builder.owned.push_back(std::move(arg));
    builder.descriptors.push_back(std::move(desc));
    builder.out_cols.push_back({"", "__agg_0", TypeId::STRING, true, 0});

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].values[1].is_null()); // STRING_AGG over all NULLs = NULL
}

// ===========================================================================
// MULTI-KEY GROUP BY EDGE CASES
// ===========================================================================

// GROUP BY with NULLs in different key columns
TEST_F(QA_HashAggregate, MultiKeyGroupByWithNulls) {
    std::vector<Tuple> data = {
        // key1=NULL, key2=1
        Tuple{{Value::make_null(), Value(int32_t(1)), Value(int32_t(10))}, {}},
        // key1=NULL, key2=1 (same group)
        Tuple{{Value::make_null(), Value(int32_t(1)), Value(int32_t(20))}, {}},
        // key1="a", key2=NULL
        Tuple{{Value(std::string("a")), Value::make_null(), Value(int32_t(30))}, {}},
        // key1=NULL, key2=NULL
        Tuple{{Value::make_null(), Value::make_null(), Value(int32_t(40))}, {}},
        // key1=NULL, key2=NULL (same group)
        Tuple{{Value::make_null(), Value::make_null(), Value(int32_t(50))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "k1"), {"t", "k1", TypeId::STRING, true, 1});
    builder.add_group_by(col_ref("t", "k2"), {"t", "k2", TypeId::INT32, true, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(multi_key_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    // 3 groups: (NULL,1), (a,NULL), (NULL,NULL)
    ASSERT_EQ(rows.size(), 3u);

    // Find each group
    const Tuple* null_1 = nullptr;
    const Tuple* a_null = nullptr;
    const Tuple* null_null = nullptr;

    for (auto& row : rows) {
        bool k1_null = row.values[0].is_null();
        bool k2_null = row.values[1].is_null();
        if (k1_null && !k2_null && row.values[1].as_int32() == 1)
            null_1 = &row;
        else if (!k1_null && k2_null)
            a_null = &row;
        else if (k1_null && k2_null)
            null_null = &row;
    }

    ASSERT_NE(null_1, nullptr);
    ASSERT_NE(a_null, nullptr);
    ASSERT_NE(null_null, nullptr);

    EXPECT_EQ(null_1->values[2].as_int64(), 30); // SUM(10+20)
    EXPECT_EQ(null_1->values[3].as_int64(), 2);  // COUNT(*)

    EXPECT_EQ(a_null->values[2].as_int64(), 30); // SUM(30)
    EXPECT_EQ(a_null->values[3].as_int64(), 1);  // COUNT(*)

    EXPECT_EQ(null_null->values[2].as_int64(), 90); // SUM(40+50)
    EXPECT_EQ(null_null->values[3].as_int64(), 2);  // COUNT(*)
}

// ===========================================================================
// ITERATOR PROTOCOL EDGE CASES
// ===========================================================================

// Close without calling next
TEST_F(QA_HashAggregate, OpenCloseWithoutNext) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(1))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto open = agg->open();
    ASSERT_TRUE(open.has_value()) << open.error().message;
    agg->close(); // Should not crash or leak
}

// Multiple next() calls past end of stream
TEST_F(QA_HashAggregate, NextPastEnd) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(1))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto open = agg->open();
    ASSERT_TRUE(open.has_value());

    auto r1 = agg->next();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value()); // One group

    auto r2 = agg->next();
    ASSERT_TRUE(r2.has_value());
    ASSERT_FALSE(r2->has_value()); // End

    // Call next again past end — should still return empty, not crash
    auto r3 = agg->next();
    ASSERT_TRUE(r3.has_value());
    ASSERT_FALSE(r3->has_value());

    agg->close();
}

// ===========================================================================
// STRESS TESTS — Direct HashAggregateOperator
// ===========================================================================

// Many groups (1000)
TEST_F(QA_HashAggregate, StressManyGroups) {
    std::vector<Tuple> data;
    for (int i = 0; i < 1000; ++i) {
        data.push_back(Tuple{{Value(std::string("g" + std::to_string(i))), Value(int32_t(i))}, {}});
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1000u);

    // Each group has exactly 1 row
    for (auto& row : rows) {
        EXPECT_EQ(row.values[1].as_int64(), 1); // COUNT(*)
    }
}

// Many rows in single group (5000 rows)
TEST_F(QA_HashAggregate, StressManyRowsSingleGroup) {
    std::vector<Tuple> data;
    int64_t expected_sum = 0;
    for (int i = 0; i < 5000; ++i) {
        data.push_back(Tuple{{Value(std::string("single")), Value(int32_t(i))}, {}});
        expected_sum += i;
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));
    builder.add_agg(AggFunc::MIN, col_ref("t", "val"), "", TypeId::INT32);
    builder.add_agg(AggFunc::MAX, col_ref("t", "val"), "", TypeId::INT32);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 5000);         // COUNT(*)
    EXPECT_EQ(rows[0].values[2].as_int64(), expected_sum); // SUM
    EXPECT_EQ(rows[0].values[3].as_int32(), 0);            // MIN
    EXPECT_EQ(rows[0].values[4].as_int32(), 4999);         // MAX
}

// ===========================================================================
// END-TO-END SQL TESTS — QA_AggregateE2E
// ===========================================================================

// Verify COUNT(*) vs COUNT(col) NULL semantics via SQL
TEST_F(QA_AggregateE2E, CountStarVsCountColWithNulls) {
    setup_with_nulls();

    auto qr = exec_ok("SELECT grp, COUNT(*), COUNT(val) FROM data GROUP BY grp");
    ASSERT_EQ(qr.rows.size(), 2u);

    std::unordered_map<std::string, std::pair<int64_t, int64_t>> counts;
    for (auto& row : qr.rows) {
        counts[row[0].as_string()] = {row[1].as_int64(), row[2].as_int64()};
    }

    // Group 'a': 3 rows total, 2 non-NULL val (10, 30)
    EXPECT_EQ(counts["a"].first, 3);  // COUNT(*)
    EXPECT_EQ(counts["a"].second, 2); // COUNT(val)

    // Group 'b': 2 rows total, 0 non-NULL val
    EXPECT_EQ(counts["b"].first, 2);  // COUNT(*)
    EXPECT_EQ(counts["b"].second, 0); // COUNT(val)
}

// Verify SUM/AVG skip NULLs via SQL
TEST_F(QA_AggregateE2E, SumAvgSkipNullsE2E) {
    setup_with_nulls();

    auto qr = exec_ok("SELECT grp, SUM(val), AVG(val) FROM data GROUP BY grp");
    ASSERT_EQ(qr.rows.size(), 2u);

    for (auto& row : qr.rows) {
        if (row[0].as_string() == "a") {
            EXPECT_EQ(row[1].as_int64(), 40);            // SUM(10+30)
            EXPECT_DOUBLE_EQ(row[2].as_float64(), 20.0); // AVG(10,30) = 20
        } else {
            EXPECT_EQ(row[0].as_string(), "b");
            EXPECT_TRUE(row[1].is_null()); // SUM over all NULLs = NULL
            EXPECT_TRUE(row[2].is_null()); // AVG over all NULLs = NULL
        }
    }
}

// Verify MIN/MAX skip NULLs
TEST_F(QA_AggregateE2E, MinMaxSkipNullsE2E) {
    setup_with_nulls();

    auto qr = exec_ok("SELECT grp, MIN(val), MAX(val) FROM data GROUP BY grp");
    ASSERT_EQ(qr.rows.size(), 2u);

    for (auto& row : qr.rows) {
        if (row[0].as_string() == "a") {
            EXPECT_EQ(row[1].as_int32(), 10); // MIN(10,30)
            EXPECT_EQ(row[2].as_int32(), 30); // MAX(10,30)
        } else {
            EXPECT_TRUE(row[1].is_null()); // MIN over all NULLs = NULL
            EXPECT_TRUE(row[2].is_null()); // MAX over all NULLs = NULL
        }
    }
}

// COUNT(DISTINCT) end-to-end
TEST_F(QA_AggregateE2E, CountDistinctE2E) {
    exec_ok("CREATE TABLE scores (dept VARCHAR, score INT)");
    exec_ok("INSERT INTO scores VALUES ('eng', 100)");
    exec_ok("INSERT INTO scores VALUES ('eng', 100)");
    exec_ok("INSERT INTO scores VALUES ('eng', 200)");
    exec_ok("INSERT INTO scores VALUES ('eng', 200)");
    exec_ok("INSERT INTO scores VALUES ('eng', 300)");
    exec_ok("INSERT INTO scores VALUES ('sales', 50)");
    exec_ok("INSERT INTO scores VALUES ('sales', 50)");

    auto qr = exec_ok("SELECT dept, COUNT(DISTINCT score) FROM scores GROUP BY dept");
    ASSERT_EQ(qr.rows.size(), 2u);

    std::unordered_map<std::string, int64_t> counts;
    for (auto& row : qr.rows) {
        counts[row[0].as_string()] = row[1].as_int64();
    }

    EXPECT_EQ(counts["eng"], 3);   // 100, 200, 300
    EXPECT_EQ(counts["sales"], 1); // 50
}

// HAVING with various operators
TEST_F(QA_AggregateE2E, HavingWithGreaterEqual) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) >= 2");
    ASSERT_EQ(qr.rows.size(), 2u); // eng(3), sales(2) pass; hr(1) filtered

    for (auto& row : qr.rows) {
        EXPECT_GE(row[1].as_int64(), 2);
    }
}

TEST_F(QA_AggregateE2E, HavingWithEqual) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) = 3");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
}

TEST_F(QA_AggregateE2E, HavingFiltersAllGroups) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 100");
    EXPECT_EQ(qr.rows.size(), 0u); // No group has > 100 rows
}

// HAVING with SUM
TEST_F(QA_AggregateE2E, HavingWithSum) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, SUM(salary) FROM emp GROUP BY dept HAVING SUM(salary) > 100");
    // eng=330, sales=185, hr=80 → eng and sales pass
    ASSERT_EQ(qr.rows.size(), 2u);
}

// Multiple aggregates end-to-end
TEST_F(QA_AggregateE2E, MultipleAggregatesE2E) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary) "
                      "FROM emp GROUP BY dept");
    ASSERT_EQ(qr.rows.size(), 3u);

    for (auto& row : qr.rows) {
        if (row[0].as_string() == "eng") {
            EXPECT_EQ(row[1].as_int64(), 3);              // COUNT(*)
            EXPECT_EQ(row[2].as_int64(), 330);            // SUM
            EXPECT_DOUBLE_EQ(row[3].as_float64(), 110.0); // AVG
            EXPECT_EQ(row[4].as_int32(), 100);            // MIN
            EXPECT_EQ(row[5].as_int32(), 120);            // MAX
        } else if (row[0].as_string() == "sales") {
            EXPECT_EQ(row[1].as_int64(), 2);
            EXPECT_EQ(row[2].as_int64(), 185);
            EXPECT_DOUBLE_EQ(row[3].as_float64(), 92.5);
            EXPECT_EQ(row[4].as_int32(), 90);
            EXPECT_EQ(row[5].as_int32(), 95);
        } else {
            EXPECT_EQ(row[0].as_string(), "hr");
            EXPECT_EQ(row[1].as_int64(), 1);
            EXPECT_EQ(row[2].as_int64(), 80);
            EXPECT_DOUBLE_EQ(row[3].as_float64(), 80.0);
            EXPECT_EQ(row[4].as_int32(), 80);
            EXPECT_EQ(row[5].as_int32(), 80);
        }
    }
}

// GROUP BY with ORDER BY
TEST_F(QA_AggregateE2E, GroupByWithOrderBy) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM emp GROUP BY dept ORDER BY dept");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Alphabetical order: eng, hr, sales
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[1][0].as_string(), "hr");
    EXPECT_EQ(qr.rows[2][0].as_string(), "sales");
}

// GROUP BY with LIMIT
TEST_F(QA_AggregateE2E, GroupByWithLimit) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM emp GROUP BY dept LIMIT 2");
    EXPECT_EQ(qr.rows.size(), 2u); // Only 2 of the 3 groups
}

// Global aggregation on empty table
TEST_F(QA_AggregateE2E, GlobalAggEmptyTableE2E) {
    exec_ok("CREATE TABLE empty_t (id INT, val INT)");

    auto qr = exec_ok("SELECT COUNT(*), SUM(val), AVG(val), MIN(val), MAX(val) FROM empty_t");
    ASSERT_EQ(qr.rows.size(), 1u);

    EXPECT_EQ(qr.rows[0][0].as_int64(), 0); // COUNT(*) = 0
    EXPECT_TRUE(qr.rows[0][1].is_null());   // SUM = NULL
    EXPECT_TRUE(qr.rows[0][2].is_null());   // AVG = NULL
    EXPECT_TRUE(qr.rows[0][3].is_null());   // MIN = NULL
    EXPECT_TRUE(qr.rows[0][4].is_null());   // MAX = NULL
}

// GROUP BY with empty table returns no rows
TEST_F(QA_AggregateE2E, GroupByEmptyTableE2E) {
    exec_ok("CREATE TABLE empty_t (id INT, dept VARCHAR)");

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM empty_t GROUP BY dept");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ===========================================================================
// ERROR PATH TESTS
// ===========================================================================

// SELECT * with GROUP BY should error
TEST_F(QA_AggregateE2E, SelectStarWithGroupByErrors) {
    setup_employees();
    exec_error("SELECT * FROM emp GROUP BY dept", StatusCode::TYPE_ERROR);
}

// Non-grouped column in SELECT with GROUP BY should error
TEST_F(QA_AggregateE2E, NonGroupedColumnErrors) {
    setup_employees();
    exec_error("SELECT dept, salary FROM emp GROUP BY dept", StatusCode::TYPE_ERROR);
}

// Non-grouped column in SELECT with aggregate should error
TEST_F(QA_AggregateE2E, NonGroupedColumnWithAggErrors) {
    setup_employees();
    exec_error("SELECT dept, id, COUNT(*) FROM emp GROUP BY dept", StatusCode::TYPE_ERROR);
}

// ===========================================================================
// ACCEPTANCE CRITERIA INTEGRATION TEST
// ===========================================================================

// The exact integration test from the acceptance criteria:
// SELECT col, COUNT(*) FROM t GROUP BY col HAVING COUNT(*) > 1
TEST_F(QA_AggregateE2E, AcceptanceCriteriaIntegrationTest) {
    exec_ok("CREATE TABLE t (col VARCHAR, val INT)");
    exec_ok("INSERT INTO t VALUES ('a', 10)");
    exec_ok("INSERT INTO t VALUES ('a', 20)");
    exec_ok("INSERT INTO t VALUES ('a', 30)");
    exec_ok("INSERT INTO t VALUES ('b', 40)");
    exec_ok("INSERT INTO t VALUES ('c', 50)");
    exec_ok("INSERT INTO t VALUES ('c', 60)");

    auto qr = exec_ok("SELECT col, COUNT(*) FROM t GROUP BY col HAVING COUNT(*) > 1");

    // 'a' has 3, 'c' has 2 → both pass. 'b' has 1 → filtered.
    ASSERT_EQ(qr.rows.size(), 2u);

    std::unordered_map<std::string, int64_t> counts;
    for (auto& row : qr.rows) {
        counts[row[0].as_string()] = row[1].as_int64();
    }

    EXPECT_EQ(counts["a"], 3);
    EXPECT_EQ(counts["c"], 2);
    EXPECT_EQ(counts.count("b"), 0u); // 'b' filtered out by HAVING
}

// ===========================================================================
// HAVING + ORDER BY combined
// ===========================================================================

TEST_F(QA_AggregateE2E, HavingWithOrderBy) {
    setup_employees();

    auto qr =
        exec_ok("SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 1 ORDER BY dept");
    ASSERT_EQ(qr.rows.size(), 2u);

    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
    EXPECT_EQ(qr.rows[1][0].as_string(), "sales");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 2);
}

// ===========================================================================
// GROUP BY with ORDER BY on aggregate column
// ===========================================================================

TEST_F(QA_AggregateE2E, OrderByAggregateColumn) {
    setup_employees();

    auto qr = exec_ok("SELECT dept, SUM(salary) FROM emp GROUP BY dept ORDER BY SUM(salary) DESC");
    ASSERT_EQ(qr.rows.size(), 3u);

    // eng=330, sales=185, hr=80 → descending
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 330);
    EXPECT_EQ(qr.rows[1][0].as_string(), "sales");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 185);
    EXPECT_EQ(qr.rows[2][0].as_string(), "hr");
    EXPECT_EQ(qr.rows[2][1].as_int64(), 80);
}

// ===========================================================================
// STRING_AGG end-to-end
// ===========================================================================

TEST_F(QA_AggregateE2E, StringAggE2E) {
    exec_ok("CREATE TABLE items (cat VARCHAR, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES ('fruit', 'apple')");
    exec_ok("INSERT INTO items VALUES ('fruit', 'banana')");
    exec_ok("INSERT INTO items VALUES ('veg', 'carrot')");

    auto qr = exec_ok("SELECT cat, STRING_AGG(name, ', ') FROM items GROUP BY cat");
    ASSERT_EQ(qr.rows.size(), 2u);

    for (auto& row : qr.rows) {
        if (row[0].as_string() == "fruit") {
            // Order depends on insertion, which is apple then banana
            EXPECT_EQ(row[1].as_string(), "apple, banana");
        } else {
            EXPECT_EQ(row[0].as_string(), "veg");
            EXPECT_EQ(row[1].as_string(), "carrot");
        }
    }
}

// ===========================================================================
// SUM integer overflow test (potential UB)
// ===========================================================================

TEST_F(QA_HashAggregate, SumLargeInt64Values) {
    // Test summing large INT64 values that are within safe range
    int64_t large_val = 1'000'000'000'000LL; // 1 trillion
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(large_val)}, {}},
        Tuple{{Value(std::string("x")), Value(large_val)}, {}},
        Tuple{{Value(std::string("x")), Value(large_val)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int64_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 3'000'000'000'000LL);
}

// ===========================================================================
// GROUP BY single column with many duplicates
// ===========================================================================

TEST_F(QA_HashAggregate, ManyDuplicateGroupKeys) {
    // 100 rows in 2 groups
    std::vector<Tuple> data;
    for (int i = 0; i < 50; ++i) {
        data.push_back(Tuple{{Value(std::string("a")), Value(int32_t(1))}, {}});
    }
    for (int i = 0; i < 50; ++i) {
        data.push_back(Tuple{{Value(std::string("b")), Value(int32_t(2))}, {}});
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_STAR);
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 2u);

    auto* a = find_group(rows, "a");
    auto* b = find_group(rows, "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(a->values[1].as_int64(), 50); // COUNT(*)
    EXPECT_EQ(a->values[2].as_int64(), 50); // SUM(50 * 1)
    EXPECT_EQ(b->values[1].as_int64(), 50);
    EXPECT_EQ(b->values[2].as_int64(), 100); // SUM(50 * 2)
}

// ===========================================================================
// GROUP BY empty string key value
// ===========================================================================

TEST_F(QA_HashAggregate, GroupByEmptyStringKey) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("")), Value(int32_t(10))}, {}},
        Tuple{{Value(std::string("")), Value(int32_t(20))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(30))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 2u);

    auto* empty = find_group(rows, "");
    auto* x = find_group(rows, "x");
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(x, nullptr);

    EXPECT_EQ(empty->values[1].as_int64(), 30); // SUM(10+20)
    EXPECT_EQ(x->values[1].as_int64(), 30);     // SUM(30)
}

// ===========================================================================
// SUM with zero values
// ===========================================================================

TEST_F(QA_HashAggregate, SumWithZeros) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("x")), Value(int32_t(0))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(0))}, {}},
        Tuple{{Value(std::string("x")), Value(int32_t(0))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::SUM, col_ref("t", "val"));

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    // SUM of zeros should be 0, not NULL (because we have non-NULL values)
    EXPECT_FALSE(rows[0].values[1].is_null());
    EXPECT_EQ(rows[0].values[1].as_int64(), 0);
}

} // namespace
} // namespace sixseven
