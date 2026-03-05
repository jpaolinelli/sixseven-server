#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/explain.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/ast.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class ExplainTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_explain";
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

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
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

// =============================================================================
// EXPLAIN basic plan output
// =============================================================================

TEST_F(ExplainTest, ExplainSelectShowsPlanTree) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN SELECT * FROM users");

    // Result should have a single QUERY PLAN column.
    ASSERT_EQ(qr.column_names.size(), 1);
    EXPECT_EQ(qr.column_names[0], "QUERY PLAN");
    EXPECT_EQ(qr.column_types[0], TypeId::STRING);

    // Should have at least one row of plan output.
    ASSERT_GE(qr.rows.size(), 1);

    // First row should contain operator name.
    auto first_line = qr.rows[0][0].as_string();
    // The root of a simple SELECT * FROM users should be Project or Seq Scan.
    bool has_operator = first_line.find("Seq Scan") != std::string::npos ||
                        first_line.find("Project") != std::string::npos;
    EXPECT_TRUE(has_operator) << "Expected plan to contain Seq Scan or Project, got: "
                              << first_line;
}

TEST_F(ExplainTest, ExplainDoesNotExecute) {
    create_test_table();
    insert_test_data();

    // EXPLAIN on a DELETE should NOT actually delete rows.
    exec_ok("EXPLAIN DELETE FROM users WHERE id = 1");

    // Verify all rows still exist.
    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_EQ(qr.rows.size(), 3);
}

TEST_F(ExplainTest, ExplainWithWhereShowsFilter) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN SELECT * FROM users WHERE age > 25");

    // Concatenate all lines.
    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    // Should show either a Filter or a Seq Scan (with pushed-down predicate).
    bool has_scan_or_filter =
        plan.find("Seq Scan") != std::string::npos || plan.find("Filter") != std::string::npos;
    EXPECT_TRUE(has_scan_or_filter) << "Expected plan with Seq Scan or Filter, got:\n" << plan;
}

TEST_F(ExplainTest, ExplainJoinShowsJoinOperator) {
    exec_ok("CREATE TABLE orders (id INT, user_id INT, amount INT)");
    create_test_table();

    auto qr = exec_ok("EXPLAIN SELECT * FROM users JOIN orders ON users.id = orders.user_id");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    // Should show a join operator.
    bool has_join = plan.find("Nested Loop") != std::string::npos ||
                    plan.find("Hash Join") != std::string::npos ||
                    plan.find("Merge Join") != std::string::npos;
    EXPECT_TRUE(has_join) << "Expected plan with join operator, got:\n" << plan;
}

TEST_F(ExplainTest, ExplainSortShowsSortOperator) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN SELECT * FROM users ORDER BY age");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    bool has_sort = plan.find("Sort") != std::string::npos;
    EXPECT_TRUE(has_sort) << "Expected plan with Sort operator, got:\n" << plan;
}

TEST_F(ExplainTest, ExplainAggregateShowsHashAggregate) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN SELECT age, COUNT(*) FROM users GROUP BY age");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    bool has_aggregate = plan.find("HashAggregate") != std::string::npos;
    EXPECT_TRUE(has_aggregate) << "Expected plan with HashAggregate, got:\n" << plan;
}

TEST_F(ExplainTest, ExplainLimitShowsLimitOperator) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN SELECT * FROM users LIMIT 10");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    bool has_limit = plan.find("Limit") != std::string::npos;
    EXPECT_TRUE(has_limit) << "Expected plan with Limit operator, got:\n" << plan;
}

// =============================================================================
// EXPLAIN ANALYZE
// =============================================================================

TEST_F(ExplainTest, ExplainAnalyzeShowsActualRows) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("EXPLAIN ANALYZE SELECT * FROM users");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    // ANALYZE output should show actual rows.
    EXPECT_NE(plan.find("actual rows="), std::string::npos)
        << "Expected ANALYZE to show actual rows, got:\n"
        << plan;
}

TEST_F(ExplainTest, ExplainAnalyzeShowsExecutionTime) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("EXPLAIN ANALYZE SELECT * FROM users");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    // ANALYZE output should show timing.
    EXPECT_NE(plan.find("time="), std::string::npos) << "Expected ANALYZE to show timing, got:\n"
                                                     << plan;
}

TEST_F(ExplainTest, ExplainAnalyzeActualRowCount) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("EXPLAIN ANALYZE SELECT * FROM users");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    // The Seq Scan should produce 3 rows.
    EXPECT_NE(plan.find("actual rows=3"), std::string::npos)
        << "Expected actual rows=3 for 3-row table, got:\n"
        << plan;
}

TEST_F(ExplainTest, ExplainAnalyzeWithFilter) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("EXPLAIN ANALYZE SELECT * FROM users WHERE age > 25");

    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string() + "\n";
    }

    // Should have actual row stats.
    EXPECT_NE(plan.find("actual rows="), std::string::npos)
        << "Expected ANALYZE with filter to show actual rows, got:\n"
        << plan;
}

// =============================================================================
// FORMAT JSON
// =============================================================================

TEST_F(ExplainTest, ExplainFormatJson) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN FORMAT JSON SELECT * FROM users");

    ASSERT_EQ(qr.column_names.size(), 1);
    EXPECT_EQ(qr.column_names[0], "QUERY PLAN");

    // JSON format returns a single row with the full JSON string.
    ASSERT_EQ(qr.rows.size(), 1);

    auto json_str = qr.rows[0][0].as_string();

    // Validate it's parseable JSON.
    auto parsed = nlohmann::json::parse(json_str, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded()) << "Expected valid JSON, got: " << json_str;

    // Should be an array with one plan object.
    EXPECT_TRUE(parsed.is_array());
    EXPECT_EQ(parsed.size(), 1);

    // Plan object should have a Node Type.
    EXPECT_TRUE(parsed[0].contains("Node Type")) << "Expected Node Type in JSON plan";
}

TEST_F(ExplainTest, ExplainAnalyzeFormatJson) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM users");

    ASSERT_EQ(qr.rows.size(), 1);
    auto json_str = qr.rows[0][0].as_string();

    auto parsed = nlohmann::json::parse(json_str, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    EXPECT_TRUE(parsed.is_array());
    EXPECT_EQ(parsed.size(), 1);

    // ANALYZE JSON should include actual stats.
    EXPECT_TRUE(parsed[0].contains("Actual Rows"));
    EXPECT_TRUE(parsed[0].contains("Actual Total Time"));
    EXPECT_TRUE(parsed[0].contains("Actual Loops"));
}

TEST_F(ExplainTest, ExplainJsonWithChildren) {
    create_test_table();

    auto qr = exec_ok("EXPLAIN FORMAT JSON SELECT * FROM users ORDER BY age");

    auto json_str = qr.rows[0][0].as_string();
    auto parsed = nlohmann::json::parse(json_str, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());

    // A sorted query should have child Plans.
    // Walk the tree to find a "Plans" array somewhere.
    std::function<bool(const nlohmann::json&)> has_plans;
    has_plans = [&](const nlohmann::json& node) -> bool {
        if (node.contains("Plans") && node["Plans"].is_array() && !node["Plans"].empty()) {
            return true;
        }
        if (node.contains("Plans")) {
            for (const auto& child : node["Plans"]) {
                if (has_plans(child))
                    return true;
            }
        }
        return false;
    };

    EXPECT_TRUE(has_plans(parsed[0])) << "Expected JSON plan to have child Plans for sorted query";
}

// =============================================================================
// Parser tests for FORMAT
// =============================================================================

TEST_F(ExplainTest, ExplainFormatText) {
    create_test_table();

    // FORMAT TEXT should produce same output as default.
    auto qr_default = exec_ok("EXPLAIN SELECT * FROM users");
    auto qr_text = exec_ok("EXPLAIN FORMAT TEXT SELECT * FROM users");

    EXPECT_EQ(qr_default.rows.size(), qr_text.rows.size());
    for (size_t i = 0; i < qr_default.rows.size(); ++i) {
        EXPECT_EQ(qr_default.rows[i][0].as_string(), qr_text.rows[i][0].as_string());
    }
}

TEST_F(ExplainTest, ExplainAnalyzeFormat) {
    create_test_table();
    insert_test_data();

    // Both ANALYZE and FORMAT should work together.
    auto qr = exec_ok("EXPLAIN ANALYZE FORMAT JSON SELECT * FROM users");
    ASSERT_EQ(qr.rows.size(), 1);

    auto parsed = nlohmann::json::parse(qr.rows[0][0].as_string(), nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    EXPECT_TRUE(parsed[0].contains("Actual Rows"));
}

// =============================================================================
// Error cases
// =============================================================================

TEST_F(ExplainTest, ExplainNonExistentTable) {
    exec_error("EXPLAIN SELECT * FROM nonexistent", StatusCode::NOT_FOUND);
}

// =============================================================================
// ExplainFormatter unit tests (direct API)
// =============================================================================

namespace {

/// Minimal iterator for testing the formatter directly.
class MockIterator : public Iterator {
public:
    MockIterator(std::string name,
                 std::string detail,
                 std::vector<std::unique_ptr<MockIterator>> children)
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
    std::vector<std::unique_ptr<MockIterator>> children_;
};

} // anonymous namespace

TEST_F(ExplainTest, FormatterTextSingleNode) {
    MockIterator root("Seq Scan", "on users", {});
    auto text = ExplainFormatter::format_text(root, false);
    EXPECT_EQ(text, "Seq Scan  (on users)");
}

TEST_F(ExplainTest, FormatterTextWithChildren) {
    std::vector<std::unique_ptr<MockIterator>> children;
    children.push_back(std::make_unique<MockIterator>(
        "Seq Scan", "on users", std::vector<std::unique_ptr<MockIterator>>{}));
    MockIterator root("Sort", "", std::move(children));

    auto text = ExplainFormatter::format_text(root, false);
    EXPECT_NE(text.find("Sort"), std::string::npos);
    EXPECT_NE(text.find("-> Seq Scan"), std::string::npos);
}

TEST_F(ExplainTest, FormatterTextAnalyze) {
    MockIterator root("Seq Scan", "on users", {});
    // Enable instrumentation and simulate execution.
    root.enable_instrumentation();

    auto text = ExplainFormatter::format_text(root, true);
    EXPECT_NE(text.find("actual rows=0"), std::string::npos);
    EXPECT_NE(text.find("time="), std::string::npos);
}

TEST_F(ExplainTest, FormatterJsonSingleNode) {
    MockIterator root("Seq Scan", "on users", {});
    auto json_str = ExplainFormatter::format_json(root, false);

    auto parsed = nlohmann::json::parse(json_str, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    EXPECT_TRUE(parsed.is_array());
    EXPECT_EQ(parsed[0]["Node Type"], "Seq Scan");
    EXPECT_EQ(parsed[0]["Detail"], "on users");
}

TEST_F(ExplainTest, FormatterJsonAnalyze) {
    MockIterator root("Seq Scan", "on users", {});
    root.enable_instrumentation();

    auto json_str = ExplainFormatter::format_json(root, true);

    auto parsed = nlohmann::json::parse(json_str, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    EXPECT_TRUE(parsed[0].contains("Actual Rows"));
    EXPECT_EQ(parsed[0]["Actual Rows"], 0);
    EXPECT_TRUE(parsed[0].contains("Actual Total Time"));
}

TEST_F(ExplainTest, FormatterQueryResult) {
    MockIterator root("Seq Scan", "on users", {});
    auto qr = ExplainFormatter::to_query_result(root, ExplainFormat::TEXT, false);

    EXPECT_EQ(qr.column_names.size(), 1);
    EXPECT_EQ(qr.column_names[0], "QUERY PLAN");
    EXPECT_GE(qr.rows.size(), 1);
}
