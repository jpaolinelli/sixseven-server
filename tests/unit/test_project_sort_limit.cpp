#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/limit.h"
#include "giodb/executor/project.h"
#include "giodb/executor/seq_scan.h"
#include "giodb/executor/sort.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace giodb;

// -- Expression helpers -------------------------------------------------------

namespace {

ExprPtr lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr lit_string(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::STRING;
    e->value = v;
    return e;
}

ExprPtr col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

ExprPtr binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryExpr>();
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

} // namespace

// -- Test fixture: heap-backed ------------------------------------------------

class ProjectSortLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "giodb_test_psl.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        // Schema: id (INT32), name (STRING), age (INT32)
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

    /// Build a SeqScan over the current heap.
    std::unique_ptr<SeqScanOperator> make_scan(TableHeap& heap) {
        return std::make_unique<SeqScanOperator>(heap, storage_schema_, output_schema_);
    }

    /// Drain all tuples from an iterator.
    std::vector<Tuple> drain(Iterator& iter) {
        auto open = iter.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;
        std::vector<Tuple> results;
        while (true) {
            auto row = iter.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row || !row->has_value()) {
                break;
            }
            results.push_back(std::move(row->value()));
        }
        iter.close();
        return results;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    Schema storage_schema_;
    OutputSchema output_schema_;
};

// =============================================================================
// Project tests
// =============================================================================

TEST_F(ProjectSortLimitTest, ProjectSelectColumns) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    // Project only: name, id (reorder columns)
    auto name_expr = col_ref("name");
    auto id_expr = col_ref("id");
    std::vector<ProjectionExpr> projs = {
        {name_expr.get(), "name"},
        {id_expr.get(), "id"},
    };

    OutputColumn oc1{"", "name", TypeId::STRING, true, 0};
    OutputColumn oc2{"", "id", TypeId::INT32, false, 0};
    OutputSchema proj_schema({oc1, oc2});

    ProjectOperator project(std::move(scan), std::move(projs), std::move(proj_schema), bound);
    auto results = drain(project);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_string(), "alice");
    EXPECT_EQ(results[0].values[1].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_string(), "bob");
    EXPECT_EQ(results[1].values[1].as_int32(), 2);
}

TEST_F(ProjectSortLimitTest, ProjectComputedExpression) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    BoundStatement bound;

    // Project: age + 10 AS age_plus_10
    auto expr = binary_expr(BinaryOp::ADD, col_ref("age"), lit_int("10"));
    std::vector<ProjectionExpr> projs = {
        {expr.get(), "age_plus_10"},
    };

    OutputColumn oc{"", "age_plus_10", TypeId::INT64, true, 0};
    OutputSchema proj_schema({oc});

    ProjectOperator project(std::move(scan), std::move(projs), std::move(proj_schema), bound);
    auto results = drain(project);

    ASSERT_EQ(results.size(), 1u);
    // 30 + 10 = 40 (int64 since literals are int64)
    EXPECT_EQ(results[0].values[0].as_int64(), 40);
}

TEST_F(ProjectSortLimitTest, ProjectLiteralExpression) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    BoundStatement bound;

    // Project: 'hello' AS greeting
    auto hello = lit_string("hello");
    std::vector<ProjectionExpr> projs = {
        {hello.get(), "greeting"},
    };

    OutputColumn oc{"", "greeting", TypeId::STRING, false, 0};
    OutputSchema proj_schema({oc});

    ProjectOperator project(std::move(scan), std::move(projs), std::move(proj_schema), bound);
    auto results = drain(project);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_string(), "hello");
}

TEST_F(ProjectSortLimitTest, ProjectOutputSchema) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto scan = make_scan(heap);
    BoundStatement bound;

    auto id_expr = col_ref("id");
    std::vector<ProjectionExpr> projs = {{id_expr.get(), "my_id"}};

    OutputColumn oc{"", "my_id", TypeId::INT32, false, 0};
    OutputSchema proj_schema({oc});

    ProjectOperator project(std::move(scan), std::move(projs), std::move(proj_schema), bound);
    EXPECT_EQ(project.output_schema().column_count(), 1u);
    EXPECT_EQ(project.output_schema().column(0).name, "my_id");
}

// =============================================================================
// Sort tests
// =============================================================================

TEST_F(ProjectSortLimitTest, SortAscending) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 3, "charlie", 35);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[1].as_string(), "bob");     // age 25
    EXPECT_EQ(results[1].values[1].as_string(), "alice");   // age 30
    EXPECT_EQ(results[2].values[1].as_string(), "charlie"); // age 35
}

TEST_F(ProjectSortLimitTest, SortDescending) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::DESC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[1].as_string(), "charlie"); // age 35
    EXPECT_EQ(results[1].values[1].as_string(), "alice");   // age 30
    EXPECT_EQ(results[2].values[1].as_string(), "bob");     // age 25
}

TEST_F(ProjectSortLimitTest, SortMultipleKeys) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 30);
    insert_row(heap, 3, "charlie", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    // ORDER BY age ASC, name DESC
    auto age_expr = col_ref("age");
    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {
        {age_expr.get(), SortDirection::ASC},
        {name_expr.get(), SortDirection::DESC},
    };

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[1].as_string(), "charlie"); // age 25
    EXPECT_EQ(results[1].values[1].as_string(), "bob");     // age 30, name DESC
    EXPECT_EQ(results[2].values[1].as_string(), "alice");   // age 30, name DESC
}

TEST_F(ProjectSortLimitTest, SortEmptyTable) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto scan = make_scan(heap);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);
    EXPECT_TRUE(results.empty());
}

TEST_F(ProjectSortLimitTest, SortStringColumn) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 3, "charlie", 35);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto name_expr = col_ref("name");
    std::vector<SortKey> keys = {{name_expr.get(), SortDirection::ASC}};

    SortOperator sort(std::move(scan), std::move(keys), bound);
    auto results = drain(sort);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[1].as_string(), "alice");
    EXPECT_EQ(results[1].values[1].as_string(), "bob");
    EXPECT_EQ(results[2].values[1].as_string(), "charlie");
}

// =============================================================================
// Limit tests
// =============================================================================

TEST_F(ProjectSortLimitTest, LimitRowCount) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    auto scan = make_scan(heap);
    LimitOperator limit(std::move(scan), 2);
    auto results = drain(limit);

    EXPECT_EQ(results.size(), 2u);
}

TEST_F(ProjectSortLimitTest, LimitWithOffset) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    auto scan = make_scan(heap);
    LimitOperator limit(std::move(scan), 2, 1); // skip 1, take 2
    auto results = drain(limit);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[1].as_string(), "bob");
    EXPECT_EQ(results[1].values[1].as_string(), "charlie");
}

TEST_F(ProjectSortLimitTest, LimitZero) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    LimitOperator limit(std::move(scan), 0);
    auto results = drain(limit);

    EXPECT_TRUE(results.empty());
}

TEST_F(ProjectSortLimitTest, LimitExceedsRowCount) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    LimitOperator limit(std::move(scan), 100);
    auto results = drain(limit);

    EXPECT_EQ(results.size(), 2u);
}

TEST_F(ProjectSortLimitTest, OffsetExceedsRowCount) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);

    auto scan = make_scan(heap);
    LimitOperator limit(std::move(scan), 10, 5); // skip 5 rows, only 1 exists
    auto results = drain(limit);

    EXPECT_TRUE(results.empty());
}

// =============================================================================
// Composition: Sort + Limit
// =============================================================================

TEST_F(ProjectSortLimitTest, SortThenLimit) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 3, "charlie", 35);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);

    auto scan = make_scan(heap);
    BoundStatement bound;

    auto age_expr = col_ref("age");
    std::vector<SortKey> keys = {{age_expr.get(), SortDirection::ASC}};

    auto sort = std::make_unique<SortOperator>(std::move(scan), std::move(keys), bound);
    LimitOperator limit(std::move(sort), 2);
    auto results = drain(limit);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[1].as_string(), "bob");   // age 25 (smallest)
    EXPECT_EQ(results[1].values[1].as_string(), "alice"); // age 30
}

TEST_F(ProjectSortLimitTest, ProjectThenSortThenLimit) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice", 30);
    insert_row(heap, 2, "bob", 25);
    insert_row(heap, 3, "charlie", 35);

    auto scan = make_scan(heap);
    BoundStatement bound;

    // Project: name, age
    auto name_e = col_ref("name");
    auto age_e = col_ref("age");
    std::vector<ProjectionExpr> projs = {
        {name_e.get(), "name"},
        {age_e.get(), "age"},
    };
    OutputColumn oc1{"", "name", TypeId::STRING, true, 0};
    OutputColumn oc2{"", "age", TypeId::INT32, true, 0};
    OutputSchema proj_schema({oc1, oc2});

    auto project = std::make_unique<ProjectOperator>(
        std::move(scan), std::move(projs), std::move(proj_schema), bound);

    // Sort by age DESC
    auto sort_age = col_ref("age");
    std::vector<SortKey> keys = {{sort_age.get(), SortDirection::DESC}};
    auto sort = std::make_unique<SortOperator>(std::move(project), std::move(keys), bound);

    // Limit 2
    LimitOperator limit(std::move(sort), 2);
    auto results = drain(limit);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values.size(), 2u);                // projected to 2 columns
    EXPECT_EQ(results[0].values[0].as_string(), "charlie"); // age 35 (highest)
    EXPECT_EQ(results[1].values[0].as_string(), "alice");   // age 30
}
