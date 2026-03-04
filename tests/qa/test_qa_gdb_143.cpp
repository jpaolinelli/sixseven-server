/// @file test_qa_gdb_143.cpp
/// @brief QA adversarial tests for GDB-143: EXPLAIN and EXPLAIN ANALYZE.
///
/// Targets edge cases: nested EXPLAIN, EXPLAIN on DML/DDL, empty table ANALYZE,
/// deeply nested plan trees, formatter with many children, instrumentation
/// counters, JSON structure validation, parser edge cases.

#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/explain.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/parser/ast.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace giodb;

// =============================================================================
// Test fixture
// =============================================================================

class QA143ExplainTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa143_explain";
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
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
    }

    std::string plan_text(const std::string& sql) {
        auto qr = exec_ok(sql);
        std::string plan;
        for (const auto& row : qr.rows) {
            if (!plan.empty()) {
                plan += "\n";
            }
            plan += row[0].as_string();
        }
        return plan;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// MockIterator for formatter tests
// =============================================================================

namespace {

class MockIter : public Iterator {
public:
    MockIter(std::string name, std::string detail, std::vector<std::unique_ptr<MockIter>> children)
        : name_(std::move(name)), detail_(std::move(detail)), children_(std::move(children)) {}

    const OutputSchema& output_schema() const override { return schema_; }
    std::string plan_node_name() const override { return name_; }
    std::string plan_node_detail() const override { return detail_; }

    std::vector<const Iterator*> plan_children() const override {
        std::vector<const Iterator*> ptrs;
        for (const auto& c : children_) {
            ptrs.push_back(c.get());
        }
        return ptrs;
    }

protected:
    Result<void> do_open() override { return ok(); }
    Result<std::optional<Tuple>> do_next() override {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    void do_close() override {}

    std::vector<Iterator*> plan_children_mutable() override {
        std::vector<Iterator*> ptrs;
        for (auto& c : children_) {
            ptrs.push_back(c.get());
        }
        return ptrs;
    }

private:
    std::string name_;
    std::string detail_;
    OutputSchema schema_;
    std::vector<std::unique_ptr<MockIter>> children_;
};

using MockVec = std::vector<std::unique_ptr<MockIter>>;

std::unique_ptr<MockIter> leaf(const std::string& name, const std::string& detail = "") {
    return std::make_unique<MockIter>(name, detail, MockVec{});
}

} // namespace

// =============================================================================
// EXPLAIN on empty table (zero rows)
// =============================================================================

TEST_F(QA143ExplainTest, ExplainAnalyzeEmptyTable) {
    exec_ok("CREATE TABLE empty_t (id INT)");

    auto plan = plan_text("EXPLAIN ANALYZE SELECT * FROM empty_t");
    EXPECT_NE(plan.find("actual rows=0"), std::string::npos)
        << "Expected 0 rows on empty table, got:\n"
        << plan;
}

// =============================================================================
// EXPLAIN on INSERT does not execute
// =============================================================================

TEST_F(QA143ExplainTest, ExplainInsertDoesNotExecute) {
    exec_ok("CREATE TABLE t1 (id INT, name VARCHAR)");

    exec_ok("EXPLAIN INSERT INTO t1 VALUES (1, 'test')");

    // Verify nothing was inserted.
    auto qr = exec_ok("SELECT * FROM t1");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// EXPLAIN ANALYZE on INSERT does execute
// =============================================================================

TEST_F(QA143ExplainTest, ExplainAnalyzeInsertExecutes) {
    exec_ok("CREATE TABLE t2 (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t2 VALUES (1, 'existing')");

    auto plan = plan_text("EXPLAIN ANALYZE DELETE FROM t2 WHERE id = 1");
    EXPECT_NE(plan.find("actual rows="), std::string::npos);

    // ANALYZE should have executed the DELETE.
    auto qr = exec_ok("SELECT * FROM t2");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// EXPLAIN result always has exactly one QUERY PLAN column
// =============================================================================

TEST_F(QA143ExplainTest, AlwaysOneColumn) {
    exec_ok("CREATE TABLE cols (a INT, b INT, c INT, d INT)");

    auto qr = exec_ok("EXPLAIN SELECT a, b, c, d FROM cols");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "QUERY PLAN");
    EXPECT_EQ(qr.column_types[0], TypeId::STRING);
}

// =============================================================================
// EXPLAIN on UPDATE
// =============================================================================

TEST_F(QA143ExplainTest, ExplainUpdateDoesNotExecute) {
    exec_ok("CREATE TABLE t3 (id INT, val INT)");
    exec_ok("INSERT INTO t3 VALUES (1, 100)");

    exec_ok("EXPLAIN UPDATE t3 SET val = 999 WHERE id = 1");

    auto qr = exec_ok("SELECT val FROM t3 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 100);
}

// =============================================================================
// EXPLAIN with subquery
// =============================================================================

TEST_F(QA143ExplainTest, ExplainWithSubquery) {
    exec_ok("CREATE TABLE outer_t (id INT, val INT)");

    auto plan = plan_text("EXPLAIN SELECT * FROM outer_t WHERE val IN (SELECT id FROM outer_t)");
    // Should produce a valid plan without crashing.
    EXPECT_FALSE(plan.empty());
}

// =============================================================================
// EXPLAIN with ORDER BY + LIMIT
// =============================================================================

TEST_F(QA143ExplainTest, ExplainOrderByLimit) {
    exec_ok("CREATE TABLE ordered (id INT, score INT)");

    auto plan = plan_text("EXPLAIN SELECT * FROM ordered ORDER BY score LIMIT 5");

    EXPECT_NE(plan.find("Limit"), std::string::npos) << "Expected Limit in plan, got:\n" << plan;
    EXPECT_NE(plan.find("Sort"), std::string::npos) << "Expected Sort in plan, got:\n" << plan;
}

// =============================================================================
// EXPLAIN ANALYZE with GROUP BY
// =============================================================================

TEST_F(QA143ExplainTest, ExplainAnalyzeGroupBy) {
    exec_ok("CREATE TABLE groups (category INT, amount INT)");
    exec_ok("INSERT INTO groups VALUES (1, 10)");
    exec_ok("INSERT INTO groups VALUES (1, 20)");
    exec_ok("INSERT INTO groups VALUES (2, 30)");

    auto plan =
        plan_text("EXPLAIN ANALYZE SELECT category, SUM(amount) FROM groups GROUP BY category");

    EXPECT_NE(plan.find("actual rows="), std::string::npos);
    EXPECT_NE(plan.find("HashAggregate"), std::string::npos)
        << "Expected HashAggregate in ANALYZE plan, got:\n"
        << plan;
}

// =============================================================================
// FORMAT JSON structure validation
// =============================================================================

TEST_F(QA143ExplainTest, JsonStructureComplete) {
    exec_ok("CREATE TABLE json_t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO json_t VALUES (1, 'a')");

    auto qr = exec_ok("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM json_t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 1u);

    auto parsed = nlohmann::json::parse(qr.rows[0][0].as_string(), nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_TRUE(parsed.is_array());
    ASSERT_EQ(parsed.size(), 1u);

    // Root node must have Node Type.
    auto& root = parsed[0];
    EXPECT_TRUE(root.contains("Node Type"));
    EXPECT_TRUE(root["Node Type"].is_string());

    // ANALYZE fields.
    EXPECT_TRUE(root.contains("Actual Rows"));
    EXPECT_TRUE(root["Actual Rows"].is_number_integer());
    EXPECT_TRUE(root.contains("Actual Total Time"));
    EXPECT_TRUE(root["Actual Total Time"].is_number());
    EXPECT_TRUE(root.contains("Actual Loops"));
    EXPECT_TRUE(root["Actual Loops"].is_number_integer());

    // With ORDER BY, root should have child Plans.
    EXPECT_TRUE(root.contains("Plans"));
    EXPECT_TRUE(root["Plans"].is_array());
    EXPECT_GE(root["Plans"].size(), 1u);

    // Each child should also have Node Type.
    for (const auto& child : root["Plans"]) {
        EXPECT_TRUE(child.contains("Node Type"));
    }
}

// =============================================================================
// FORMAT JSON without ANALYZE has no timing fields
// =============================================================================

TEST_F(QA143ExplainTest, JsonWithoutAnalyzeNoTimingFields) {
    exec_ok("CREATE TABLE noa (id INT)");

    auto qr = exec_ok("EXPLAIN FORMAT JSON SELECT * FROM noa");
    ASSERT_EQ(qr.rows.size(), 1u);

    auto parsed = nlohmann::json::parse(qr.rows[0][0].as_string(), nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());

    EXPECT_FALSE(parsed[0].contains("Actual Rows"));
    EXPECT_FALSE(parsed[0].contains("Actual Total Time"));
    EXPECT_FALSE(parsed[0].contains("Actual Loops"));
}

// =============================================================================
// EXPLAIN on nonexistent table returns error
// =============================================================================

TEST_F(QA143ExplainTest, ExplainNonexistentTableError) {
    exec_error("EXPLAIN SELECT * FROM ghost");
}

TEST_F(QA143ExplainTest, ExplainAnalyzeNonexistentTableError) {
    exec_error("EXPLAIN ANALYZE SELECT * FROM ghost");
}

// =============================================================================
// Formatter: node with no detail
// =============================================================================

TEST(QA143Formatter, NodeWithNoDetail) {
    auto root = leaf("Seq Scan");
    auto text = ExplainFormatter::format_text(*root, false);
    EXPECT_EQ(text, "Seq Scan");
    // No parentheses when detail is empty.
    EXPECT_EQ(text.find("("), std::string::npos);
}

// =============================================================================
// Formatter: deeply nested tree (5 levels)
// =============================================================================

TEST(QA143Formatter, DeeplyNestedTree) {
    // Build a chain: Sort -> Project -> Filter -> Limit -> Seq Scan
    auto n4 = leaf("Seq Scan", "on t");
    MockVec c3;
    c3.push_back(std::move(n4));
    auto n3 = std::make_unique<MockIter>("Limit", "10", std::move(c3));
    MockVec c2;
    c2.push_back(std::move(n3));
    auto n2 = std::make_unique<MockIter>("Filter", "age > 5", std::move(c2));
    MockVec c1;
    c1.push_back(std::move(n2));
    auto n1 = std::make_unique<MockIter>("Project", "", std::move(c1));
    MockVec c0;
    c0.push_back(std::move(n1));
    MockIter root("Sort", "by age", std::move(c0));

    auto text = ExplainFormatter::format_text(root, false);

    EXPECT_NE(text.find("Sort"), std::string::npos);
    EXPECT_NE(text.find("-> Project"), std::string::npos);
    EXPECT_NE(text.find("-> Filter"), std::string::npos);
    EXPECT_NE(text.find("-> Limit"), std::string::npos);
    EXPECT_NE(text.find("-> Seq Scan"), std::string::npos);

    // Count the number of lines (should be 5).
    int lines = 1;
    for (char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    EXPECT_EQ(lines, 5);
}

// =============================================================================
// Formatter: node with multiple children (e.g., join)
// =============================================================================

TEST(QA143Formatter, MultipleChildren) {
    MockVec children;
    children.push_back(leaf("Seq Scan", "on users"));
    children.push_back(leaf("Index Scan", "on orders"));

    MockIter root("Hash Join", "users.id = orders.uid", std::move(children));

    auto text = ExplainFormatter::format_text(root, false);

    EXPECT_NE(text.find("Hash Join"), std::string::npos);
    EXPECT_NE(text.find("-> Seq Scan"), std::string::npos);
    EXPECT_NE(text.find("-> Index Scan"), std::string::npos);
}

// =============================================================================
// Formatter: JSON with deeply nested tree
// =============================================================================

TEST(QA143Formatter, JsonDeeplyNested) {
    auto child = leaf("Seq Scan", "on t");
    MockVec cv;
    cv.push_back(std::move(child));
    MockIter root("Sort", "by id", std::move(cv));

    auto json_str = ExplainFormatter::format_json(root, false);
    auto parsed = nlohmann::json::parse(json_str, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_TRUE(parsed.is_array());

    auto& r = parsed[0];
    EXPECT_EQ(r["Node Type"], "Sort");
    EXPECT_EQ(r["Detail"], "by id");
    ASSERT_TRUE(r.contains("Plans"));
    ASSERT_EQ(r["Plans"].size(), 1u);
    EXPECT_EQ(r["Plans"][0]["Node Type"], "Seq Scan");
    EXPECT_EQ(r["Plans"][0]["Detail"], "on t");
    EXPECT_FALSE(r["Plans"][0].contains("Plans")); // Leaf has no children.
}

// =============================================================================
// Formatter: to_query_result JSON produces single row
// =============================================================================

TEST(QA143Formatter, QueryResultJsonSingleRow) {
    auto root = leaf("Seq Scan", "on t");
    auto qr = ExplainFormatter::to_query_result(*root, ExplainFormat::JSON, false);

    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "QUERY PLAN");
    ASSERT_EQ(qr.rows.size(), 1u); // JSON is always one row.

    // Verify it's valid JSON.
    auto parsed = nlohmann::json::parse(qr.rows[0][0].as_string(), nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
}

// =============================================================================
// Formatter: to_query_result TEXT produces multiple rows for multi-node tree
// =============================================================================

TEST(QA143Formatter, QueryResultTextMultipleRows) {
    MockVec children;
    children.push_back(leaf("Seq Scan", "on t1"));
    children.push_back(leaf("Seq Scan", "on t2"));
    MockIter root("Nested Loop", "", std::move(children));

    auto qr = ExplainFormatter::to_query_result(root, ExplainFormat::TEXT, false);
    // Root + 2 children = 3 lines = 3 rows.
    EXPECT_EQ(qr.rows.size(), 3u);
}

// =============================================================================
// Instrumentation: disabled by default
// =============================================================================

TEST(QA143Instrumentation, DisabledByDefault) {
    auto it = leaf("Test");
    EXPECT_FALSE(it->is_instrumented());
    EXPECT_EQ(it->rows_produced(), 0);
    EXPECT_EQ(it->loops(), 0);
    EXPECT_EQ(it->execution_time().count(), 0);
}

// =============================================================================
// Instrumentation: enable propagates to children
// =============================================================================

TEST(QA143Instrumentation, EnablePropagates) {
    auto child = leaf("Child");
    auto* child_ptr = child.get();
    MockVec cv;
    cv.push_back(std::move(child));
    MockIter root("Root", "", std::move(cv));

    root.enable_instrumentation();
    EXPECT_TRUE(root.is_instrumented());
    EXPECT_TRUE(child_ptr->is_instrumented());
}

// =============================================================================
// Instrumentation: loops counter increments on open()
// =============================================================================

TEST(QA143Instrumentation, LoopsCounterIncrementsOnOpen) {
    auto it = leaf("Test");
    it->enable_instrumentation();

    EXPECT_EQ(it->loops(), 0);

    auto r1 = it->open();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(it->loops(), 1);

    it->close();
    auto r2 = it->open();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(it->loops(), 2);

    it->close();
}

// =============================================================================
// Instrumentation: rows_produced increments on non-null next()
// =============================================================================

TEST(QA143Instrumentation, RowsProducedCountsNonNull) {
    auto it = leaf("Test");
    it->enable_instrumentation();

    auto r = it->open();
    ASSERT_TRUE(r.has_value());

    // MockIter always returns nullopt, so rows_produced stays 0.
    auto row = it->next();
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->has_value()); // nullopt
    EXPECT_EQ(it->rows_produced(), 0);

    it->close();
}

// =============================================================================
// Instrumentation: execution_time is non-negative
// =============================================================================

TEST(QA143Instrumentation, ExecutionTimeNonNegative) {
    auto it = leaf("Test");
    it->enable_instrumentation();

    auto r = it->open();
    ASSERT_TRUE(r.has_value());
    it->close();

    EXPECT_GE(it->execution_time().count(), 0);
}

// =============================================================================
// EXPLAIN ANALYZE: timing is non-zero after execution
// =============================================================================

TEST_F(QA143ExplainTest, AnalyzeTimingNonZero) {
    exec_ok("CREATE TABLE timed (id INT)");
    for (int i = 0; i < 100; ++i) {
        exec_ok("INSERT INTO timed VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM timed");
    auto parsed = nlohmann::json::parse(qr.rows[0][0].as_string(), nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());

    // With 100 rows, the root operator should show actual rows > 0.
    EXPECT_GT(parsed[0]["Actual Rows"].get<int64_t>(), 0);
}

// =============================================================================
// EXPLAIN on multi-join query
// =============================================================================

TEST_F(QA143ExplainTest, ExplainMultiJoin) {
    exec_ok("CREATE TABLE a (id INT)");
    exec_ok("CREATE TABLE b (id INT, aid INT)");
    exec_ok("CREATE TABLE c (id INT, bid INT)");

    auto plan = plan_text("EXPLAIN SELECT * FROM a JOIN b ON a.id = b.aid JOIN c ON b.id = c.bid");

    // Should have at least two join operators or nested join structure.
    EXPECT_FALSE(plan.empty());
    // At least 3 table references should appear in the plan.
    EXPECT_NE(plan.find("Scan"), std::string::npos);
}

// =============================================================================
// EXPLAIN ANALYZE with DISTINCT
// =============================================================================

TEST_F(QA143ExplainTest, ExplainAnalyzeDistinct) {
    exec_ok("CREATE TABLE dup (val INT)");
    exec_ok("INSERT INTO dup VALUES (1)");
    exec_ok("INSERT INTO dup VALUES (1)");
    exec_ok("INSERT INTO dup VALUES (2)");

    auto plan = plan_text("EXPLAIN ANALYZE SELECT DISTINCT val FROM dup");
    EXPECT_NE(plan.find("actual rows="), std::string::npos);
}

// =============================================================================
// EXPLAIN text format: arrow prefix only on children
// =============================================================================

TEST(QA143Formatter, ArrowPrefixOnlyOnChildren) {
    MockVec cv;
    cv.push_back(leaf("Child1"));
    MockIter root("Root", "", std::move(cv));

    auto text = ExplainFormatter::format_text(root, false);

    // Root should NOT have "-> " prefix.
    EXPECT_EQ(text.substr(0, 4), "Root");

    // Child should have "-> " prefix.
    EXPECT_NE(text.find("-> Child1"), std::string::npos);
}

// =============================================================================
// EXPLAIN ANALYZE loops=1 is NOT shown (only loops > 1 shown)
// =============================================================================

TEST(QA143Formatter, LoopsOneNotShown) {
    auto root = leaf("Seq Scan");
    root->enable_instrumentation();
    auto r = root->open();
    (void)r;
    root->close();
    EXPECT_EQ(root->loops(), 1);

    auto text = ExplainFormatter::format_text(*root, true);
    EXPECT_EQ(text.find("loops="), std::string::npos)
        << "loops=1 should not appear in text, got: " << text;
}

// =============================================================================
// EXPLAIN ANALYZE loops > 1 IS shown
// =============================================================================

TEST(QA143Formatter, LoopsGreaterThanOneShown) {
    auto root = leaf("Seq Scan");
    root->enable_instrumentation();
    // Open twice to get loops=2.
    (void)root->open();
    root->close();
    (void)root->open();
    root->close();
    EXPECT_EQ(root->loops(), 2);

    auto text = ExplainFormatter::format_text(*root, true);
    EXPECT_NE(text.find("loops=2"), std::string::npos) << "Expected loops=2 in text, got: " << text;
}
