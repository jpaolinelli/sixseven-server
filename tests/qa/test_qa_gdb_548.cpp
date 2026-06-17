/// @file test_qa_gdb_548.cpp
/// QA adversarial tests for GDB-548: Fix binder to expose algorithm TVF output
/// columns to scope.
///
/// Verifies the fix and adversarially tests:
///   AC1: SELECT * from algorithm TVFs resolves all output columns (not 0).
///   AC2: Named column references (SELECT node_id) work at bind time.
///   AC3: Qualified column references (SELECT pr.node_id FROM ... AS pr) work.
///   AC4: WHERE, ORDER BY on algorithm output columns work.
///
/// Adversarial categories: edge cases, boundary values, error paths,
/// complex query patterns, E2E regression, type preservation.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Part 1: Binder-level adversarial tests
// ============================================================================

class QA_GDB548_Binder : public ::testing::Test {
protected:
    Catalog catalog;
    AlgorithmRegistry registry;
    std::unique_ptr<Binder> binder;

    void SetUp() override {
        // Table: users(id INT32, name STRING)
        {
            TableSchema s;
            s.name = "users";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "name", TypeId::STRING, true, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value());
        }

        // Algorithm: pagerank — 2 output columns.
        {
            AlgorithmDef def;
            def.name = "pagerank";
            def.params = {
                {"damping", TypeId::FLOAT64, false, Value(0.85)},
                {"iterations", TypeId::INT64, false, Value(static_cast<int64_t>(20))},
            };
            def.output_columns = {
                {"node_id", TypeId::INT64, false},
                {"rank", TypeId::FLOAT64, false},
            };
            auto r = registry.register_algorithm(std::move(def), nullptr);
            ASSERT_TRUE(r.has_value());
        }

        // Algorithm: community_detection — 3 output columns, mixed nullability.
        {
            AlgorithmDef def;
            def.name = "community_detection";
            def.output_columns = {
                {"node_id", TypeId::INT64, false},
                {"community_id", TypeId::INT64, false},
                {"modularity", TypeId::FLOAT64, true},
            };
            auto r = registry.register_algorithm(std::move(def), nullptr);
            ASSERT_TRUE(r.has_value());
        }

        // Algorithm: single_col — 1 output column.
        {
            AlgorithmDef def;
            def.name = "single_col";
            def.output_columns = {
                {"node_id", TypeId::INT64, false},
            };
            auto r = registry.register_algorithm(std::move(def), nullptr);
            ASSERT_TRUE(r.has_value());
        }

        // Algorithm: many_cols -- 11 output columns with diverse types.
        // node_id INT64 is prepended as [0] to satisfy the node_id-first requirement;
        // the original type-coverage columns follow as [1]..[10].
        {
            AlgorithmDef def;
            def.name = "many_cols";
            def.output_columns = {
                {"node_id", TypeId::INT64, false},       // [0] required first column
                {"col_int8", TypeId::INT8, false},       // [1]
                {"col_int16", TypeId::INT16, false},     // [2]
                {"col_int32", TypeId::INT32, true},      // [3]
                {"col_int64", TypeId::INT64, false},     // [4]
                {"col_float32", TypeId::FLOAT32, true},  // [5]
                {"col_float64", TypeId::FLOAT64, false}, // [6]
                {"col_string", TypeId::STRING, true},    // [7]
                {"col_bool", TypeId::BOOL, false},       // [8]
                {"col_date", TypeId::DATE, true},        // [9]
                {"col_uuid", TypeId::UUID, false},       // [10]
            };
            auto r = registry.register_algorithm(std::move(def), nullptr);
            ASSERT_TRUE(r.has_value());
        }

        // Algorithm: no_output -- 0 output columns.
        // Registration MUST FAIL: empty output_columns violates the node_id-first requirement.
        // Tests that previously used no_output are updated to reflect bind failure.
        {
            AlgorithmDef def;
            def.name = "no_output";
            auto r = registry.register_algorithm(std::move(def), nullptr);
            ASSERT_FALSE(r.has_value());
            EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
        }
        binder = std::make_unique<Binder>(catalog, default_database_id, &registry);
    }

    StmtPtr parse(const std::string& sql) {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens.has_value()) {
            ADD_FAILURE() << "Lex failed: " << tokens.error().message;
            return nullptr;
        }
        Parser parser(std::move(*tokens));
        auto result = parser.parse();
        if (!result.has_value()) {
            ADD_FAILURE() << "Parse failed: " << result.error().message;
            return nullptr;
        }
        return std::move(*result);
    }

    BoundStatement bind_ok(const std::string& sql) {
        auto stmt = parse(sql);
        if (!stmt) {
            return {};
        }
        auto result = binder->bind(*stmt);
        if (!result.has_value()) {
            ADD_FAILURE() << "Bind failed: " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void bind_error(const std::string& sql, StatusCode expected) {
        auto stmt = parse(sql);
        if (!stmt) {
            ADD_FAILURE() << "Parse failed unexpectedly";
            return;
        }
        auto result = binder->bind(*stmt);
        EXPECT_FALSE(result.has_value()) << "Expected bind error but succeeded";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << status_code_name(expected) << " but got "
                << status_code_name(result.error().code) << ": " << result.error().message;
        }
    }
};

// ---------------------------------------------------------------------------
// AC1: SELECT * resolves all output columns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, AC1_SelectStarResolvesColumns) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(bound.output_columns[0].nullable);
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(bound.output_columns[1].nullable);
}

TEST_F(QA_GDB548_Binder, AC1_SelectStarThreeColumns) {
    auto bound = bind_ok("SELECT * FROM community_detection('follows')");
    ASSERT_EQ(bound.output_columns.size(), 3);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "community_id");
    EXPECT_EQ(bound.output_columns[2].column_name, "modularity");
    EXPECT_TRUE(bound.output_columns[2].nullable);
}

// ---------------------------------------------------------------------------
// AC2: Named column references work
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, AC2_NamedColumnReference) {
    auto bound = bind_ok("SELECT node_id, rank FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
}

TEST_F(QA_GDB548_Binder, AC2_UnknownColumnFails) {
    bind_error("SELECT nonexistent FROM pagerank('knows')", StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// AC3: Qualified column references with alias
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, AC3_QualifiedColumnWithAlias) {
    auto bound = bind_ok("SELECT pr.node_id, pr.rank FROM pagerank('knows') AS pr");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].table_name, "pr");
    EXPECT_EQ(bound.output_columns[1].table_name, "pr");
}

TEST_F(QA_GDB548_Binder, AC3_QualifiedStarWithAlias) {
    auto bound = bind_ok("SELECT pr.* FROM pagerank('knows') AS pr");
    ASSERT_EQ(bound.output_columns.size(), 2);
}

// ---------------------------------------------------------------------------
// AC4: WHERE and ORDER BY resolve algorithm columns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, AC4_WhereResolvesAlgorithmColumn) {
    auto bound = bind_ok("SELECT node_id FROM pagerank('knows') WHERE rank > 0.5");
    ASSERT_EQ(bound.output_columns.size(), 1);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
}

TEST_F(QA_GDB548_Binder, AC4_OrderByResolvesAlgorithmColumn) {
    auto bound = bind_ok("SELECT node_id, rank FROM pagerank('knows') ORDER BY rank DESC");
    ASSERT_EQ(bound.output_columns.size(), 2);
}

// ---------------------------------------------------------------------------
// Edge cases: single column, many columns, zero columns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, EdgeCase_SingleColumnAlgorithm) {
    auto bound = bind_ok("SELECT * FROM single_col('e')");
    ASSERT_EQ(bound.output_columns.size(), 1);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
}

TEST_F(QA_GDB548_Binder, EdgeCase_ManyColumnsSelectStar) {
    auto bound = bind_ok("SELECT * FROM many_cols('e')");
    ASSERT_EQ(bound.output_columns.size(), 11);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(bound.output_columns[10].column_name, "col_uuid");
    EXPECT_EQ(bound.output_columns[10].type_id, TypeId::UUID);
}

TEST_F(QA_GDB548_Binder, EdgeCase_ManyColumnsNamedRef) {
    auto bound = bind_ok("SELECT col_string, col_bool FROM many_cols('e')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "col_string");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
    EXPECT_TRUE(bound.output_columns[0].nullable);
    EXPECT_EQ(bound.output_columns[1].column_name, "col_bool");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::BOOL);
    EXPECT_FALSE(bound.output_columns[1].nullable);
}

TEST_F(QA_GDB548_Binder, EdgeCase_ZeroOutputColumnsSelectStar) {
    // no_output failed registration (empty schema violates node_id-first requirement).
    // The binder must therefore reject any query referencing it.
    bind_error("SELECT * FROM no_output('e')", StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Type & nullable preservation
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, TypePreservation_AllTypesCorrect) {
    auto bound = bind_ok("SELECT * FROM many_cols('e')");
    ASSERT_EQ(bound.output_columns.size(), 11);

    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);   // node_id
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::INT8);    // col_int8
    EXPECT_EQ(bound.output_columns[2].type_id, TypeId::INT16);   // col_int16
    EXPECT_EQ(bound.output_columns[3].type_id, TypeId::INT32);   // col_int32
    EXPECT_EQ(bound.output_columns[4].type_id, TypeId::INT64);   // col_int64
    EXPECT_EQ(bound.output_columns[5].type_id, TypeId::FLOAT32); // col_float32
    EXPECT_EQ(bound.output_columns[6].type_id, TypeId::FLOAT64); // col_float64
    EXPECT_EQ(bound.output_columns[7].type_id, TypeId::STRING);  // col_string
    EXPECT_EQ(bound.output_columns[8].type_id, TypeId::BOOL);    // col_bool
    EXPECT_EQ(bound.output_columns[9].type_id, TypeId::DATE);    // col_date
    EXPECT_EQ(bound.output_columns[10].type_id, TypeId::UUID);   // col_uuid
}

TEST_F(QA_GDB548_Binder, NullabilityPreservation) {
    auto bound = bind_ok("SELECT * FROM many_cols('e')");
    ASSERT_EQ(bound.output_columns.size(), 11);

    EXPECT_FALSE(bound.output_columns[0].nullable);  // node_id
    EXPECT_FALSE(bound.output_columns[1].nullable);  // col_int8
    EXPECT_FALSE(bound.output_columns[2].nullable);  // col_int16
    EXPECT_TRUE(bound.output_columns[3].nullable);   // col_int32
    EXPECT_FALSE(bound.output_columns[4].nullable);  // col_int64
    EXPECT_TRUE(bound.output_columns[5].nullable);   // col_float32
    EXPECT_FALSE(bound.output_columns[6].nullable);  // col_float64
    EXPECT_TRUE(bound.output_columns[7].nullable);   // col_string
    EXPECT_FALSE(bound.output_columns[8].nullable);  // col_bool
    EXPECT_TRUE(bound.output_columns[9].nullable);   // col_date
    EXPECT_FALSE(bound.output_columns[10].nullable); // col_uuid
}

// ---------------------------------------------------------------------------
// Ordinal assignment
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, OrdinalAssignment) {
    auto bound = bind_ok("SELECT * FROM community_detection('e')");
    ASSERT_EQ(bound.output_columns.size(), 3);
    EXPECT_EQ(bound.output_columns[0].ordinal, 0);
    EXPECT_EQ(bound.output_columns[1].ordinal, 1);
    EXPECT_EQ(bound.output_columns[2].ordinal, 2);
}

// ---------------------------------------------------------------------------
// Alias vs function name as table_name
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, TableName_UsesAliasWhenProvided) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows') AS pr");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].table_name, "pr");
    EXPECT_EQ(bound.output_columns[1].table_name, "pr");
}

TEST_F(QA_GDB548_Binder, TableName_UsesFunctionNameWhenNoAlias) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].table_name, "pagerank");
    EXPECT_EQ(bound.output_columns[1].table_name, "pagerank");
}

// ---------------------------------------------------------------------------
// Case-insensitive algorithm name lookup
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, CaseInsensitive_UpperCase) {
    auto bound = bind_ok("SELECT * FROM PAGERANK('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
}

TEST_F(QA_GDB548_Binder, CaseInsensitive_MixedCase) {
    auto bound = bind_ok("SELECT * FROM PageRank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
}

// ---------------------------------------------------------------------------
// Error paths: wrong alias qualification
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, ErrorPath_WrongAliasQualification) {
    bind_error("SELECT wrong.node_id FROM pagerank('knows') AS pr", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB548_Binder, ErrorPath_UnqualifiedUnknownColumn) {
    bind_error("SELECT bogus_column FROM pagerank('knows')", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB548_Binder, ErrorPath_QualifiedUnknownColumn) {
    bind_error("SELECT pr.bogus FROM pagerank('knows') AS pr", StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Complex query patterns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, Complex_WhereAndOrderBy) {
    auto bound =
        bind_ok("SELECT node_id FROM pagerank('knows') WHERE rank > 0.1 ORDER BY rank ASC");
    ASSERT_EQ(bound.output_columns.size(), 1);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
}

TEST_F(QA_GDB548_Binder, Complex_ExpressionOnAlgorithmColumn) {
    auto bound = bind_ok("SELECT node_id, rank * 100 FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
}

TEST_F(QA_GDB548_Binder, Complex_AliasedSelectColumn) {
    auto bound = bind_ok("SELECT node_id AS nid, rank AS score FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "nid");
    EXPECT_EQ(bound.output_columns[1].column_name, "score");
}

TEST_F(QA_GDB548_Binder, Complex_LimitOffset) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows') LIMIT 10 OFFSET 5");
    ASSERT_EQ(bound.output_columns.size(), 2);
}

TEST_F(QA_GDB548_Binder, Complex_Distinct) {
    auto bound = bind_ok("SELECT DISTINCT node_id FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 1);
}

TEST_F(QA_GDB548_Binder, Complex_WhereWithCompoundCondition) {
    auto bound = bind_ok("SELECT node_id FROM pagerank('knows') "
                         "WHERE rank > 0.1 AND node_id > 0");
    ASSERT_EQ(bound.output_columns.size(), 1);
}

TEST_F(QA_GDB548_Binder, Complex_OrderByMultipleColumns) {
    auto bound = bind_ok(
        "SELECT * FROM community_detection('e') ORDER BY community_id ASC, modularity DESC");
    ASSERT_EQ(bound.output_columns.size(), 3);
}

// ---------------------------------------------------------------------------
// Graceful degradation without registry
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, GracefulDegradation_NoRegistrySelectStar) {
    Binder no_reg_binder(catalog);
    auto stmt = parse("SELECT * FROM pagerank('knows')");
    ASSERT_NE(stmt, nullptr);
    auto result = no_reg_binder.bind(*stmt);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output_columns.size(), 0);
}

TEST_F(QA_GDB548_Binder, GracefulDegradation_NoRegistryNamedColumn) {
    // Without registry, binder can't resolve named columns — should fail.
    Binder no_reg_binder(catalog);
    auto stmt = parse("SELECT node_id FROM pagerank('knows')");
    ASSERT_NE(stmt, nullptr);
    auto result = no_reg_binder.bind(*stmt);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Named parameters don't affect column resolution
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_Binder, NamedParams_DontAffectColumns) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows', damping := 0.5, iterations := 10)");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
}

// ============================================================================
// Part 2: End-to-end tests through QueryEngine
// ============================================================================

class QA_GDB548_E2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb548";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Register pagerank algorithm with real output.
        {
            AlgorithmDef def;
            def.name = "pagerank";
            def.params = {
                {"damping", TypeId::FLOAT64, false, Value(0.85)},
                {"iterations", TypeId::INT64, false, Value(int64_t{20})},
            };
            def.output_columns = {
                {"node_id", TypeId::INT64, false},
                {"rank", TypeId::FLOAT64, false},
            };

            auto execute = [](const AlgorithmContext& ctx) -> Result<std::vector<AlgorithmRow>> {
                auto it = ctx.named_args.find("damping");
                double damping = 0.85;
                if (it != ctx.named_args.end()) {
                    damping = std::get<double>(it->second.data());
                }
                std::vector<AlgorithmRow> rows;
                rows.push_back({std::vector<Value>{Value(int64_t{1}), Value(damping)}});
                rows.push_back({std::vector<Value>{Value(int64_t{2}), Value(damping * 0.5)}});
                rows.push_back({std::vector<Value>{Value(int64_t{3}), Value(damping * 0.1)}});
                return ok(std::move(rows));
            };

            (void)algo_registry_.register_algorithm(std::move(def), execute);
        }

        // Register community_detection with 3 columns.
        {
            AlgorithmDef def;
            def.name = "community_detection";
            def.output_columns = {
                {"node_id", TypeId::INT64, false},
                {"community_id", TypeId::INT64, false},
                {"modularity", TypeId::FLOAT64, true},
            };
            auto exec = [](const AlgorithmContext& /*ctx*/) -> Result<std::vector<AlgorithmRow>> {
                std::vector<AlgorithmRow> rows;
                rows.push_back(
                    {std::vector<Value>{Value(int64_t{1}), Value(int64_t{0}), Value(0.75)}});
                rows.push_back(
                    {std::vector<Value>{Value(int64_t{2}), Value(int64_t{0}), Value(0.75)}});
                rows.push_back(
                    {std::vector<Value>{Value(int64_t{3}), Value(int64_t{1}), Value(0.60)}});
                return ok(std::move(rows));
            };
            (void)algo_registry_.register_algorithm(std::move(def), exec);
        }

        engine_->set_algorithm_registry(&algo_registry_);

        // Create table and graph.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Charlie')");
        exec_ok("CREATE EDGE TYPE knows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA knows");
        exec_ok("LINK users(2) TO users(3) VIA knows");
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

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected_code);
        }
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    AlgorithmRegistry algo_registry_;
};

// ---------------------------------------------------------------------------
// AC1 E2E: SELECT * returns correct columns and data
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, AC1_SelectStarReturnsColumns) {
    auto result = exec_ok("SELECT * FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.column_names[0], "node_id");
    EXPECT_EQ(result.column_names[1], "rank");
    EXPECT_EQ(result.rows.size(), 3);
}

TEST_F(QA_GDB548_E2E, AC1_SelectStarThreeColumns) {
    auto result = exec_ok("SELECT * FROM community_detection('knows')");
    ASSERT_EQ(result.column_names.size(), 3);
    EXPECT_EQ(result.column_names[0], "node_id");
    EXPECT_EQ(result.column_names[1], "community_id");
    EXPECT_EQ(result.column_names[2], "modularity");
    EXPECT_EQ(result.rows.size(), 3);
}

// ---------------------------------------------------------------------------
// AC2 E2E: Named column references
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, AC2_NamedColumnWorks) {
    auto result = exec_ok("SELECT node_id FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 1);
    EXPECT_EQ(result.column_names[0], "node_id");
    EXPECT_EQ(result.rows.size(), 3);
}

TEST_F(QA_GDB548_E2E, AC2_MultipleNamedColumns) {
    auto result = exec_ok("SELECT node_id, rank FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 2);
}

// ---------------------------------------------------------------------------
// AC3 E2E: Qualified column references
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, AC3_QualifiedWithAlias) {
    auto result = exec_ok("SELECT pr.node_id, pr.rank FROM pagerank('knows') AS pr");
    ASSERT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB548_E2E, AC3_QualifiedWithFunctionName) {
    auto result = exec_ok("SELECT pagerank.node_id FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 1);
}

TEST_F(QA_GDB548_E2E, AC3_QualifiedStarWithAlias) {
    auto result = exec_ok("SELECT pr.* FROM pagerank('knows') AS pr");
    ASSERT_EQ(result.column_names.size(), 2);
}

// ---------------------------------------------------------------------------
// AC4 E2E: WHERE and ORDER BY
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, AC4_WhereOnAlgorithmColumn) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') WHERE node_id = 1");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 1);
}

TEST_F(QA_GDB548_E2E, AC4_WhereOnRank) {
    auto result = exec_ok("SELECT node_id FROM pagerank('knows') WHERE rank > 0.5");
    ASSERT_EQ(result.column_names.size(), 1);
    // With damping=0.85, rows are: (1, 0.85), (2, 0.425), (3, 0.085)
    // rank > 0.5 should match only node_id=1.
    EXPECT_EQ(result.rows.size(), 1);
}

TEST_F(QA_GDB548_E2E, AC4_OrderByDesc) {
    auto result = exec_ok("SELECT node_id, rank FROM pagerank('knows') ORDER BY rank DESC");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 3);
    // First row should have highest rank.
}

TEST_F(QA_GDB548_E2E, AC4_WhereAndOrderByCombined) {
    auto result = exec_ok("SELECT node_id, rank FROM pagerank('knows') "
                          "WHERE rank > 0.1 ORDER BY node_id ASC");
    ASSERT_EQ(result.column_names.size(), 2);
}

// ---------------------------------------------------------------------------
// E2E: Algorithm case insensitivity
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, CaseInsensitive_UpperCase) {
    auto result = exec_ok("SELECT * FROM PAGERANK('knows')");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 3);
}

TEST_F(QA_GDB548_E2E, CaseInsensitive_MixedCase) {
    auto result = exec_ok("SELECT * FROM PageRank('knows')");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 3);
}

// ---------------------------------------------------------------------------
// E2E: Named parameters work with column resolution
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, NamedParams_WithColumnResolution) {
    auto result = exec_ok("SELECT node_id, rank FROM pagerank('knows', damping := 0.5)");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 3);
}

TEST_F(QA_GDB548_E2E, NamedParams_MultipleWithWhere) {
    auto result = exec_ok("SELECT node_id FROM pagerank('knows', damping := 0.9, iterations := 10) "
                          "WHERE rank > 0.0");
    ASSERT_EQ(result.column_names.size(), 1);
}

// ---------------------------------------------------------------------------
// E2E: Error paths still work
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, ErrorPath_UnknownAlgorithm) {
    exec_error("SELECT * FROM nonexistent_algo('knows')", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB548_E2E, ErrorPath_UnknownColumn) {
    exec_error("SELECT bogus FROM pagerank('knows')", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB548_E2E, ErrorPath_WrongAlias) {
    exec_error("SELECT wrong.node_id FROM pagerank('knows') AS pr", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB548_E2E, ErrorPath_MissingEdgeType) {
    exec_error("SELECT * FROM pagerank()", StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// E2E: LIMIT, DISTINCT, expressions
// ---------------------------------------------------------------------------

TEST_F(QA_GDB548_E2E, Limit) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') LIMIT 1");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 1);
}

TEST_F(QA_GDB548_E2E, Expression_ArithmeticOnColumn) {
    auto result = exec_ok("SELECT node_id, rank * 100 FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.rows.size(), 3);
}

TEST_F(QA_GDB548_E2E, ColumnAlias) {
    auto result = exec_ok("SELECT node_id AS nid, rank AS score FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 2);
    EXPECT_EQ(result.column_names[0], "nid");
    EXPECT_EQ(result.column_names[1], "score");
}

} // namespace
} // namespace sixseven
