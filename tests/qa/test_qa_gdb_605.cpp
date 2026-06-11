/// @file test_qa_gdb_605.cpp
/// QA regression tests for GDB-605: TRAVERSE start key validation for
/// heterogeneous edge types with different PK types (UUID vs INT).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Fixture: UUID source → INT32 target
// ============================================================================

class GDB605_UUIDtoINT : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb605_uuid_int";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE authors (id UUID PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO authors VALUES ('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', 'Alice')");
        exec_ok("INSERT INTO authors VALUES ('11111111-2222-3333-4444-555555555555', 'Bob')");
        exec_ok("INSERT INTO authors VALUES ('00000000-0000-0000-0000-000000000000', 'Charlie')");

        exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("INSERT INTO articles VALUES (1, 'First')");
        exec_ok("INSERT INTO articles VALUES (2, 'Second')");
        exec_ok("INSERT INTO articles VALUES (0, 'Zero')");
        exec_ok("INSERT INTO articles VALUES (-1, 'Negative')");

        exec_ok("CREATE EDGE TYPE wrote FROM authors TO articles");
        exec_ok("LINK authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') TO articles(1) VIA wrote");
        exec_ok("LINK authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') TO articles(2) VIA wrote");
        exec_ok("LINK authors('11111111-2222-3333-4444-555555555555') TO articles(0) VIA wrote");
        exec_ok("LINK authors('00000000-0000-0000-0000-000000000000') TO articles(-1) VIA wrote");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    void exec_fails(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// AC-1: OUT from UUID source validates start key as UUID, not INT
// ============================================================================

TEST_F(GDB605_UUIDtoINT, AC1_OutFromUUIDSource_EnrichedTraversal) {
    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE wrote "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> titles;
    for (const auto& row : qr.rows) {
        titles.push_back(row[0].as_string());
    }
    std::sort(titles.begin(), titles.end());
    EXPECT_EQ(titles[0], "First");
    EXPECT_EQ(titles[1], "Second");
}

TEST_F(GDB605_UUIDtoINT, AC1_OutFromUUIDSource_StandaloneTraverse) {
    auto qr = exec_ok("TRAVERSE wrote FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
                      "DIRECTION OUT FETCH");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// ============================================================================
// AC-2: IN from INT target validates start key as INT, not UUID
// ============================================================================

TEST_F(GDB605_UUIDtoINT, AC2_InFromINTTarget_EnrichedTraversal) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE wrote "
                      "FROM articles(1) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
}

TEST_F(GDB605_UUIDtoINT, AC2_InFromINTTarget_StandaloneTraverse) {
    auto qr = exec_ok("TRAVERSE wrote FROM articles(1) DIRECTION IN FETCH");
    EXPECT_GE(qr.rows.size(), 1u);
}

// ============================================================================
// Boundary: zero and negative INT keys
// ============================================================================

TEST_F(GDB605_UUIDtoINT, Boundary_ZeroINTKey_IN) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE wrote FROM articles(0) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
}

TEST_F(GDB605_UUIDtoINT, Boundary_NegativeINTKey_IN) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE wrote FROM articles(-1) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Charlie");
}

// ============================================================================
// Boundary: nil UUID (all zeros)
// ============================================================================

TEST_F(GDB605_UUIDtoINT, Boundary_NilUUID_OUT) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE wrote "
                      "FROM authors('00000000-0000-0000-0000-000000000000') DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Negative");
}

// ============================================================================
// Error: passing INT where UUID expected should fail clearly
// ============================================================================

TEST_F(GDB605_UUIDtoINT, Error_IntKeyForUUIDSource) {
    // Passing an integer literal for a UUID-PK source table must fail.
    exec_fails("SELECT * FROM TRAVERSE wrote FROM authors(42) DIRECTION OUT");
}

// ============================================================================
// Error: passing UUID string where INT expected should fail
// ============================================================================

TEST_F(GDB605_UUIDtoINT, Error_UUIDKeyForINTTarget) {
    // Passing a UUID string for an INT-PK target table must fail.
    exec_fails("SELECT * FROM TRAVERSE wrote "
               "FROM articles('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') DIRECTION IN");
}

// ============================================================================
// Depth capping: heterogeneous edges must cap depth to 1
// ============================================================================

TEST_F(GDB605_UUIDtoINT, DepthCap_ExplicitDepth10_CappedTo1) {
    // Even with explicit MAX_DEPTH 10, heterogeneous edges cap at 1.
    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE wrote "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
                      "DIRECTION OUT MAX_DEPTH 10");
    ASSERT_EQ(qr.rows.size(), 2u);
    for (const auto& row : qr.rows) {
        // All results must be at depth 1 (no deeper traversal possible).
        auto depth = row[1].type_id() == TypeId::INT32 ? row[1].as_int32() : row[1].as_int64();
        EXPECT_EQ(depth, 1);
    }
}

TEST_F(GDB605_UUIDtoINT, DepthCap_StandaloneTraverse_Depth100) {
    // Standalone TRAVERSE with large depth — should still work (capped to 1).
    auto qr = exec_ok("TRAVERSE wrote FROM authors('11111111-2222-3333-4444-555555555555') "
                      "DIRECTION OUT MAX_DEPTH 100 FETCH");
    EXPECT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// SELECT * enriched traversal verifies correct schema
// ============================================================================

TEST_F(GDB605_UUIDtoINT, SelectStar_OUT_VerifySchema) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE wrote "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') DIRECTION OUT");
    // Target table (articles) columns: id, title + meta columns.
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "title");
    ASSERT_EQ(qr.rows.size(), 2u);
}

TEST_F(GDB605_UUIDtoINT, SelectStar_IN_VerifySchema) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE wrote FROM articles(1) DIRECTION IN");
    // Source table (authors) columns: id, name + meta columns.
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// DIRECTION BOTH on heterogeneous edge with different PK types → error
// ============================================================================

TEST_F(GDB605_UUIDtoINT, BothDirectionRejected) {
    exec_error("SELECT * FROM TRAVERSE wrote "
               "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

// ============================================================================
// Edge mode (EDGES) on heterogeneous edges with different PK types
// ============================================================================

TEST_F(GDB605_UUIDtoINT, EdgeMode_OUT_UUIDSource) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE wrote "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
                      "DIRECTION OUT EDGES");
    ASSERT_EQ(qr.rows.size(), 2u);
    // Edge mode must include __from, __to, __depth columns.
    EXPECT_GE(qr.column_names.size(), 3u);
}

TEST_F(GDB605_UUIDtoINT, EdgeMode_IN_INTTarget) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE wrote "
                      "FROM articles(1) DIRECTION IN EDGES");
    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// Non-existent start key (valid type, no row) → empty result
// ============================================================================

TEST_F(GDB605_UUIDtoINT, NonExistentUUID_EmptyResult) {
    auto qr = exec_ok("SELECT title FROM TRAVERSE wrote "
                      "FROM authors('99999999-9999-9999-9999-999999999999') DIRECTION OUT");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(GDB605_UUIDtoINT, NonExistentINT_EmptyResult) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE wrote FROM articles(999) DIRECTION IN");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ============================================================================
// SHORTEST PATH with heterogeneous edges
// ============================================================================

TEST_F(GDB605_UUIDtoINT, ShortestPath_UUIDtoINT) {
    // SHORTEST PATH from UUID source to INT target.
    auto qr = exec_ok("SHORTEST PATH "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
                      "TO articles(1) VIA wrote");
    // Should find a 1-hop path.
    EXPECT_GE(qr.rows.size(), 1u);
}

TEST_F(GDB605_UUIDtoINT, ShortestPath_NonExistentTarget) {
    auto qr = exec_ok("SHORTEST PATH "
                      "FROM authors('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
                      "TO articles(999) VIA wrote");
    // No path exists — empty result.
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ============================================================================
// Fixture: INT64 source → UUID target (reverse direction of the main fixture)
// ============================================================================

class GDB605_INTtoUUID : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb605_int_uuid";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Reverse: INT source → UUID target.
        exec_ok("CREATE TABLE departments (id INT PRIMARY KEY, dept_name VARCHAR)");
        exec_ok("INSERT INTO departments VALUES (100, 'Engineering')");
        exec_ok("INSERT INTO departments VALUES (200, 'Marketing')");

        exec_ok("CREATE TABLE employees (id UUID PRIMARY KEY, emp_name VARCHAR)");
        exec_ok("INSERT INTO employees VALUES ('abcdef01-2345-6789-abcd-ef0123456789', 'Eve')");
        exec_ok("INSERT INTO employees VALUES ('fedcba98-7654-3210-fedc-ba9876543210', 'Frank')");

        exec_ok("CREATE EDGE TYPE employs FROM departments TO employees");
        exec_ok("LINK departments(100) TO employees('abcdef01-2345-6789-abcd-ef0123456789') VIA employs");
        exec_ok("LINK departments(100) TO employees('fedcba98-7654-3210-fedc-ba9876543210') VIA employs");
        exec_ok("LINK departments(200) TO employees('fedcba98-7654-3210-fedc-ba9876543210') VIA employs");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_fails(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// INT source OUT → UUID target nodes
TEST_F(GDB605_INTtoUUID, OutFromINTSource) {
    auto qr = exec_ok("SELECT emp_name FROM TRAVERSE employs FROM departments(100) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Eve");
    EXPECT_EQ(names[1], "Frank");
}

// UUID target IN → INT source nodes
TEST_F(GDB605_INTtoUUID, InFromUUIDTarget) {
    auto qr = exec_ok("SELECT dept_name FROM TRAVERSE employs "
                      "FROM employees('fedcba98-7654-3210-fedc-ba9876543210') DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 2u);
    std::vector<std::string> depts;
    for (const auto& row : qr.rows) {
        depts.push_back(row[0].as_string());
    }
    std::sort(depts.begin(), depts.end());
    EXPECT_EQ(depts[0], "Engineering");
    EXPECT_EQ(depts[1], "Marketing");
}

// Standalone TRAVERSE from INT source
TEST_F(GDB605_INTtoUUID, Standalone_OutFromINT) {
    auto qr = exec_ok("TRAVERSE employs FROM departments(100) DIRECTION OUT FETCH");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// Standalone TRAVERSE IN from UUID target
TEST_F(GDB605_INTtoUUID, Standalone_InFromUUID) {
    auto qr = exec_ok("TRAVERSE employs "
                      "FROM employees('abcdef01-2345-6789-abcd-ef0123456789') "
                      "DIRECTION IN FETCH");
    EXPECT_GE(qr.rows.size(), 1u);
}

// Wrong type errors
TEST_F(GDB605_INTtoUUID, Error_UUIDKeyForINTSource) {
    exec_fails("SELECT * FROM TRAVERSE employs "
               "FROM departments('abcdef01-2345-6789-abcd-ef0123456789') DIRECTION OUT");
}

TEST_F(GDB605_INTtoUUID, Error_IntKeyForUUIDTarget) {
    exec_fails("SELECT * FROM TRAVERSE employs FROM employees(100) DIRECTION IN");
}

// SHORTEST PATH with INT→UUID
TEST_F(GDB605_INTtoUUID, ShortestPath_INTtoUUID) {
    auto qr = exec_ok("SHORTEST PATH "
                      "FROM departments(100) "
                      "TO employees('abcdef01-2345-6789-abcd-ef0123456789') VIA employs");
    EXPECT_GE(qr.rows.size(), 1u);
}

// ============================================================================
// Fixture: Multiple heterogeneous edge types on the same tables
// ============================================================================

class GDB605_MultiEdge : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb605_multi";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, pname VARCHAR)");
        exec_ok("INSERT INTO people VALUES ('aaaa0000-0000-0000-0000-000000000001', 'Alice')");

        exec_ok("CREATE TABLE items (id INT PRIMARY KEY, iname VARCHAR)");
        exec_ok("INSERT INTO items VALUES (1, 'Widget')");
        exec_ok("INSERT INTO items VALUES (2, 'Gadget')");

        // Two different edge types between the same tables.
        exec_ok("CREATE EDGE TYPE owns FROM people TO items");
        exec_ok("CREATE EDGE TYPE likes FROM people TO items");

        exec_ok("LINK people('aaaa0000-0000-0000-0000-000000000001') TO items(1) VIA owns");
        exec_ok("LINK people('aaaa0000-0000-0000-0000-000000000001') TO items(2) VIA likes");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(GDB605_MultiEdge, DifferentEdgeTypesReturnCorrectResults) {
    auto qr1 = exec_ok("SELECT iname FROM TRAVERSE owns "
                       "FROM people('aaaa0000-0000-0000-0000-000000000001') DIRECTION OUT");
    ASSERT_EQ(qr1.rows.size(), 1u);
    EXPECT_EQ(qr1.rows[0][0].as_string(), "Widget");

    auto qr2 = exec_ok("SELECT iname FROM TRAVERSE likes "
                       "FROM people('aaaa0000-0000-0000-0000-000000000001') DIRECTION OUT");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_string(), "Gadget");
}

TEST_F(GDB605_MultiEdge, InDirection_BothEdgeTypes) {
    auto qr1 = exec_ok("SELECT pname FROM TRAVERSE owns FROM items(1) DIRECTION IN");
    ASSERT_EQ(qr1.rows.size(), 1u);
    EXPECT_EQ(qr1.rows[0][0].as_string(), "Alice");

    auto qr2 = exec_ok("SELECT pname FROM TRAVERSE likes FROM items(2) DIRECTION IN");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_string(), "Alice");
}

// ============================================================================
// Stress: many edges from a single UUID source
// ============================================================================

class GDB605_Stress : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb605_stress";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE src (id UUID PRIMARY KEY, label VARCHAR)");
        exec_ok("INSERT INTO src VALUES ('ffffffff-ffff-ffff-ffff-ffffffffffff', 'Source')");

        exec_ok("CREATE TABLE tgt (id INT PRIMARY KEY, val VARCHAR)");
        exec_ok("CREATE EDGE TYPE connects FROM src TO tgt");

        // Insert 100 target nodes and link them.
        for (int i = 0; i < 100; ++i) {
            exec_ok("INSERT INTO tgt VALUES (" + std::to_string(i) + ", 'V" +
                    std::to_string(i) + "')");
            exec_ok("LINK src('ffffffff-ffff-ffff-ffff-ffffffffffff') TO tgt(" +
                    std::to_string(i) + ") VIA connects");
        }
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(GDB605_Stress, ManyEdges_OutFromUUID) {
    auto qr = exec_ok("SELECT val FROM TRAVERSE connects "
                      "FROM src('ffffffff-ffff-ffff-ffff-ffffffffffff') DIRECTION OUT");
    EXPECT_EQ(qr.rows.size(), 100u);
}

TEST_F(GDB605_Stress, ManyEdges_InFromINT_Multiple) {
    // Pick a target that has exactly one incoming edge.
    auto qr = exec_ok("SELECT label FROM TRAVERSE connects FROM tgt(50) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Source");
}

} // namespace
} // namespace sixseven
