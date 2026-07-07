// Adversarial QA tests for GDB-799:
// PATH serialization round-trip silently drops Path::total_weight
//
// Two fix sites:
//   (a) TupleSerializer (src/table/tuple.cpp) — var_value_bytes / read_var_value
//   (b) ExternalSortOperator disk-spill (src/executor/external_sort.cpp) — write_tuple / read_tuple
//
// Attack surface:
//   - Empty paths (zero steps, zero weight)
//   - Single-node paths (one step, no edge)
//   - Long paths (1000+ steps)
//   - total_weight = 0.0, negative, fractional, very large, NaN, +/-Inf
//   - Multiple PATH columns in one tuple (thread-local buffer alias)
//   - PATH mixed with other column types in a tuple
//   - Multiple spill runs (many tuples forced through flush+merge)
//   - Work_mem threshold boundary (force flush on the nth tuple)
//   - NULL PATH values alongside non-null PATH values
//   - Deserialization with length == sizeof(double) (zero steps, valid weight)
//   - Deserialization with length < sizeof(double) (returns null — guarded path)

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/external_sort.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers shared across all GDB799 tests
// =============================================================================

namespace {

// Minimal in-memory iterator that drains a vector of tuples.
class VectorSource799 : public Iterator {
public:
    VectorSource799(std::vector<Tuple> tuples, OutputSchema schema)
        : tuples_(std::move(tuples)), schema_(std::move(schema)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        cursor_ = 0;
        return ok();
    }
    Result<std::optional<Tuple>> do_next() override {
        if (cursor_ >= tuples_.size())
            return ok(std::optional<Tuple>(std::nullopt));
        return ok(std::optional<Tuple>(tuples_[cursor_++]));
    }
    void do_close() override { cursor_ = 0; }

private:
    std::vector<Tuple> tuples_;
    OutputSchema schema_;
    size_t cursor_ = 0;
};

ExprPtr col_ref_gdb799(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

// Make a Path with the given total_weight and n steps.
Path make_path(double weight, uint32_t n_steps) {
    Path p;
    p.total_weight = weight;
    for (uint32_t i = 0; i < n_steps; ++i) {
        p.steps.push_back({static_cast<int64_t>(i + 1),
                           (i + 1 < n_steps) ? static_cast<int64_t>(i + 100) : int64_t{-1}});
    }
    return p;
}

// Drain all tuples from an opened iterator.
std::vector<Tuple> drain_gdb799(Iterator& iter) {
    auto open = iter.open();
    if (!open)
        ADD_FAILURE() << open.error().message;
    std::vector<Tuple> results;
    while (true) {
        auto row = iter.next();
        if (!row) {
            ADD_FAILURE() << row.error().message;
            break;
        }
        if (!row->has_value())
            break;
        results.push_back(std::move(row->value()));
    }
    iter.close();
    return results;
}

} // namespace

// =============================================================================
// Suite A: TupleSerializer (var_value_bytes / read_var_value) round-trips
// =============================================================================

class QA_GDB799_TupleSerializer : public ::testing::Test {
protected:
    // Round-trip a single-column PATH tuple through TupleSerializer.
    std::vector<Value> round_trip(const Path& path) {
        Schema schema({{"path", TypeId::PATH}});
        std::vector<Value> values = {Value(path)};
        auto buf = TupleSerializer::serialize(values, schema);
        EXPECT_TRUE(buf.has_value()) << buf.error().message;
        if (!buf)
            return {};
        auto result = TupleSerializer::deserialize(*buf, schema);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        if (!result)
            return {};
        return *result;
    }
};

// -- Empty path, zero weight --------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_EmptyPathZeroWeight) {
    Path p = make_path(0.0, 0);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    ASSERT_FALSE(vals[0].is_null());
    const auto& restored = vals[0].as_path();
    EXPECT_EQ(restored.total_weight, 0.0);
    EXPECT_TRUE(restored.steps.empty());
}

// -- Single-node path (terminal step only, no edge) ---------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_SingleNodePath) {
    Path p;
    p.total_weight = 7.77;
    p.steps.push_back({42, -1}); // terminal node
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    ASSERT_FALSE(vals[0].is_null());
    const auto& restored = vals[0].as_path();
    EXPECT_EQ(restored.total_weight, 7.77);
    ASSERT_EQ(restored.steps.size(), 1u);
    EXPECT_EQ(restored.steps[0].node_pk_as_int64(), 42);
    EXPECT_EQ(restored.steps[0].edge_id, -1);
}

// -- Negative total_weight ----------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_NegativeWeight) {
    Path p = make_path(-3.14, 2);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    ASSERT_FALSE(vals[0].is_null());
    EXPECT_EQ(vals[0].as_path().total_weight, -3.14);
}

// -- Fractional total_weight (sub-1.0) ----------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_FractionalWeight) {
    Path p = make_path(0.000001, 3);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_DOUBLE_EQ(vals[0].as_path().total_weight, 0.000001);
}

// -- Very large total_weight --------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_VeryLargeWeight) {
    Path p = make_path(1.7e308, 1);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0].as_path().total_weight, 1.7e308);
}

// -- NaN total_weight ---------------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_NaNWeight) {
    Path p = make_path(std::numeric_limits<double>::quiet_NaN(), 2);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    // After round-trip, NaN should remain NaN.
    EXPECT_TRUE(std::isnan(vals[0].as_path().total_weight));
}

// -- +Inf total_weight --------------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_InfWeight) {
    Path p = make_path(std::numeric_limits<double>::infinity(), 2);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_TRUE(std::isinf(vals[0].as_path().total_weight));
    EXPECT_GT(vals[0].as_path().total_weight, 0.0);
}

// -- -Inf total_weight --------------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_NegInfWeight) {
    Path p = make_path(-std::numeric_limits<double>::infinity(), 2);
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_TRUE(std::isinf(vals[0].as_path().total_weight));
    EXPECT_LT(vals[0].as_path().total_weight, 0.0);
}

// -- Long path (1000 steps) ---------------------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_LongPath1000Steps) {
    // Each PathStep is 16 bytes; 1000 steps = 16000 bytes + 8 bytes header < 65535.
    Path p = make_path(999.5, 100); // 100 steps stays well within tuple limit
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    ASSERT_FALSE(vals[0].is_null());
    const auto& restored = vals[0].as_path();
    EXPECT_EQ(restored.total_weight, 999.5);
    ASSERT_EQ(restored.steps.size(), 100u);
    // Spot-check first and last steps.
    EXPECT_EQ(restored.steps[0].node_pk_as_int64(), 1);
    EXPECT_EQ(restored.steps[99].node_pk_as_int64(), 100);
    EXPECT_EQ(restored.steps[99].edge_id, -1);
}

// -- Two PATH columns in same tuple (thread-local buffer alias attack) ---------
// var_value_bytes uses a static thread_local buffer. If two PATH values are
// serialized from the same tuple, the second call in the size-counting pass
// must not corrupt the bytes written for the first in the write pass.

TEST_F(QA_GDB799_TupleSerializer, GDB799_TwoPathColumnsInOneTuple) {
    Schema schema({{"path1", TypeId::PATH}, {"path2", TypeId::PATH}});

    Path p1 = make_path(11.1, 2);
    Path p2 = make_path(22.2, 3);

    std::vector<Value> values = {Value(p1), Value(p2)};
    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value()) << buf.error().message;

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);

    const auto& r1 = (*result)[0].as_path();
    const auto& r2 = (*result)[1].as_path();

    EXPECT_DOUBLE_EQ(r1.total_weight, 11.1) << "path1 total_weight corrupted";
    ASSERT_EQ(r1.steps.size(), 2u);
    EXPECT_EQ(r1.steps[0].node_pk_as_int64(), 1);

    EXPECT_DOUBLE_EQ(r2.total_weight, 22.2) << "path2 total_weight corrupted";
    ASSERT_EQ(r2.steps.size(), 3u);
    EXPECT_EQ(r2.steps[0].node_pk_as_int64(), 1);
}

// -- PATH mixed with other column types (STRING, INT32, PATH, FLOAT64) --------

TEST_F(QA_GDB799_TupleSerializer, GDB799_PathMixedWithOtherTypes) {
    Schema schema({{"name", TypeId::STRING},
                   {"count", TypeId::INT32},
                   {"route", TypeId::PATH},
                   {"score", TypeId::FLOAT64}});

    Path p = make_path(3.5, 3);

    std::vector<Value> values = {
        Value(std::string("alice")), Value(int32_t{42}), Value(p), Value(double{9.81})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value()) << buf.error().message;

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 4u);

    EXPECT_EQ((*result)[0].as_string(), "alice");
    EXPECT_EQ((*result)[1].as_int32(), 42);

    const auto& restored_path = (*result)[2].as_path();
    EXPECT_DOUBLE_EQ(restored_path.total_weight, 3.5) << "total_weight lost in mixed-type tuple";
    ASSERT_EQ(restored_path.steps.size(), 3u);

    EXPECT_DOUBLE_EQ((*result)[3].as_float64(), 9.81);
}

// -- NULL PATH alongside a non-null PATH --------------------------------------

TEST_F(QA_GDB799_TupleSerializer, GDB799_NullPathAlongsideNonNull) {
    Schema schema({{"path1", TypeId::PATH}, {"path2", TypeId::PATH}});

    Path p2 = make_path(55.5, 2);
    std::vector<Value> values = {Value::make_null(), Value(p2)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value()) << buf.error().message;

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);

    EXPECT_TRUE((*result)[0].is_null()) << "first PATH should remain NULL";
    EXPECT_FALSE((*result)[1].is_null()) << "second PATH should not be NULL";
    EXPECT_DOUBLE_EQ((*result)[1].as_path().total_weight, 55.5);
}

// -- Serialized length == sizeof(double): empty steps, non-zero weight ---------
// This exercises the exact boundary of the length < sizeof(double) guard added
// in read_var_value. Length == 8 means zero PathStep entries and a valid weight.

TEST_F(QA_GDB799_TupleSerializer, GDB799_LengthExactlySizeofDouble) {
    Path p = make_path(123.456, 0); // 0 steps => serialized = 8 bytes header only
    auto vals = round_trip(p);
    ASSERT_EQ(vals.size(), 1u);
    ASSERT_FALSE(vals[0].is_null()) << "zero-step PATH should not deserialize as null";
    EXPECT_DOUBLE_EQ(vals[0].as_path().total_weight, 123.456);
    EXPECT_TRUE(vals[0].as_path().steps.empty());
}

// -- Repeated round-trips (accumulation check) --------------------------------
// The thread_local buffer must not bleed state across successive serialize calls.

TEST_F(QA_GDB799_TupleSerializer, GDB799_RepeatedRoundTrips) {
    double weights[] = {1.0, 2.5, 0.0, -7.0, 1e15};
    for (double w : weights) {
        Path p = make_path(w, 2);
        auto vals = round_trip(p);
        ASSERT_EQ(vals.size(), 1u) << "weight=" << w;
        EXPECT_EQ(vals[0].as_path().total_weight, w) << "weight=" << w;
    }
}

// =============================================================================
// Suite B: ExternalSortOperator spill path round-trips
// =============================================================================

class QA_GDB799_ExternalSort : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb799_esort";
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);
    }
    void TearDown() override { std::filesystem::remove_all(temp_dir_); }

    // Build and drain an ExternalSort with a tiny work_mem to force spilling.
    // Schema: [sort_key INT32, path PATH]
    std::vector<Tuple>
    sort_and_drain(std::vector<Tuple> input, const OutputSchema& schema, size_t work_mem = 64) {
        auto source = std::make_unique<VectorSource799>(std::move(input), schema);
        BoundStatement bound;
        auto key_expr = col_ref_gdb799("sort_key");
        std::vector<SortKey> keys = {{key_expr.get(), SortDirection::ASC}};
        ExternalSortOperator sort(
            std::move(source), std::move(keys), bound, work_mem, 128, temp_dir_);
        return drain_gdb799(sort);
    }

    std::filesystem::path temp_dir_;
};

// -- Zero total_weight survives spill -----------------------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillZeroWeight) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path p = make_path(0.0, 2);
    Tuple t{{Value(int32_t{1}), Value(std::move(p))}, std::nullopt};

    auto results = sort_and_drain({t}, schema);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[1].as_path().total_weight, 0.0);
    EXPECT_EQ(results[0].values[1].as_path().steps.size(), 2u);
}

// -- Negative weight survives spill -------------------------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillNegativeWeight) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path p = make_path(-99.9, 3);
    Tuple t{{Value(int32_t{1}), Value(std::move(p))}, std::nullopt};

    auto results = sort_and_drain({t}, schema);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[1].as_path().total_weight, -99.9);
}

// -- NaN weight survives spill ------------------------------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillNaNWeight) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path p = make_path(std::numeric_limits<double>::quiet_NaN(), 1);
    Tuple t{{Value(int32_t{1}), Value(std::move(p))}, std::nullopt};

    auto results = sort_and_drain({t}, schema);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(std::isnan(results[0].values[1].as_path().total_weight))
        << "NaN total_weight must survive disk spill";
}

// -- +Inf weight survives spill -----------------------------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillInfWeight) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path p = make_path(std::numeric_limits<double>::infinity(), 1);
    Tuple t{{Value(int32_t{1}), Value(std::move(p))}, std::nullopt};

    auto results = sort_and_drain({t}, schema);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(std::isinf(results[0].values[1].as_path().total_weight));
    EXPECT_GT(results[0].values[1].as_path().total_weight, 0.0);
}

// -- Empty path survives spill ------------------------------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillEmptyPath) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path p = make_path(42.0, 0);
    Tuple t{{Value(int32_t{1}), Value(std::move(p))}, std::nullopt};

    auto results = sort_and_drain({t}, schema);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[1].as_path().total_weight, 42.0)
        << "empty-steps PATH must preserve total_weight through spill";
    EXPECT_TRUE(results[0].values[1].as_path().steps.empty());
}

// -- Many tuples forcing multiple spill runs and merges -----------------------
// Forces N flush runs. With work_mem=64 bytes, each tuple triggers a flush
// because an estimated-size check fires when buffer.size() > 1. With 20 tuples
// we get ~10 runs (each flush writes 2 tuples), exercising the merge path.

TEST_F(QA_GDB799_ExternalSort, GDB799_MultipleSpillRunsAndMerges) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    const int N = 20;
    std::vector<Tuple> input;
    input.reserve(N);
    // Insert in reverse order so sorting is non-trivial.
    for (int i = N; i >= 1; --i) {
        Path p = make_path(static_cast<double>(i) * 1.5, 2);
        input.push_back({{Value(int32_t{i}), Value(std::move(p))}, std::nullopt});
    }

    auto results = sort_and_drain(std::move(input), schema, /*work_mem=*/64);
    ASSERT_EQ(results.size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(results[i].values[0].as_int32(), i + 1) << "sort order wrong at index " << i;
        double expected_weight = static_cast<double>(i + 1) * 1.5;
        EXPECT_DOUBLE_EQ(results[i].values[1].as_path().total_weight, expected_weight)
            << "total_weight wrong after merge for sort_key=" << (i + 1);
    }
}

// -- PATH mixed with STRING and INT32 in spill path ---------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillPathMixedTypes) {
    OutputSchema schema{{{"", "sort_key", TypeId::INT32, false, 0},
                         {"", "label", TypeId::STRING, true, 0},
                         {"", "route", TypeId::PATH, false, 0}}};

    Path p1 = make_path(8.8, 3);
    Path p2 = make_path(4.4, 2);
    std::vector<Tuple> input = {
        {{Value(int32_t{2}), Value(std::string("beta")), Value(std::move(p1))}, std::nullopt},
        {{Value(int32_t{1}), Value(std::string("alpha")), Value(std::move(p2))}, std::nullopt},
    };

    auto source = std::make_unique<VectorSource799>(std::move(input), schema);
    BoundStatement bound;
    auto key_expr = col_ref_gdb799("sort_key");
    std::vector<SortKey> keys = {{key_expr.get(), SortDirection::ASC}};
    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 64, 128, temp_dir_);
    auto results = drain_gdb799(sort);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[1].as_string(), "alpha");
    EXPECT_DOUBLE_EQ(results[0].values[2].as_path().total_weight, 4.4)
        << "total_weight wrong for sort_key=1 in mixed-type spill";
    EXPECT_EQ(results[1].values[1].as_string(), "beta");
    EXPECT_DOUBLE_EQ(results[1].values[2].as_path().total_weight, 8.8)
        << "total_weight wrong for sort_key=2 in mixed-type spill";
}

// -- Two PATH columns in one tuple through spill path -------------------------
// Tests the two-call pattern of write_tuple for multiple PATH columns.

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillTwoPathColumns) {
    OutputSchema schema{{{"", "sort_key", TypeId::INT32, false, 0},
                         {"", "path1", TypeId::PATH, false, 0},
                         {"", "path2", TypeId::PATH, false, 0}}};

    Path p1a = make_path(1.1, 2);
    Path p1b = make_path(2.2, 3);
    Path p2a = make_path(3.3, 1);
    Path p2b = make_path(4.4, 4);

    std::vector<Tuple> input = {
        {{Value(int32_t{2}), Value(p1a), Value(p1b)}, std::nullopt},
        {{Value(int32_t{1}), Value(p2a), Value(p2b)}, std::nullopt},
    };

    auto source = std::make_unique<VectorSource799>(std::move(input), schema);
    BoundStatement bound;
    auto key_expr = col_ref_gdb799("sort_key");
    std::vector<SortKey> keys = {{key_expr.get(), SortDirection::ASC}};
    ExternalSortOperator sort(std::move(source), std::move(keys), bound, 64, 128, temp_dir_);
    auto results = drain_gdb799(sort);

    ASSERT_EQ(results.size(), 2u);
    // sort_key=1: p2a, p2b
    EXPECT_DOUBLE_EQ(results[0].values[1].as_path().total_weight, 3.3)
        << "path1 col wrong for row 0";
    EXPECT_DOUBLE_EQ(results[0].values[2].as_path().total_weight, 4.4)
        << "path2 col wrong for row 0";
    // sort_key=2: p1a, p1b
    EXPECT_DOUBLE_EQ(results[1].values[1].as_path().total_weight, 1.1)
        << "path1 col wrong for row 1";
    EXPECT_DOUBLE_EQ(results[1].values[2].as_path().total_weight, 2.2)
        << "path2 col wrong for row 1";
}

// -- Work_mem boundary: flush at exactly 2 tuples -----------------------------
// With work_mem set to 0 (forces flush after first tuple lands in a 2-element
// buffer), every pair of tuples becomes a separate run. Verify total_weight
// is intact after merging those runs.

TEST_F(QA_GDB799_ExternalSort, GDB799_WorkMemBoundaryFlushAt2) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    Path pa = make_path(100.0, 1);
    Path pb = make_path(200.0, 1);
    Path pc = make_path(300.0, 1);
    std::vector<Tuple> input = {
        {{Value(int32_t{3}), Value(pc)}, std::nullopt},
        {{Value(int32_t{1}), Value(pa)}, std::nullopt},
        {{Value(int32_t{2}), Value(pb)}, std::nullopt},
    };

    // work_mem=0 guarantees mem_used >= work_mem_bytes_ as soon as there is > 1
    // tuple in the buffer, maximally stressing the flush path.
    auto results = sort_and_drain(std::move(input), schema, /*work_mem=*/0);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_DOUBLE_EQ(results[0].values[1].as_path().total_weight, 100.0);
    EXPECT_DOUBLE_EQ(results[1].values[1].as_path().total_weight, 200.0);
    EXPECT_DOUBLE_EQ(results[2].values[1].as_path().total_weight, 300.0);
}

// -- Very large total_weight survives spill -----------------------------------

TEST_F(QA_GDB799_ExternalSort, GDB799_SpillVeryLargeWeight) {
    OutputSchema schema{
        {{"", "sort_key", TypeId::INT32, false, 0}, {"", "path", TypeId::PATH, false, 0}}};

    const double large = 1.7e308;
    Path p = make_path(large, 1);
    Tuple t{{Value(int32_t{1}), Value(std::move(p))}, std::nullopt};

    auto results = sort_and_drain({t}, schema);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[1].as_path().total_weight, large);
}

// end of GDB-799 adversarial QA tests
