/// @file test_qa_gdb_22.cpp
/// Adversarial QA tests for GDB-22: Basic Executor (Volcano Iterator Model).
///
/// Covers subtasks GDB-108 (Iterator/SeqScan/Filter), GDB-109 (Project/Sort/
/// Limit/ExprEval), GDB-110 (Insert/Update/Delete), GDB-111 (Planner/Pipeline).

#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/delete.h"
#include "giodb/executor/expr_evaluator.h"
#include "giodb/executor/filter.h"
#include "giodb/executor/insert.h"
#include "giodb/executor/limit.h"
#include "giodb/executor/project.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/seq_scan.h"
#include "giodb/executor/sort.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/tuple.h"
#include "giodb/executor/update.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace giodb;

// =============================================================================
// Expression helpers
// =============================================================================

namespace {

ExprPtr lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr lit_float(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::FLOAT;
    e->value = v;
    return e;
}

ExprPtr lit_string(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::STRING;
    e->value = v;
    return e;
}

ExprPtr lit_bool(bool v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::BOOLEAN;
    e->value = v ? "true" : "false";
    return e;
}

ExprPtr lit_null() {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::NULL_LITERAL;
    e->value = "";
    return e;
}

ExprPtr col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

ExprPtr binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryExpr>();
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

ExprPtr unary(UnaryOp op, ExprPtr operand) {
    auto e = std::make_unique<UnaryExpr>();
    e->op = op;
    e->operand = std::move(operand);
    return e;
}

Tuple make_tuple(std::vector<Value> vals) {
    return Tuple{std::move(vals), std::nullopt};
}

BoundStatement empty_bound() {
    return BoundStatement{};
}

} // namespace

// =============================================================================
// Heap-based test fixture for operator-level tests
// =============================================================================

class QA_ExecutorOperators : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb22_ops.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
            {"age", TypeId::INT32},
        });

        OutputColumn c1{"", "id", TypeId::INT32, false, 0};
        OutputColumn c2{"", "name", TypeId::STRING, true, 0};
        OutputColumn c3{"", "age", TypeId::INT32, true, 0};
        output_schema_ = OutputSchema({c1, c2, c3});
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    void insert_row(TableHeap& heap, int32_t id, const std::string& name, int32_t age) {
        std::vector<Value> vals = {Value(id), Value(name), Value(age)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void insert_null_row(TableHeap& heap, int32_t id) {
        std::vector<Value> vals = {Value(id), Value::make_null(), Value::make_null()};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::unique_ptr<SeqScanOperator> make_scan(TableHeap& heap) {
        return std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    }

    std::vector<Tuple> drain(Iterator& iter) {
        auto open = iter.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        std::vector<Tuple> results;
        while (true) {
            auto row = iter.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row || !row->has_value())
                break;
            results.push_back(std::move(row->value()));
        }
        iter.close();
        return results;
    }

    int count_rows(TableHeap& heap) {
        SeqScanOperator scan(heap, storage_schema_, output_schema_);
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        int count = 0;
        while (true) {
            auto row = scan.next();
            EXPECT_TRUE(row.has_value());
            if (!row || !row->has_value())
                break;
            ++count;
        }
        scan.close();
        return count;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    Schema storage_schema_;
    OutputSchema output_schema_;
};

// =============================================================================
// GDB-108: SeqScan & Filter adversarial tests
// =============================================================================

// --- SeqScan: NULL values ---

TEST_F(QA_ExecutorOperators, SeqScanWithNullValues) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_null_row(heap, 1);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    auto results = drain(*scan);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_TRUE(results[0].values[1].is_null());
    EXPECT_TRUE(results[0].values[2].is_null());
    EXPECT_EQ(results[1].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[1].as_string(), "bob");
}

// --- SeqScan: calling next() before open() ---

TEST_F(QA_ExecutorOperators, SeqScanNextBeforeOpen) {
    TableHeap heap(*bpm_, dm_, file_id_);
    SeqScanOperator scan(heap, storage_schema_, output_schema_);

    // next() without open() should return an error, not crash.
    auto row = scan.next();
    EXPECT_FALSE(row.has_value());
    EXPECT_EQ(row.error().code, StatusCode::INTERNAL_ERROR);
}

// --- SeqScan: calling close() is safe even without open ---

TEST_F(QA_ExecutorOperators, SeqScanCloseWithoutOpen) {
    TableHeap heap(*bpm_, dm_, file_id_);
    SeqScanOperator scan(heap, storage_schema_, output_schema_);
    // Should not crash.
    scan.close();
}

// --- SeqScan: predicate with NULL column ---

TEST_F(QA_ExecutorOperators, SeqScanPredicateOnNullColumn) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_null_row(heap, 1); // age is NULL
    insert_row(heap, 2, "bob", 25);

    // Predicate: age > 20 — should skip NULL row (NULL > 20 => NULL => false).
    auto pred = binary(BinaryOp::GREATER, col_ref("age"), lit_int("20"));
    BoundStatement bound;
    SeqScanOperator scan(heap, storage_schema_, output_schema_, pred.get(), &bound);

    auto open = scan.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    std::vector<Tuple> results;
    while (true) {
        auto row = scan.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        results.push_back(std::move(row->value()));
    }
    scan.close();

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
}

// --- Filter: all NULL values ---

TEST_F(QA_ExecutorOperators, FilterAllNullValues) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_null_row(heap, 1);
    insert_null_row(heap, 2);
    insert_null_row(heap, 3);

    auto child = make_scan(heap);
    auto pred = binary(BinaryOp::EQUAL, col_ref("name"), lit_string("alice"));
    BoundStatement bound;
    FilterOperator filter(std::move(child), *pred, bound);

    auto results = drain(filter);
    EXPECT_TRUE(results.empty());
}

// --- SeqScan: many rows (stress test) ---

TEST_F(QA_ExecutorOperators, SeqScanManyRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        insert_row(heap, i, "user_" + std::to_string(i), 20 + (i % 50));
    }

    auto scan = make_scan(heap);
    auto results = drain(*scan);
    EXPECT_EQ(results.size(), static_cast<size_t>(N));
}

// =============================================================================
// GDB-109: Project / Sort / Limit / ExprEval adversarial tests
// =============================================================================

// --- Sort with NULL values ---

TEST_F(QA_ExecutorOperators, SortWithNullValuesAsc) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_null_row(heap, 2); // name is NULL
    insert_row(heap, 3, "bob", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::ASC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    // NULLs sort last for ASC
    EXPECT_EQ(results[0].values[1].as_string(), "alice");
    EXPECT_EQ(results[1].values[1].as_string(), "bob");
    EXPECT_TRUE(results[2].values[1].is_null());
}

TEST_F(QA_ExecutorOperators, SortWithNullValuesDesc) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_null_row(heap, 2);
    insert_row(heap, 3, "bob", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::DESC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    // NULLs sort first for DESC
    EXPECT_TRUE(results[0].values[1].is_null());
    EXPECT_EQ(results[1].values[1].as_string(), "bob");
    EXPECT_EQ(results[2].values[1].as_string(), "alice");
}

TEST_F(QA_ExecutorOperators, SortAllNulls) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_null_row(heap, 1);
    insert_null_row(heap, 2);
    insert_null_row(heap, 3);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::ASC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    // All NULLs — stable sort should preserve original order.
    EXPECT_TRUE(results[0].values[1].is_null());
    EXPECT_TRUE(results[1].values[1].is_null());
    EXPECT_TRUE(results[2].values[1].is_null());
}

// --- Sort single row ---

TEST_F(QA_ExecutorOperators, SortSingleRow) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[1].as_string(), "alice");
}

// --- Limit: negative values ---

TEST_F(QA_ExecutorOperators, LimitNegativeValue) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    // Negative limit — should return no rows (like limit 0).
    LimitOperator limit(std::move(scan), -1);
    auto results = drain(limit);
    EXPECT_TRUE(results.empty());
}

TEST_F(QA_ExecutorOperators, LimitNegativeOffset) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    // Negative offset — should act as offset 0.
    LimitOperator limit(std::move(scan), 10, -5);
    auto results = drain(limit);
    EXPECT_EQ(results.size(), 2u);
}

// --- Limit: very large limit ---

TEST_F(QA_ExecutorOperators, LimitVeryLargeValue) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    LimitOperator limit(std::move(scan), INT64_MAX);
    auto results = drain(limit);
    EXPECT_EQ(results.size(), 1u);
}

// --- Project: empty projection list ---

TEST_F(QA_ExecutorOperators, ProjectEmptyProjection) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    BoundStatement bound;

    // Empty projection — should produce tuples with 0 columns.
    std::vector<ProjectionExpr> projs;
    OutputSchema proj_schema;

    ProjectOperator project(std::move(scan), std::move(projs), std::move(proj_schema), bound);
    auto results = drain(project);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].values.empty());
}

// --- Project: NULL column reference ---

TEST_F(QA_ExecutorOperators, ProjectNullColumnValue) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_null_row(heap, 1);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto name_e = col_ref("name");
    std::vector<ProjectionExpr> projs = {{name_e.get(), "name"}};
    OutputColumn oc{"", "name", TypeId::STRING, true, 0};
    OutputSchema proj_schema({oc});

    ProjectOperator project(std::move(scan), std::move(projs), std::move(proj_schema), bound);
    auto results = drain(project);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].values[0].is_null());
}

// =============================================================================
// GDB-109: Expression evaluator adversarial tests
// =============================================================================

// --- Modulo by zero (integer path) ---

TEST(QA_ExprEval, ModuloByZeroInteger) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::MODULO, lit_int("10"), lit_int("0"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- Modulo by zero (float path) ---

TEST(QA_ExprEval, ModuloByZeroFloat) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::MODULO, lit_float("10.5"), lit_float("0.0"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- Division by zero (float) ---

TEST(QA_ExprEval, DivisionByZeroFloat) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::DIVIDE, lit_float("10.0"), lit_float("0.0"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- NULL propagation through all arithmetic operators ---

TEST(QA_ExprEval, NullPlusNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::ADD, lit_null(), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(QA_ExprEval, NullMultiply) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::MULTIPLY, lit_null(), lit_int("5"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(QA_ExprEval, NullDivide) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::DIVIDE, lit_int("10"), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(QA_ExprEval, NullModulo) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::MODULO, lit_null(), lit_int("3"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

// --- Three-valued logic: NULL AND FALSE = FALSE ---

TEST(QA_ExprEval, NullAndFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::AND, lit_null(), lit_bool(false));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->type_id(), TypeId::BOOL);
    EXPECT_FALSE(result->as_bool());
}

// --- Three-valued logic: NULL OR TRUE = TRUE ---

TEST(QA_ExprEval, NullOrTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::OR, lit_null(), lit_bool(true));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_TRUE(result->as_bool());
}

// --- Three-valued logic: NULL OR FALSE = NULL ---

TEST(QA_ExprEval, NullOrFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::OR, lit_null(), lit_bool(false));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// --- NOT NULL = NULL ---

TEST(QA_ExprEval, NotNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NOT, lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// --- Comparison: both NULL ---

TEST(QA_ExprEval, NullEqualsNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::EQUAL, lit_null(), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // NULL = NULL should be NULL, not true.
    EXPECT_TRUE(result->is_null());
}

// --- Negate zero ---

TEST(QA_ExprEval, NegateZero) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NEGATE, lit_int("0"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 0);
}

// --- NEGATE on string should error ---

TEST(QA_ExprEval, NegateStringErrors) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NEGATE, lit_string("hello"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// --- NOT on non-bool should error ---

TEST(QA_ExprEval, NotOnIntegerErrors) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NOT, lit_int("1"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// --- BETWEEN boundary values (inclusive) ---

TEST(QA_ExprEval, BetweenBoundaryInclusive) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // 5 BETWEEN 5 AND 10 → true (lower bound inclusive)
    auto between_low = std::make_unique<BetweenExpr>();
    between_low->expr = lit_int("5");
    between_low->low = lit_int("5");
    between_low->high = lit_int("10");
    between_low->negated = false;

    auto result = evaluate_expr(*between_low, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());

    // 10 BETWEEN 5 AND 10 → true (upper bound inclusive)
    auto between_high = std::make_unique<BetweenExpr>();
    between_high->expr = lit_int("10");
    between_high->low = lit_int("5");
    between_high->high = lit_int("10");
    between_high->negated = false;

    result = evaluate_expr(*between_high, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

// --- BETWEEN with NULL ---

TEST(QA_ExprEval, BetweenWithNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto between_expr = std::make_unique<BetweenExpr>();
    between_expr->expr = lit_null();
    between_expr->low = lit_int("1");
    between_expr->high = lit_int("10");
    between_expr->negated = false;

    auto result = evaluate_expr(*between_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// --- IN with NULL in list ---

TEST(QA_ExprEval, InListWithNullNotFound) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_int("5");
    in_expr->values.push_back(lit_int("1"));
    in_expr->values.push_back(lit_null());
    in_expr->values.push_back(lit_int("3"));
    in_expr->negated = false;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // 5 not found, but NULL in list → result should be NULL (SQL semantics).
    EXPECT_TRUE(result->is_null());
}

TEST(QA_ExprEval, InListWithNullFound) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_int("3");
    in_expr->values.push_back(lit_int("1"));
    in_expr->values.push_back(lit_null());
    in_expr->values.push_back(lit_int("3"));
    in_expr->negated = false;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Found match → true (NULL presence doesn't matter).
    EXPECT_FALSE(result->is_null());
    EXPECT_TRUE(result->as_bool());
}

// --- IN with NULL value being searched ---

TEST(QA_ExprEval, InWithNullExpr) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_null();
    in_expr->values.push_back(lit_int("1"));
    in_expr->values.push_back(lit_int("2"));
    in_expr->negated = false;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// --- IN with empty list ---

TEST(QA_ExprEval, InEmptyList) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_int("5");
    // No values in list.
    in_expr->negated = false;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

// --- LIKE: empty string matching ---

TEST(QA_ExprEval, LikeEmptyStringEmptyPattern) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("");
    like_expr->pattern = lit_string("");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(QA_ExprEval, LikeEmptyStringPercentPattern) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("");
    like_expr->pattern = lit_string("%");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(QA_ExprEval, LikeEmptyStringUnderscorePattern) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("");
    like_expr->pattern = lit_string("_");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

// --- LIKE with NULL ---

TEST(QA_ExprEval, LikeWithNullValue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_null();
    like_expr->pattern = lit_string("%");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// --- CASE with all NULLs ---

TEST(QA_ExprEval, CaseAllNullConditions) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto case_expr = std::make_unique<CaseExpr>();
    CaseWhen when;
    when.condition = lit_null();
    when.result = lit_string("match");
    case_expr->whens.push_back(std::move(when));
    // No else — should return NULL.

    auto result = evaluate_expr(*case_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// --- Simple CASE with NULL operand ---

TEST(QA_ExprEval, SimpleCaseNullOperand) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto case_expr = std::make_unique<CaseExpr>();
    case_expr->operand = lit_null();
    CaseWhen when;
    when.condition = lit_int("1");
    when.result = lit_string("match");
    case_expr->whens.push_back(std::move(when));
    case_expr->else_expr = lit_string("else");

    auto result = evaluate_expr(*case_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // NULL operand should never match, return else.
    EXPECT_EQ(result->as_string(), "else");
}

// --- Column ref: out of range index ---

TEST(QA_ExprEval, ColumnRefAmbiguous) {
    // Two columns with the same name from different tables.
    OutputColumn c1{"t1", "id", TypeId::INT32, false, 1};
    OutputColumn c2{"t2", "id", TypeId::INT32, false, 2};
    OutputSchema schema({c1, c2});
    auto tuple = make_tuple({Value(int32_t(1)), Value(int32_t(2))});
    auto bound = empty_bound();

    // Unqualified "id" is ambiguous — should error.
    auto result = evaluate_expr(*col_ref("id"), tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// --- Deeply nested expression ---

TEST(QA_ExprEval, DeeplyNestedExpression) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // Build: ((((1 + 1) + 1) + 1) + ... + 1) = 100
    ExprPtr expr = lit_int("1");
    for (int i = 1; i < 100; ++i) {
        expr = binary(BinaryOp::ADD, std::move(expr), lit_int("1"));
    }
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 100);
}

// --- String comparison ---

TEST(QA_ExprEval, StringComparison) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::LESS, lit_string("abc"), lit_string("abd"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(QA_ExprEval, StringEqualityEmpty) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::EQUAL, lit_string(""), lit_string(""));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

// --- CONCAT with type coercion ---

TEST(QA_ExprEval, ConcatIntCoercion) {
    // CONCAT of string with integer — coerce() does not support int→string,
    // so this is expected to fail (not a bug, by design).
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::CONCAT, lit_string("age: "), lit_int("30"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
}

// --- Predicate: non-boolean expression ---

TEST(QA_ExprEval, PredicateNonBooleanErrors) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // Predicate evaluation with integer result (not bool).
    auto result = evaluate_predicate(*lit_int("42"), tuple, schema, bound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// GDB-110: DML operator adversarial tests
// =============================================================================

// --- Insert: zero rows ---

TEST_F(QA_ExecutorOperators, InsertZeroRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    // Empty value rows list.
    std::vector<std::vector<const Expr*>> rows;
    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);

    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = insert.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 0);

    insert.close();
    EXPECT_EQ(count_rows(heap), 0);
}

// --- Insert: many rows ---

TEST_F(QA_ExecutorOperators, InsertManyRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    constexpr int N = 200;
    std::vector<ExprPtr> owned_exprs;
    std::vector<std::vector<const Expr*>> rows;

    for (int i = 0; i < N; ++i) {
        owned_exprs.push_back(lit_int(std::to_string(i)));
        owned_exprs.push_back(lit_string("user_" + std::to_string(i)));
        owned_exprs.push_back(lit_int(std::to_string(20 + i)));
        auto* id = owned_exprs[owned_exprs.size() - 3].get();
        auto* name = owned_exprs[owned_exprs.size() - 2].get();
        auto* age = owned_exprs[owned_exprs.size() - 1].get();
        rows.push_back({id, name, age});
    }

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);
    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = insert.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), N);

    insert.close();
    EXPECT_EQ(count_rows(heap), N);
}

// --- Insert: NULL values ---

TEST_F(QA_ExecutorOperators, InsertWithNullValues) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    auto id_expr = lit_int("1");
    auto name_expr = lit_null();
    auto age_expr = lit_null();

    std::vector<std::vector<const Expr*>> rows = {
        {id_expr.get(), name_expr.get(), age_expr.get()},
    };

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);
    auto open = insert.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = insert.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 1);

    // Verify the inserted row.
    SeqScanOperator scan(heap, storage_schema_, output_schema_);
    auto sopen = scan.open();
    ASSERT_TRUE(sopen.has_value()) << sopen.error().message;
    auto row = scan.next();
    ASSERT_TRUE(row.has_value()) << row.error().message;
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ(row->value().values[0].as_int32(), 1);
    EXPECT_TRUE(row->value().values[1].is_null());
    EXPECT_TRUE(row->value().values[2].is_null());
    scan.close();
    insert.close();
}

// --- Update: no matching rows ---

TEST_F(QA_ExecutorOperators, UpdateNoMatchingRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    BoundStatement bound;

    // UPDATE WHERE age > 100 — no matches.
    auto pred = binary(BinaryOp::GREATER, col_ref("age"), lit_int("100"));
    auto new_name = lit_string("updated");
    std::vector<UpdateAssignment> assigns = {{1, new_name.get()}};

    auto scan = std::make_unique<SeqScanOperator>(
        heap, storage_schema_, output_schema_, pred.get(), &bound);
    UpdateOperator update(heap, storage_schema_, std::move(scan), std::move(assigns), bound);

    auto open = update.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = update.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 0);

    update.close();

    // Verify data unchanged.
    SeqScanOperator verify(heap, storage_schema_, output_schema_);
    auto vopen = verify.open();
    ASSERT_TRUE(vopen.has_value());
    auto row = verify.next();
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ(row->value().values[1].as_string(), "alice");
    verify.close();
}

// --- Update: SET to NULL ---

TEST_F(QA_ExecutorOperators, UpdateSetToNull) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    BoundStatement bound;

    auto null_expr = lit_null();
    std::vector<UpdateAssignment> assigns = {{1, null_expr.get()}}; // name = NULL

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    UpdateOperator update(heap, storage_schema_, std::move(scan), std::move(assigns), bound);

    auto open = update.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = update.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 1);

    update.close();

    // Verify name is now NULL.
    SeqScanOperator verify(heap, storage_schema_, output_schema_);
    auto vopen = verify.open();
    ASSERT_TRUE(vopen.has_value());
    auto row = verify.next();
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());
    EXPECT_TRUE(row->value().values[1].is_null());
    verify.close();
}

// --- Delete: no matching rows ---

TEST_F(QA_ExecutorOperators, DeleteNoMatchingRows) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    BoundStatement bound;

    auto pred = binary(BinaryOp::GREATER, col_ref("age"), lit_int("100"));
    auto scan = std::make_unique<SeqScanOperator>(
        heap, storage_schema_, output_schema_, pred.get(), &bound);
    DeleteOperator del(heap, std::move(scan));

    auto open = del.open();
    ASSERT_TRUE(open.has_value()) << open.error().message;

    auto result = del.next();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().values[0].as_int64(), 0);

    del.close();
    EXPECT_EQ(count_rows(heap), 1);
}

// --- DML: second next() call returns nullopt ---

TEST_F(QA_ExecutorOperators, DmlInsertSecondNextIsEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);
    BoundStatement bound;

    auto id = lit_int("1");
    auto name = lit_string("x");
    auto age = lit_int("10");
    std::vector<std::vector<const Expr*>> rows = {{id.get(), name.get(), age.get()}};

    InsertOperator insert(heap, storage_schema_, std::move(rows), bound);
    auto open = insert.open();
    ASSERT_TRUE(open.has_value());

    auto r1 = insert.next();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());

    auto r2 = insert.next();
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());

    auto r3 = insert.next();
    ASSERT_TRUE(r3.has_value());
    EXPECT_FALSE(r3->has_value());

    insert.close();
}

TEST_F(QA_ExecutorOperators, DmlUpdateSecondNextIsEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    BoundStatement bound;
    auto age_lit = lit_int("99");
    std::vector<UpdateAssignment> assigns = {{2, age_lit.get()}};
    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    UpdateOperator update(heap, storage_schema_, std::move(scan), std::move(assigns), bound);

    auto open = update.open();
    ASSERT_TRUE(open.has_value());

    auto r1 = update.next();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());

    auto r2 = update.next();
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());

    update.close();
}

TEST_F(QA_ExecutorOperators, DmlDeleteSecondNextIsEmpty) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    DeleteOperator del(heap, std::move(scan));

    auto open = del.open();
    ASSERT_TRUE(open.has_value());

    auto r1 = del.next();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());

    auto r2 = del.next();
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());

    del.close();
}

// =============================================================================
// GDB-111: End-to-end pipeline adversarial tests (via QueryEngine)
// =============================================================================

class QA_QueryPipeline : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb22_pipeline";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

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
        EXPECT_TRUE(result.has_value())
            << "SQL failed: " << sql << "\nError: " << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << " for: " << sql;
        }
    }

    void create_test_table() { exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)"); }

    void insert_test_data() {
        exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 35)");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// --- SELECT with WHERE returning no rows ---

TEST_F(QA_QueryPipeline, SelectWhereNoMatch) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users WHERE age > 100");
    EXPECT_TRUE(qr.rows.empty());
}

// --- SELECT from empty table ---

TEST_F(QA_QueryPipeline, SelectFromEmptyTable) {
    create_test_table();
    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_TRUE(qr.rows.empty());
}

// --- SELECT with ORDER BY + LIMIT + OFFSET ---

TEST_F(QA_QueryPipeline, SelectOrderByLimitOffset) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age ASC LIMIT 2 OFFSET 1");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[1][0].as_string(), "charlie");
}

// --- SELECT LIMIT 0 ---

TEST_F(QA_QueryPipeline, SelectLimitZero) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users LIMIT 0");
    EXPECT_TRUE(qr.rows.empty());
}

// --- SELECT OFFSET beyond row count ---

TEST_F(QA_QueryPipeline, SelectOffsetBeyondRows) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users LIMIT 10 OFFSET 100");
    EXPECT_TRUE(qr.rows.empty());
}

// --- INSERT with NULL ---

TEST_F(QA_QueryPipeline, InsertWithNull) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, NULL, NULL)");

    auto qr = exec_ok("SELECT * FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_TRUE(qr.rows[0][1].is_null());
    EXPECT_TRUE(qr.rows[0][2].is_null());
}

// --- UPDATE with no matching rows ---

TEST_F(QA_QueryPipeline, UpdateNoMatch) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("UPDATE users SET name = 'x' WHERE age > 1000");
    EXPECT_EQ(qr.affected_rows, 0);
}

// --- DELETE with no matching rows ---

TEST_F(QA_QueryPipeline, DeleteNoMatch) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("DELETE FROM users WHERE age > 1000");
    EXPECT_EQ(qr.affected_rows, 0);

    // Verify all rows still exist.
    auto check = exec_ok("SELECT * FROM users");
    EXPECT_EQ(check.rows.size(), 3u);
}

// --- DELETE all rows ---

TEST_F(QA_QueryPipeline, DeleteAllRows) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("DELETE FROM users");
    EXPECT_EQ(qr.affected_rows, 3);

    auto check = exec_ok("SELECT * FROM users");
    EXPECT_TRUE(check.rows.empty());
}

// --- DROP TABLE then SELECT should error ---

TEST_F(QA_QueryPipeline, DropTableThenSelect) {
    create_test_table();
    exec_ok("DROP TABLE users");
    exec_error("SELECT * FROM users", StatusCode::NOT_FOUND);
}

// --- CREATE TABLE duplicate without IF NOT EXISTS ---

TEST_F(QA_QueryPipeline, CreateTableDuplicate) {
    create_test_table();
    exec_error("CREATE TABLE users (id INT)", StatusCode::ALREADY_EXISTS);
}

// --- SELECT with expression in projection ---

TEST_F(QA_QueryPipeline, SelectExpressionProjection) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");

    auto qr = exec_ok("SELECT age + 10 FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    // age(30) + 10 = 40 (may be int64 from expression evaluator)
    EXPECT_EQ(qr.rows[0][0].as_int64(), 40);
}

// --- SELECT with arithmetic in WHERE ---

TEST_F(QA_QueryPipeline, SelectArithmeticInWhere) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users WHERE age * 2 > 60");
    // age*2>60 means age>30, so only charlie(35).
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "charlie");
}

// --- Division by zero in expression ---

TEST_F(QA_QueryPipeline, DivisionByZeroInSelect) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");

    auto result = engine_->execute("SELECT age / 0 FROM users");
    EXPECT_FALSE(result.has_value());
}

// --- INSERT + UPDATE + DELETE round-trip ---

TEST_F(QA_QueryPipeline, InsertUpdateDeleteRoundTrip) {
    create_test_table();

    // Insert
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");
    exec_ok("INSERT INTO users VALUES (3, 'charlie', 35)");

    // Update
    auto up = exec_ok("UPDATE users SET age = 99 WHERE name = 'bob'");
    EXPECT_EQ(up.affected_rows, 1);

    // Verify
    auto qr = exec_ok("SELECT age FROM users WHERE name = 'bob'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 99);

    // Delete
    auto del = exec_ok("DELETE FROM users WHERE age = 99");
    EXPECT_EQ(del.affected_rows, 1);

    // Verify
    auto remaining = exec_ok("SELECT * FROM users");
    EXPECT_EQ(remaining.rows.size(), 2u);
}

// --- Multiple inserts in one VALUES clause ---

TEST_F(QA_QueryPipeline, MultiRowInsert) {
    create_test_table();

    auto qr = exec_ok("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30)");
    EXPECT_EQ(qr.affected_rows, 3);

    auto check = exec_ok("SELECT * FROM users ORDER BY id");
    ASSERT_EQ(check.rows.size(), 3u);
    EXPECT_EQ(check.rows[0][0].as_int32(), 1);
    EXPECT_EQ(check.rows[1][0].as_int32(), 2);
    EXPECT_EQ(check.rows[2][0].as_int32(), 3);
}

// --- SELECT with ORDER BY DESC + LIMIT ---

TEST_F(QA_QueryPipeline, SelectOrderByDescLimit) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age DESC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "charlie");
}

// --- SELECT with IS NULL ---

TEST_F(QA_QueryPipeline, SelectIsNull) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, NULL, 30)");
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");

    auto qr = exec_ok("SELECT id FROM users WHERE name IS NULL");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// --- SELECT with IS NOT NULL ---

TEST_F(QA_QueryPipeline, SelectIsNotNull) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, NULL, 30)");
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");

    auto qr = exec_ok("SELECT id FROM users WHERE name IS NOT NULL");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

// --- SELECT with BETWEEN ---

TEST_F(QA_QueryPipeline, SelectBetween) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users WHERE age BETWEEN 26 AND 34");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

// --- SELECT with IN ---

TEST_F(QA_QueryPipeline, SelectIn) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users WHERE age IN (25, 35) ORDER BY age");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "bob");
    EXPECT_EQ(qr.rows[1][0].as_string(), "charlie");
}

// --- SELECT with NOT IN ---

TEST_F(QA_QueryPipeline, SelectNotIn) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users WHERE age NOT IN (25, 35)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

// --- SELECT with LIKE ---

TEST_F(QA_QueryPipeline, SelectLike) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name FROM users WHERE name LIKE 'a%'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

// --- SELECT with CASE expression ---

TEST_F(QA_QueryPipeline, SelectCaseExpression) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name, CASE WHEN age > 30 THEN 'senior' ELSE 'junior' END "
                      "FROM users ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "junior"); // alice, 30
    EXPECT_EQ(qr.rows[1][1].as_string(), "junior"); // bob, 25
    EXPECT_EQ(qr.rows[2][1].as_string(), "senior"); // charlie, 35
}

// --- SELECT with string concatenation ---

TEST_F(QA_QueryPipeline, SelectConcatOperator) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");

    auto qr = exec_ok("SELECT name || '_suffix' FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice_suffix");
}

// --- UPDATE SET column = expression using another column ---

TEST_F(QA_QueryPipeline, UpdateWithColumnExpression) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");

    exec_ok("UPDATE users SET age = age + 5");

    auto qr = exec_ok("SELECT age FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 35);
}

// --- SELECT from non-existent table ---

TEST_F(QA_QueryPipeline, SelectFromNonExistent) {
    exec_error("SELECT * FROM nonexistent", StatusCode::NOT_FOUND);
}

// --- INSERT into non-existent table ---

TEST_F(QA_QueryPipeline, InsertIntoNonExistent) {
    exec_error("INSERT INTO nonexistent VALUES (1, 'a', 10)", StatusCode::NOT_FOUND);
}

// --- UPDATE on non-existent table ---

TEST_F(QA_QueryPipeline, UpdateNonExistent) {
    exec_error("UPDATE nonexistent SET x = 1", StatusCode::NOT_FOUND);
}

// --- DELETE from non-existent table ---

TEST_F(QA_QueryPipeline, DeleteFromNonExistent) {
    exec_error("DELETE FROM nonexistent", StatusCode::NOT_FOUND);
}

// --- DROP non-existent table ---

TEST_F(QA_QueryPipeline, DropNonExistentTable) {
    exec_error("DROP TABLE nonexistent", StatusCode::NOT_FOUND);
}

// --- DROP IF EXISTS non-existent table ---

TEST_F(QA_QueryPipeline, DropIfExistsNonExistent) {
    auto qr = exec_ok("DROP TABLE IF EXISTS nonexistent");
    EXPECT_EQ(qr.message, "DROP TABLE");
}

// --- CREATE TABLE IF NOT EXISTS ---

TEST_F(QA_QueryPipeline, CreateTableIfNotExists) {
    create_test_table();
    auto qr = exec_ok("CREATE TABLE IF NOT EXISTS users (id INT)");
    EXPECT_EQ(qr.message, "CREATE TABLE");
}

// --- Stress: insert then select many rows ---

TEST_F(QA_QueryPipeline, StressInsertThenSelectManyRows) {
    create_test_table();

    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", 'user_" + std::to_string(i) +
                "', " + std::to_string(20 + i) + ")");
    }

    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_EQ(qr.rows.size(), static_cast<size_t>(N));
}

// --- Instrumentation: enable and check metrics ---

TEST_F(QA_ExecutorOperators, InstrumentationRecordsMetrics) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    scan->enable_instrumentation();

    EXPECT_TRUE(scan->is_instrumented());
    EXPECT_EQ(scan->rows_produced(), 0);

    auto results = drain(*scan);

    EXPECT_EQ(scan->rows_produced(), 2);
    EXPECT_GE(scan->loops(), 1);
    EXPECT_GT(scan->execution_time().count(), 0);
}
