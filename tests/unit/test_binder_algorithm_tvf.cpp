#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {

// ===========================================================================
// Test fixture — binder with AlgorithmRegistry for TVF column resolution
// ===========================================================================

class BinderAlgorithmTvfTest : public ::testing::Test {
protected:
    Catalog catalog;
    AlgorithmRegistry registry;
    std::unique_ptr<Binder> binder;

    void SetUp() override {
        init_test_catalog(catalog);
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

        // Register a "pagerank" algorithm with output columns.
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

        // Register a "community_detection" algorithm with 3 output columns.
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

        binder = std::make_unique<Binder>(catalog, default_database_id, &registry);
    }

    /// Parse a SQL string into a Stmt, assert success.
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

    /// Parse and bind, assert success. Returns the BoundStatement.
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

    /// Parse and bind, assert failure with expected StatusCode.
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

// ===========================================================================
// SELECT * from algorithm TVF
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, SelectStarResolvesAllColumns) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::FLOAT64);
}

TEST_F(BinderAlgorithmTvfTest, SelectStarWithThreeColumns) {
    auto bound = bind_ok("SELECT * FROM community_detection('follows')");
    ASSERT_EQ(bound.output_columns.size(), 3);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "community_id");
    EXPECT_EQ(bound.output_columns[2].column_name, "modularity");
    EXPECT_EQ(bound.output_columns[2].nullable, true);
}

// ===========================================================================
// Named column references
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, NamedColumnReference) {
    auto bound = bind_ok("SELECT node_id, rank FROM pagerank('knows')");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
}

TEST_F(BinderAlgorithmTvfTest, UnknownColumnFails) {
    bind_error("SELECT nonexistent FROM pagerank('knows')", StatusCode::NOT_FOUND);
}

// ===========================================================================
// Qualified column references with alias
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, QualifiedColumnWithAlias) {
    auto bound = bind_ok("SELECT pr.node_id, pr.rank FROM pagerank('knows') AS pr");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[0].table_name, "pr");
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
    EXPECT_EQ(bound.output_columns[1].table_name, "pr");
}

TEST_F(BinderAlgorithmTvfTest, QualifiedStarWithAlias) {
    auto bound = bind_ok("SELECT pr.* FROM pagerank('knows') AS pr");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
}

// ===========================================================================
// WHERE clause referencing algorithm columns
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, WhereClauseResolvesAlgorithmColumns) {
    auto bound = bind_ok("SELECT node_id FROM pagerank('knows') WHERE rank > 0.5");
    ASSERT_EQ(bound.output_columns.size(), 1);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
}

// ===========================================================================
// ORDER BY clause referencing algorithm columns
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, OrderByResolvesAlgorithmColumns) {
    auto bound = bind_ok("SELECT node_id, rank FROM pagerank('knows') ORDER BY rank DESC");
    ASSERT_EQ(bound.output_columns.size(), 2);
}

// ===========================================================================
// Named parameters
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, NamedParametersParsedCorrectly) {
    auto bound = bind_ok("SELECT * FROM pagerank('knows', damping := 0.85, iterations := 20)");
    ASSERT_EQ(bound.output_columns.size(), 2);
    EXPECT_EQ(bound.output_columns[0].column_name, "node_id");
    EXPECT_EQ(bound.output_columns[1].column_name, "rank");
}

// ===========================================================================
// Without registry — graceful degradation
// ===========================================================================

TEST_F(BinderAlgorithmTvfTest, WithoutRegistrySelectStarReturnsNoColumns) {
    // Create a binder without the algorithm registry.
    Binder no_reg_binder(catalog);
    auto stmt = parse("SELECT * FROM pagerank('knows')");
    ASSERT_NE(stmt, nullptr);
    auto result = no_reg_binder.bind(*stmt);
    // Should succeed but with 0 output columns (old behavior).
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output_columns.size(), 0);
}

} // namespace sixseven
