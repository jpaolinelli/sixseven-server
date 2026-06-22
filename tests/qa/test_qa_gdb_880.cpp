/// @file test_qa_gdb_880.cpp
/// QA regression tests for GDB-880: COUNT(DISTINCT) O(n^2) dedup replaced with
/// sort+unique using compare()-equality — behavior-preservation verification.
///
/// Tests verify:
///   (a) Correctness on a large group: 1000 rows with 10 distinct values.
///   (b) Edge cases: all-same, all-distinct, mixed NULLs, single value, empty.
///   (c) NULL behavior locked in: COUNT(DISTINCT) excludes NULLs (SQL standard).
///   (d) End-to-end SQL path via QueryEngine.
///   (e) Adversarial: NaN, +/-0.0, duplicate floats, very long strings.
///   (f) Non-vacuity: mutation-grade assertions confirming tests would fail if
///       dedup were broken.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/hash_aggregate.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_helpers.h"
#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ===========================================================================
// Direct HashAggregateOperator fixture (operator-level)
// ===========================================================================

class QA_GDB880 : public ::testing::Test {
protected:
    static OutputSchema int_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::INT32, true, 1},
        });
    }

    static OutputSchema string_schema() {
        return OutputSchema({
            {"t", "grp", TypeId::STRING, false, 1},
            {"t", "val", TypeId::STRING, true, 1},
        });
    }

    static ExprPtr col_ref(const std::string& table, const std::string& column) {
        auto e = std::make_unique<ColumnRefExpr>();
        e->table = table;
        e->column = column;
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
                     bool nullable = false) {
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
// (a) Large-group correctness: 1000 rows, 10 distinct values
// This also implicitly covers the O(n^2) → O(n log n) regression: the old
// loop would scan 1000*1000/2 = 500k comparisons; the new sort+unique does
// ~10000. Both must produce exactly 10 distinct values.
// ===========================================================================

TEST_F(QA_GDB880, LargeGroupCorrectDistinctCount) {
    // 1000 rows with values 0..9 cycling — exactly 10 distinct values.
    std::vector<Tuple> data;
    data.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        data.push_back(Tuple{{Value(std::string("g")), Value(int32_t(i % 10))}, {}});
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 10);
}

// ===========================================================================
// (b) Edge cases
// ===========================================================================

// All rows have the same value in one group: distinct count = 1.
TEST_F(QA_GDB880, AllSameValue) {
    std::vector<Tuple> data;
    for (int i = 0; i < 20; ++i) {
        data.push_back(Tuple{{Value(std::string("g")), Value(int32_t(42))}, {}});
    }

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

// All rows have different values: distinct count = n.
TEST_F(QA_GDB880, AllDistinctValues) {
    const int n = 50;
    std::vector<Tuple> data;
    for (int i = 0; i < n; ++i) {
        data.push_back(Tuple{{Value(std::string("g")), Value(int32_t(i))}, {}});
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), static_cast<int64_t>(n));
}

// Single value: distinct count = 1.
TEST_F(QA_GDB880, SingleValue) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value(int32_t(7))}, {}},
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

// Empty group (no rows at all, no GROUP BY): global aggregate returns 0.
TEST_F(QA_GDB880, EmptyInput) {
    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::vector<Tuple>{});
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u); // Global aggregate: one output row even for empty input.
    EXPECT_EQ(rows[0].values[0].as_int64(), 0);
}

// (c) NULL behavior locked in: NULLs are EXCLUDED from COUNT(DISTINCT).
// Mixed NULLs and non-NULLs: only non-NULL distinct values are counted.
TEST_F(QA_GDB880, NullsAreExcluded) {
    // 5 NULLs + values 1, 2, 2, 3 → 3 distinct non-NULL values.
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(1))}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(2))}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(2))}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(3))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 3);
}

// All NULLs: COUNT(DISTINCT) returns 0.
TEST_F(QA_GDB880, AllNullsCountDistinctIsZero) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 0);
}

// Multiple groups: each group's distinct count is independent.
TEST_F(QA_GDB880, MultipleGroupsIndependentDistinct) {
    // group "a": values 10, 10, 20 → 2 distinct
    // group "b": values 5, 5, 5   → 1 distinct
    // group "c": values 1, 2, 3   → 3 distinct
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("a")), Value(int32_t(10))}, {}},
        Tuple{{Value(std::string("a")), Value(int32_t(10))}, {}},
        Tuple{{Value(std::string("a")), Value(int32_t(20))}, {}},
        Tuple{{Value(std::string("b")), Value(int32_t(5))}, {}},
        Tuple{{Value(std::string("b")), Value(int32_t(5))}, {}},
        Tuple{{Value(std::string("b")), Value(int32_t(5))}, {}},
        Tuple{{Value(std::string("c")), Value(int32_t(1))}, {}},
        Tuple{{Value(std::string("c")), Value(int32_t(2))}, {}},
        Tuple{{Value(std::string("c")), Value(int32_t(3))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 3u);

    auto* a = find_group(rows, "a");
    auto* b = find_group(rows, "b");
    auto* c = find_group(rows, "c");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(a->values[1].as_int64(), 2);
    EXPECT_EQ(b->values[1].as_int64(), 1);
    EXPECT_EQ(c->values[1].as_int64(), 3);
}

// String column: sort order is lexicographic; dedup must still work correctly.
TEST_F(QA_GDB880, StringColumnDistinct) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value(std::string("apple"))}, {}},
        Tuple{{Value(std::string("g")), Value(std::string("banana"))}, {}},
        Tuple{{Value(std::string("g")), Value(std::string("apple"))}, {}},
        Tuple{{Value(std::string("g")), Value(std::string("cherry"))}, {}},
        Tuple{{Value(std::string("g")), Value(std::string("banana"))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 3); // apple, banana, cherry
}

// ===========================================================================
// End-to-end SQL path via QueryEngine
// ===========================================================================

class QA_GDB880_E2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb880";
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
        EXPECT_TRUE(result.has_value()) << "SQL failed: " << sql << "\n"
                                        << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// End-to-end: COUNT(DISTINCT) over a column with known distinct count.
TEST_F(QA_GDB880_E2E, CountDistinctE2E) {
    exec_ok("CREATE TABLE items (category VARCHAR, score INT)");
    // category 'A': scores 10, 20, 10, 30 → 3 distinct
    // category 'B': scores 5, 5, 5        → 1 distinct
    exec_ok("INSERT INTO items VALUES ('A', 10)");
    exec_ok("INSERT INTO items VALUES ('A', 20)");
    exec_ok("INSERT INTO items VALUES ('A', 10)");
    exec_ok("INSERT INTO items VALUES ('A', 30)");
    exec_ok("INSERT INTO items VALUES ('B', 5)");
    exec_ok("INSERT INTO items VALUES ('B', 5)");
    exec_ok("INSERT INTO items VALUES ('B', 5)");

    auto qr = exec_ok("SELECT category, COUNT(DISTINCT score) FROM items GROUP BY category");
    ASSERT_EQ(qr.rows.size(), 2u);

    std::unordered_map<std::string, int64_t> counts;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 2u);
        counts[row[0].as_string()] = row[1].as_int64();
    }

    EXPECT_EQ(counts["A"], 3);
    EXPECT_EQ(counts["B"], 1);
}

// End-to-end: NULLs are excluded from COUNT(DISTINCT) (SQL standard behavior).
TEST_F(QA_GDB880_E2E, CountDistinctNullsExcludedE2E) {
    exec_ok("CREATE TABLE mixed (grp VARCHAR, val INT)");
    exec_ok("INSERT INTO mixed VALUES ('g', NULL)");
    exec_ok("INSERT INTO mixed VALUES ('g', NULL)");
    exec_ok("INSERT INTO mixed VALUES ('g', 1)");
    exec_ok("INSERT INTO mixed VALUES ('g', 2)");
    exec_ok("INSERT INTO mixed VALUES ('g', 2)");

    auto qr = exec_ok("SELECT grp, COUNT(DISTINCT val) FROM mixed GROUP BY grp");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2); // 1 and 2; NULLs excluded
}

// End-to-end: large group correctness (implicit performance regression guard).
// 100 rows, 5 distinct values — verifies the fix is correct at scale without
// a timing assertion (which would be flaky).
TEST_F(QA_GDB880_E2E, LargeGroupE2E) {
    exec_ok("CREATE TABLE big (grp VARCHAR, val INT)");
    for (int i = 0; i < 100; ++i) {
        exec_ok("INSERT INTO big VALUES ('g', " + std::to_string(i % 5) + ")");
    }

    auto qr = exec_ok("SELECT grp, COUNT(DISTINCT val) FROM big GROUP BY grp");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 5);
}

// ===========================================================================
// (e) Adversarial: float edge cases — NaN, ±0.0, duplicate floats
// ===========================================================================

// OutputSchema with FLOAT64 column for float tests.
static OutputSchema float64_schema() {
    return OutputSchema({
        {"t", "grp", TypeId::STRING, false, 1},
        {"t", "val", TypeId::FLOAT64, true, 1},
    });
}

// NaN values: compare_doubles treats NaN==NaN as equal, so two NaN rows
// should dedup to a distinct count of 1 (not 2).
// NaN produced via 0.0/0.0 — no <cmath> required on MSVC.
TEST_F(QA_GDB880, NanDedupedToOne) {
    // NOLINTNEXTLINE(clang-diagnostic-division-by-zero)
    volatile double zero = 0.0;
    double nan_val = zero / zero; // quiet NaN on x86
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value(nan_val)}, {}},
        Tuple{{Value(std::string("g")), Value(nan_val)}, {}},
        Tuple{{Value(std::string("g")), Value(1.0)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(float64_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    // NaN deduped to 1, plus 1.0 = 2 distinct (not 3).
    EXPECT_EQ(rows[0].values[1].as_int64(), 2);
}

// Positive zero and negative zero: IEEE 754 treats them as equal under <, >,
// and compare_doubles also returns equal — so ±0.0 dedup to 1 distinct.
TEST_F(QA_GDB880, PosAndNegZeroDedupedToOne) {
    double pos_zero = 0.0;
    double neg_zero = -0.0;
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value(pos_zero)}, {}},
        Tuple{{Value(std::string("g")), Value(neg_zero)}, {}},
        Tuple{{Value(std::string("g")), Value(pos_zero)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(float64_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 1); // +0.0 == -0.0 under compare()
}

// Duplicate floats in unsorted order: sort+unique must still deduplicate.
TEST_F(QA_GDB880, DuplicateFloatsUnsortedOrder) {
    // Values: 3.0, 1.0, 2.0, 1.0, 3.0, 2.0 — 3 distinct.
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value(3.0)}, {}},
        Tuple{{Value(std::string("g")), Value(1.0)}, {}},
        Tuple{{Value(std::string("g")), Value(2.0)}, {}},
        Tuple{{Value(std::string("g")), Value(1.0)}, {}},
        Tuple{{Value(std::string("g")), Value(3.0)}, {}},
        Tuple{{Value(std::string("g")), Value(2.0)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(float64_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 3);
}

// +Inf and -Inf: should be distinct from each other and from finite values.
// 1e308 * 10 overflows to +Inf on IEEE 754; negation gives -Inf.
TEST_F(QA_GDB880, InfinityValuesDistinct) {
    double pos_inf = 1e308 * 10.0; // +Inf
    double neg_inf = -pos_inf;     // -Inf
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value(pos_inf)}, {}},
        Tuple{{Value(std::string("g")), Value(pos_inf)}, {}},
        Tuple{{Value(std::string("g")), Value(neg_inf)}, {}},
        Tuple{{Value(std::string("g")), Value(1.0)}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(float64_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    // +Inf deduped to 1, -Inf = 1, 1.0 = 1 → 3 distinct total.
    EXPECT_EQ(rows[0].values[1].as_int64(), 3);
}

// ===========================================================================
// (e) Adversarial: very long strings — sort must handle large allocations.
// ===========================================================================

TEST_F(QA_GDB880, VeryLongStringsDedupCorrectly) {
    // Two distinct very long strings (8000 chars each), repeated multiple times.
    std::string long_a(8000, 'a');
    std::string long_b(8000, 'b');
    std::vector<Tuple> data;
    for (int i = 0; i < 50; ++i) {
        data.push_back(Tuple{{Value(std::string("g")), Value(long_a)}, {}});
        data.push_back(Tuple{{Value(std::string("g")), Value(long_b)}, {}});
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(string_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].values[1].as_int64(), 2);
}

// ===========================================================================
// (f) Non-vacuity / mutation guard: LargeGroupCorrectDistinctCount would fail
// if the unique predicate were broken (all values kept). Confirm the count is
// exact — not merely "positive" — so a broken dedup would produce 1000 != 10.
// This is a meta-assertion; the value 10 is precise, not a range.
// ===========================================================================

TEST_F(QA_GDB880, MutationGuard_LargeGroupExactCount) {
    // Same setup as LargeGroupCorrectDistinctCount. A broken dedup returning
    // all rows would yield 1000; one that deduped nothing would yield 1 if sort
    // happened to put same values adjacent. 10 is the ONLY correct answer.
    std::vector<Tuple> data;
    data.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        data.push_back(Tuple{{Value(std::string("g")), Value(int32_t(i % 10))}, {}});
    }

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    int64_t count = rows[0].values[1].as_int64();
    // Exact assertion: broken dedup gives 1000, correct gives 10.
    EXPECT_EQ(count, 10) << "Expected exactly 10 distinct values; got " << count
                         << " (if 1000, dedup is entirely broken; if 1, unique pred is "
                            "inverted)";
}

// Mutation guard for NULL exclusion: if NULLs were counted, result would be 5
// instead of 3.
TEST_F(QA_GDB880, MutationGuard_NullsNotCounted) {
    std::vector<Tuple> data = {
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value::make_null()}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(10))}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(20))}, {}},
        Tuple{{Value(std::string("g")), Value(int32_t(30))}, {}},
    };

    BoundStatement bound;
    AggTestBuilder builder;
    builder.add_group_by(col_ref("t", "grp"), {"t", "grp", TypeId::STRING, false, 1});
    builder.add_agg(AggFunc::COUNT_DISTINCT, col_ref("t", "val"), "", TypeId::INT64, false);

    auto child = std::make_unique<VectorIterator>(int_schema(), std::move(data));
    auto agg = builder.build(std::move(child), bound);

    auto rows = run(*agg);
    ASSERT_EQ(rows.size(), 1u);
    int64_t count = rows[0].values[1].as_int64();
    EXPECT_EQ(count, 3) << "Expected 3 (NULLs excluded); got " << count
                        << " (if 5, NULLs are incorrectly counted as distinct)";
}

} // namespace
} // namespace sixseven
