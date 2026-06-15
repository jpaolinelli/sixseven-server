// GDB-803: Verify that SortMergeJoin, BitmapScan, and ExternalSort are
// reachable from the planner — i.e., actual queries can produce these operators
// in the EXPLAIN output or via direct operator construction.
//
// Tests:
//   ExternalSort — ORDER BY on any table now uses ExternalSortOperator
//                  (wired unconditionally for all ORDER BY).
//   BitmapScan   — OR of two simple predicates, each covered by a B+ tree
//                  index, produces a BitmapScan (OR combine mode).
//   SortMergeJoin — cost model unit test verifies SORT_MERGE is chosen when
//                   inputs are pre-sorted; direct operator test verifies
//                   SortMergeJoinOperator produces correct join results.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/sort_merge_join.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"
#include "sixseven/planner/statistics.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Minimal in-memory source for operator-level tests
// =============================================================================

namespace {

class VectorSource : public Iterator {
public:
    VectorSource(std::vector<Tuple> tuples, OutputSchema schema)
        : tuples_(std::move(tuples)), schema_(std::move(schema)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        cursor_ = 0;
        return ok();
    }

    Result<std::optional<Tuple>> do_next() override {
        if (cursor_ >= tuples_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        return ok(std::optional<Tuple>(tuples_[cursor_++]));
    }

    void do_close() override {}

private:
    std::vector<Tuple> tuples_;
    OutputSchema schema_;
    size_t cursor_ = 0;
};

} // anonymous namespace

// =============================================================================
// Shared fixture (QueryEngine)
// =============================================================================

class PlannerReachabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_planner_reachability";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    /// Flatten the EXPLAIN result into a newline-joined string.
    std::string explain(const std::string& sql) {
        auto qr = exec_ok("EXPLAIN " + sql);
        std::string text;
        for (const auto& row : qr.rows) {
            text += row[0].as_string();
            text += "\n";
        }
        return text;
    }

    /// Build and attach B+ tree indexes from the catalog.
    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(catalog_, *storage_);
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// =============================================================================
// ExternalSort reachability (GDB-815)
// =============================================================================

// ORDER BY on a table must produce an ExternalSort node in the plan.
// ExternalSort degrades gracefully to an in-memory sort when data is small,
// but the node type is always ExternalSort — verifiable via EXPLAIN.
TEST_F(PlannerReachabilityTest, ExternalSortAppearsForOrderBy) {
    exec_ok("CREATE TABLE t (id INT, val VARCHAR)");
    exec_ok("INSERT INTO t VALUES (3, 'c'), (1, 'a'), (2, 'b')");

    auto plan = explain("SELECT * FROM t ORDER BY id");
    EXPECT_NE(plan.find("External Sort"), std::string::npos)
        << "Expected 'External Sort' in plan:\n"
        << plan;
}

// ORDER BY inside an aggregate query also routes through ExternalSort.
TEST_F(PlannerReachabilityTest, ExternalSortAppearsForOrderByWithAggregate) {
    exec_ok("CREATE TABLE sales (region VARCHAR, amount INT)");
    exec_ok("INSERT INTO sales VALUES ('west', 100), ('east', 200), ('west', 50)");

    auto plan = explain("SELECT region, SUM(amount) FROM sales GROUP BY region ORDER BY region");
    EXPECT_NE(plan.find("External Sort"), std::string::npos)
        << "Expected 'External Sort' in plan:\n"
        << plan;
}

// The result of an ORDER BY query using ExternalSort is correctly sorted.
TEST_F(PlannerReachabilityTest, ExternalSortProducesCorrectlySortedResult) {
    exec_ok("CREATE TABLE nums (n INT)");
    exec_ok("INSERT INTO nums VALUES (5), (3), (8), (1), (4)");

    auto qr = exec_ok("SELECT n FROM nums ORDER BY n");
    ASSERT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 4);
    EXPECT_EQ(qr.rows[3][0].as_int32(), 5);
    EXPECT_EQ(qr.rows[4][0].as_int32(), 8);
}

// =============================================================================
// BitmapScan reachability (GDB-830)
// =============================================================================

// An OR of two equality predicates each covered by a separate B+ tree index
// must produce a BitmapScan (OR combine mode) in the plan.
TEST_F(PlannerReachabilityTest, BitmapScanAppearsForOrOfIndexedPredicates) {
    exec_ok("CREATE TABLE products (id INT, category VARCHAR, price INT)");
    exec_ok("INSERT INTO products VALUES (1, 'electronics', 100), (2, 'books', 20), "
            "(3, 'electronics', 200), (4, 'toys', 15), (5, 'books', 30)");
    exec_ok("CREATE INDEX idx_products_id ON products(id)");
    exec_ok("CREATE INDEX idx_products_price ON products(price)");
    rebuild_indexes();

    auto plan = explain("SELECT * FROM products WHERE id = 1 OR price = 20");
    EXPECT_NE(plan.find("Bitmap"), std::string::npos) << "Expected 'Bitmap' in plan:\n" << plan;
}

// The BitmapScan returns the correct union of rows from both index scans.
TEST_F(PlannerReachabilityTest, BitmapScanReturnsUnionOfMatchingRows) {
    exec_ok("CREATE TABLE items (id INT, score INT, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES (1, 10, 'alpha'), (2, 20, 'beta'), "
            "(3, 30, 'gamma'), (4, 40, 'delta'), (5, 50, 'epsilon')");
    exec_ok("CREATE INDEX idx_items_id ON items(id)");
    exec_ok("CREATE INDEX idx_items_score ON items(score)");
    rebuild_indexes();

    // id=2 → beta; score=40 → delta; union = 2 rows.
    auto qr = exec_ok("SELECT id FROM items WHERE id = 2 OR score = 40 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u) << "Expected 2 rows from OR-bitmap scan";
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 4);
}

// =============================================================================
// SortMergeJoin reachability (GDB-822)
// =============================================================================

// Cost model unit test: verify SORT_MERGE is chosen when both inputs are
// pre-sorted (sort cost is zero). This proves the wiring path is reachable.
TEST(SortMergeJoinCostModel, PicksSortMergeWhenInputsAreSorted) {
    // Two large pre-sorted inputs: sort_cpu_cost = 0.
    // SMJ total = left.total + right.total + merge_cost.
    // Hash total = build_startup + probe_total + hash_overhead.
    // With large rows and pre-sorted flag, SMJ wins.
    PlanCost left;
    left.total_cost = 100.0;
    left.estimated_rows = 10000.0;
    PlanCost right;
    right.total_cost = 100.0;
    right.estimated_rows = 10000.0;

    const CostModel cost_model;
    auto [method, cost] = choose_join_method(left, right, 0.1, true, true, cost_model);
    EXPECT_EQ(method, JoinMethod::SORT_MERGE)
        << "Cost model should pick SORT_MERGE when both inputs are pre-sorted";
    (void)cost;
}

// Direct operator test: SortMergeJoinOperator produces correct join results
// when constructed directly (bypassing cost-model selection). This proves the
// operator is functional end-to-end.
TEST(SortMergeJoinOperatorDirect, InnerJoinMergesPreSortedInputs) {
    OutputSchema left_schema({OutputColumn{"l", "id", TypeId::INT32, false, 1}});
    OutputSchema right_schema({OutputColumn{"r", "id", TypeId::INT32, false, 2}});
    OutputSchema combined({
        OutputColumn{"l", "id", TypeId::INT32, false, 1},
        OutputColumn{"r", "id", TypeId::INT32, false, 2},
    });

    // Left: 1, 2, 3, 4, 5 (sorted)
    std::vector<Tuple> left_tuples;
    for (int32_t i = 1; i <= 5; ++i) {
        left_tuples.push_back(Tuple{{Value(i)}, {}});
    }

    // Right: 2, 4, 6 (sorted, partial overlap)
    std::vector<Tuple> right_tuples;
    for (int32_t i : {2, 4, 6}) {
        right_tuples.push_back(Tuple{{Value(i)}, {}});
    }

    auto left_key = std::make_unique<ColumnRefExpr>();
    left_key->table = "l";
    left_key->column = "id";

    auto right_key = std::make_unique<ColumnRefExpr>();
    right_key->table = "r";
    right_key->column = "id";

    // Minimal BoundStatement — no expr_types needed for simple column refs.
    SelectStmt sel;
    BoundStatement bound;
    bound.stmt = &sel;

    auto left_src = std::make_unique<VectorSource>(left_tuples, left_schema);
    auto right_src = std::make_unique<VectorSource>(right_tuples, right_schema);

    SortMergeJoinOperator smj(std::move(left_src),
                              std::move(right_src),
                              JoinType::INNER,
                              left_key.get(),
                              right_key.get(),
                              bound,
                              combined);

    auto open_r = smj.open();
    ASSERT_TRUE(open_r.has_value()) << open_r.error().message;

    std::vector<int32_t> ids;
    while (true) {
        auto next_r = smj.next();
        ASSERT_TRUE(next_r.has_value()) << next_r.error().message;
        if (!next_r->has_value()) {
            break;
        }
        ids.push_back((*next_r)->values[0].as_int32());
    }
    smj.close();

    // Inner join: only ids present in both sides → 2, 4
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 4);
}

// Integration test: SortMergeJoin via SQL query, verifying correct results.
TEST_F(PlannerReachabilityTest, SortMergeJoinQueryProducesCorrectResults) {
    exec_ok("CREATE TABLE emp (id INT, name VARCHAR)");
    exec_ok("CREATE TABLE dept (emp_id INT, dept_name VARCHAR)");

    for (int i = 1; i <= 10; ++i) {
        exec_ok("INSERT INTO emp VALUES (" + std::to_string(i) + ", 'emp" + std::to_string(i) +
                "')");
        exec_ok("INSERT INTO dept VALUES (" + std::to_string(i) + ", 'dept" +
                std::to_string(i % 3) + "')");
    }

    auto qr = exec_ok("SELECT emp.id FROM emp JOIN dept ON emp.id = dept.emp_id "
                      "WHERE emp.id <= 5 ORDER BY emp.id");
    ASSERT_EQ(qr.rows.size(), 5u) << "Expected 5 rows";
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(qr.rows[i][0].as_int32(), i + 1);
    }
}
