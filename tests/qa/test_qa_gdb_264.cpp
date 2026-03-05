/// @file test_qa_gdb_264.cpp
/// QA adversarial tests for GDB-264: Binder — Build enriched scope for TRAVERSE
/// source.
///
/// Tests the binder's ability to resolve columns, meta-columns, edge properties,
/// and error paths when TRAVERSE appears as a FROM source in SELECT queries.

#include "sixseven/catalog/catalog.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/planner/type_resolver.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sixseven {

// =============================================================================
// Test fixture — catalog with tables and edge types for TRAVERSE binding
// =============================================================================

class QA_GDB264 : public ::testing::Test {
protected:
    Catalog catalog;
    std::unique_ptr<Binder> binder;

    void SetUp() override {
        // Table: users(id INT32 PK, name STRING, email STRING, age INT32, active BOOL)
        {
            TableSchema s;
            s.name = "users";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "name", TypeId::STRING, true, ""},
                {2, "email", TypeId::STRING, true, ""},
                {3, "age", TypeId::INT32, true, ""},
                {4, "active", TypeId::BOOL, false, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Table: posts(id INT32 PK, title STRING, body STRING)
        {
            TableSchema s;
            s.name = "posts";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "title", TypeId::STRING, true, ""},
                {2, "body", TypeId::STRING, true, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Table: orders(id INT32 PK, amount FLOAT64)
        {
            TableSchema s;
            s.name = "orders";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "amount", TypeId::FLOAT64, true, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Table: items(uid INT64 PK, label STRING)
        // Different PK type to test __node/__source type derivation.
        {
            TableSchema s;
            s.name = "items";
            s.columns = {
                {0, "uid", TypeId::INT64, false, ""},
                {1, "label", TypeId::STRING, true, ""},
            };
            s.pk_columns = "uid";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Edge: follows (users → users) with weight property
        {
            EdgeTypeDef e;
            e.name = "follows";
            e.source_table_id = 1; // users
            e.target_table_id = 1; // users (self-referential)
            e.properties = "weight:FLOAT64";
            auto r = catalog.create_edge_type(std::move(e));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Edge: authored (users → posts) — heterogeneous, no properties
        {
            EdgeTypeDef e;
            e.name = "authored";
            e.source_table_id = 1; // users
            e.target_table_id = 2; // posts
            auto r = catalog.create_edge_type(std::move(e));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Edge: contains (users → items) — heterogeneous, multiple properties
        {
            EdgeTypeDef e;
            e.name = "contains";
            e.source_table_id = 1; // users
            e.target_table_id = 4; // items
            e.properties = "quantity:INT32,note:STRING";
            auto r = catalog.create_edge_type(std::move(e));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        // Edge: links_to (items → items) — self-referential with INT64 PK
        {
            EdgeTypeDef e;
            e.name = "links_to";
            e.source_table_id = 4; // items
            e.target_table_id = 4; // items
            auto r = catalog.create_edge_type(std::move(e));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }

        binder = std::make_unique<Binder>(catalog);
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
            ADD_FAILURE() << "Bind failed for: " << sql << "\n  Error: " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void bind_error(const std::string& sql, StatusCode expected) {
        auto stmt = parse(sql);
        if (!stmt) {
            ADD_FAILURE() << "Parse failed unexpectedly for: " << sql;
            return;
        }
        auto result = binder->bind(*stmt);
        EXPECT_FALSE(result.has_value()) << "Expected bind error but succeeded for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << status_code_name(expected) << " but got "
                << status_code_name(result.error().code) << ": " << result.error().message
                << "\n  SQL: " << sql;
        }
    }
};

// =============================================================================
// AC1: SELECT column resolves from the target table
// =============================================================================

TEST_F(QA_GDB264, AC1_ResolvesSingleTargetColumn) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

TEST_F(QA_GDB264, AC1_ResolvesMultipleTargetColumns) {
    auto bound =
        bind_ok("SELECT id, name, email, age FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 4u);
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].column_name, "name");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::STRING);
    EXPECT_EQ(bound.output_columns[2].column_name, "email");
    EXPECT_EQ(bound.output_columns[2].type_id, TypeId::STRING);
    EXPECT_EQ(bound.output_columns[3].column_name, "age");
    EXPECT_EQ(bound.output_columns[3].type_id, TypeId::INT32);
}

TEST_F(QA_GDB264, AC1_ResolvesTargetColumnCaseInsensitive) {
    auto bound = bind_ok("SELECT NAME FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

// =============================================================================
// AC2: Meta-columns resolve correctly
// =============================================================================

TEST_F(QA_GDB264, AC2_DepthIsINT64) {
    auto bound = bind_ok("SELECT __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(QA_GDB264, AC2_NodeMatchesPKTypeINT32) {
    // users has INT32 PK → __node should be INT32.
    auto bound = bind_ok("SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__node");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(QA_GDB264, AC2_NodeMatchesPKTypeINT64) {
    // items has INT64 PK → __node should be INT64.
    auto bound = bind_ok("SELECT __node FROM TRAVERSE links_to FROM items(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__node");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(QA_GDB264, AC2_SourceIsNullable) {
    auto bound = bind_ok("SELECT __source FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__source");
    EXPECT_TRUE(bound.output_columns[0].nullable);
}

TEST_F(QA_GDB264, AC2_SourceMatchesPKType) {
    auto bound = bind_ok("SELECT __source FROM TRAVERSE links_to FROM items(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(QA_GDB264, AC2_AllThreeMetaColumnsInOneQuery) {
    auto bound = bind_ok(
        "SELECT __node, __depth, __source FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 3u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__node");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(bound.output_columns[2].column_name, "__source");
    EXPECT_EQ(bound.output_columns[2].type_id, TypeId::INT32);
    EXPECT_TRUE(bound.output_columns[2].nullable);
}

TEST_F(QA_GDB264, AC2_DepthIsNotNullable) {
    auto bound = bind_ok("SELECT __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_FALSE(bound.output_columns[0].nullable);
}

TEST_F(QA_GDB264, AC2_NodeIsNotNullable) {
    auto bound = bind_ok("SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_FALSE(bound.output_columns[0].nullable);
}

// =============================================================================
// AC3: Nonexistent column produces binding error
// =============================================================================

TEST_F(QA_GDB264, AC3_NonexistentColumnError) {
    bind_error("SELECT nonexistent FROM TRAVERSE follows FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, AC3_NonexistentColumnWithUnderscorePrefix) {
    // __bogus is not a real meta-column.
    bind_error("SELECT __bogus FROM TRAVERSE follows FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, AC3_SourceColumnsNotAccessibleInHeterogeneousTraversal) {
    // authored: users→posts. OUT from users exposes posts columns.
    // "email" is a users column — should NOT be found.
    bind_error("SELECT email FROM TRAVERSE authored FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, AC3_SourceColumnsNotAccessibleWithName) {
    // "name" is a users column, not a posts column.
    bind_error("SELECT name FROM TRAVERSE authored FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

// =============================================================================
// AC4: Heterogeneous edges — OUT exposes target columns, not source
// =============================================================================

TEST_F(QA_GDB264, AC4_OutExposesPostColumnsNotUserColumns) {
    auto bound = bind_ok("SELECT title, body FROM TRAVERSE authored FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    EXPECT_EQ(bound.output_columns[0].column_name, "title");
    EXPECT_EQ(bound.output_columns[1].column_name, "body");
}

TEST_F(QA_GDB264, AC4_InExposesSourceColumns) {
    // authored: users→posts. IN from posts goes to users (source).
    auto bound = bind_ok("SELECT name, email FROM TRAVERSE authored FROM posts(1) DIRECTION IN");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
    EXPECT_EQ(bound.output_columns[1].column_name, "email");
}

TEST_F(QA_GDB264, AC4_InDoesNotExposeTargetColumns) {
    // IN from posts → users. "title" is a posts column, should NOT be found.
    bind_error("SELECT title FROM TRAVERSE authored FROM posts(1) DIRECTION IN",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, AC4_BothFromSourceGoesToTarget) {
    // GDB-267 intentionally restricted DIRECTION BOTH to homogeneous edges.
    // Heterogeneous edges (like "authored": users→posts) reject BOTH.
    bind_error("SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB264, AC4_BothFromTargetGoesToSource) {
    // GDB-267 intentionally restricted DIRECTION BOTH to homogeneous edges.
    bind_error("SELECT name FROM TRAVERSE authored FROM posts(1) DIRECTION BOTH",
               StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB264, AC4_HeterogeneousEdgeStarExpansionOnlyTarget) {
    // authored: users→posts, no properties. OUT from users → posts columns + meta.
    // posts: id, title, body (3 cols) + __node, __depth, __source (3 meta) = 6 total
    auto bound = bind_ok("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION OUT");
    EXPECT_EQ(bound.output_columns.size(), 6u);
}

TEST_F(QA_GDB264, AC4_HeterogeneousEdgeINStarExpansion) {
    // authored: users→posts. IN from posts → users columns + meta.
    // users: id, name, email, age, active (5 cols) + __node, __depth, __source (3 meta) = 8
    auto bound = bind_ok("SELECT * FROM TRAVERSE authored FROM posts(1) DIRECTION IN");
    EXPECT_EQ(bound.output_columns.size(), 8u);
}

// =============================================================================
// AC5: Error when FROM table is not an endpoint of the edge type
// =============================================================================

TEST_F(QA_GDB264, AC5_OrdersNotEndpointOfAuthored) {
    bind_error("SELECT id FROM TRAVERSE authored FROM orders(1) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB264, AC5_PostsNotEndpointOfFollows) {
    // follows: users→users. Posts is not an endpoint.
    bind_error("SELECT id FROM TRAVERSE follows FROM posts(1) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB264, AC5_OrdersNotEndpointOfFollows) {
    bind_error("SELECT id FROM TRAVERSE follows FROM orders(1) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB264, AC5_ItemsNotEndpointOfAuthored) {
    bind_error("SELECT id FROM TRAVERSE authored FROM items(1) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

// =============================================================================
// AC6: Edge property columns accessible via qualified name
// =============================================================================

TEST_F(QA_GDB264, AC6_QualifiedEdgePropertyResolves) {
    auto bound = bind_ok("SELECT follows.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "weight");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

TEST_F(QA_GDB264, AC6_EdgePropertyIsNullable) {
    auto bound = bind_ok("SELECT follows.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_TRUE(bound.output_columns[0].nullable);
}

TEST_F(QA_GDB264, AC6_MultipleEdgeProperties) {
    // contains has two properties: quantity:INT32, note:STRING
    auto bound = bind_ok("SELECT contains.quantity, contains.note FROM TRAVERSE contains "
                         "FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    EXPECT_EQ(bound.output_columns[0].column_name, "quantity");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].column_name, "note");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::STRING);
}

TEST_F(QA_GDB264, AC6_NonexistentEdgePropertyError) {
    // "follows" edge only has "weight" property.
    bind_error("SELECT follows.nonexistent FROM TRAVERSE follows FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, AC6_QualifiedAccessOnEdgeWithNoProperties) {
    // authored has no properties. Qualified access should fail.
    bind_error("SELECT authored.weight FROM TRAVERSE authored FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, AC6_UnqualifiedEdgePropertyResolves) {
    // "weight" is only on the follows edge, not on users table.
    // Unqualified should resolve because there's only one match.
    auto bound = bind_ok("SELECT weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "weight");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

// =============================================================================
// AC7: Internal from_key and where_expr are bound correctly
// =============================================================================

TEST_F(QA_GDB264, AC7_FromKeyLiteralBinds) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

TEST_F(QA_GDB264, AC7_FromKeyExpressionBinds) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1 + 2) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
}

TEST_F(QA_GDB264, AC7_WhereExprWithMetaColumnBinds) {
    auto bound =
        bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE __depth < 3");
    ASSERT_EQ(bound.output_columns.size(), 1u);
}

TEST_F(QA_GDB264, AC7_WhereExprWithNodeBinds) {
    auto bound =
        bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE __node > 0");
    ASSERT_EQ(bound.output_columns.size(), 1u);
}

TEST_F(QA_GDB264, AC7_WhereExprWithSourceBinds) {
    auto bound = bind_ok(
        "SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE __source IS NULL");
    ASSERT_EQ(bound.output_columns.size(), 1u);
}

// =============================================================================
// Star expansion correctness
// =============================================================================

TEST_F(QA_GDB264, StarExpansion_SelfReferentialWithProperties) {
    // follows: users→users, 1 property (weight).
    // users: 5 cols + 3 meta + 1 prop = 9
    auto bound = bind_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    EXPECT_EQ(bound.output_columns.size(), 9u);

    // Verify ordering: target table cols first, then meta, then edge props.
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
    EXPECT_EQ(bound.output_columns[1].column_name, "name");
    EXPECT_EQ(bound.output_columns[2].column_name, "email");
    EXPECT_EQ(bound.output_columns[3].column_name, "age");
    EXPECT_EQ(bound.output_columns[4].column_name, "active");
    EXPECT_EQ(bound.output_columns[5].column_name, "__node");
    EXPECT_EQ(bound.output_columns[6].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[7].column_name, "__source");
    EXPECT_EQ(bound.output_columns[8].column_name, "weight");
}

TEST_F(QA_GDB264, StarExpansion_HeterogeneousNoProperties) {
    // authored: users→posts, no properties.
    // posts: id, title, body (3) + 3 meta = 6
    auto bound = bind_ok("SELECT * FROM TRAVERSE authored FROM users(1) DIRECTION OUT");
    EXPECT_EQ(bound.output_columns.size(), 6u);
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
    EXPECT_EQ(bound.output_columns[1].column_name, "title");
    EXPECT_EQ(bound.output_columns[2].column_name, "body");
    EXPECT_EQ(bound.output_columns[3].column_name, "__node");
    EXPECT_EQ(bound.output_columns[4].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[5].column_name, "__source");
}

TEST_F(QA_GDB264, StarExpansion_EdgeWithMultipleProperties) {
    // contains: users→items, 2 properties (quantity, note).
    // items: uid, label (2) + 3 meta + 2 props = 7
    auto bound = bind_ok("SELECT * FROM TRAVERSE contains FROM users(1) DIRECTION OUT");
    EXPECT_EQ(bound.output_columns.size(), 7u);
    EXPECT_EQ(bound.output_columns[0].column_name, "uid");
    EXPECT_EQ(bound.output_columns[1].column_name, "label");
    EXPECT_EQ(bound.output_columns[2].column_name, "__node");
    EXPECT_EQ(bound.output_columns[3].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[4].column_name, "__source");
    EXPECT_EQ(bound.output_columns[5].column_name, "quantity");
    EXPECT_EQ(bound.output_columns[6].column_name, "note");
}

// =============================================================================
// Error paths — edge type and table resolution
// =============================================================================

TEST_F(QA_GDB264, EdgeTypeNotFound) {
    bind_error("SELECT id FROM TRAVERSE nonexistent FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB264, FromTableNotFound) {
    bind_error("SELECT id FROM TRAVERSE follows FROM nonexistent(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

// =============================================================================
// Referenced tables and edge types tracking
// =============================================================================

TEST_F(QA_GDB264, ReferencedEdgeTypesTracked) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    EXPECT_FALSE(bound.referenced_edge_types.empty());
}

TEST_F(QA_GDB264, ReferencedTablesTracked) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    // Should reference the FROM table (users).
    EXPECT_FALSE(bound.referenced_tables.empty());
}

// =============================================================================
// Direction logic: self-referential edges
// =============================================================================

TEST_F(QA_GDB264, SelfReferentialOutResolvesUsers) {
    // follows: users→users. OUT target = users.
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

TEST_F(QA_GDB264, SelfReferentialInResolvesUsers) {
    // follows: users→users. IN target = users (source).
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION IN");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

TEST_F(QA_GDB264, SelfReferentialBothResolvesUsers) {
    // follows: users→users. BOTH target = users (same either way).
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION BOTH");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

// =============================================================================
// Direction logic: heterogeneous edges
// =============================================================================

TEST_F(QA_GDB264, HeterogeneousOutFromSource) {
    // authored: users→posts. OUT from users → posts columns.
    auto bound = bind_ok("SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "title");
}

TEST_F(QA_GDB264, HeterogeneousInFromTarget) {
    // authored: users→posts. IN from posts → users columns.
    auto bound = bind_ok("SELECT name FROM TRAVERSE authored FROM posts(1) DIRECTION IN");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

TEST_F(QA_GDB264, HeterogeneousOutFromTarget) {
    // authored: users→posts. OUT from posts → posts columns (target_table_id).
    // The FROM table (posts) is the target. OUT always uses edge->target_table_id.
    // So target = posts → resolves posts columns. This is semantically questionable
    // but follows the implementation logic.
    auto bound = bind_ok("SELECT title FROM TRAVERSE authored FROM posts(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "title");
}

TEST_F(QA_GDB264, HeterogeneousInFromSource) {
    // authored: users→posts. IN from users → users columns (source_table_id).
    // The FROM table (users) is the source. IN always uses edge->source_table_id.
    // So target = users → resolves users columns. Again, semantically questionable.
    auto bound = bind_ok("SELECT name FROM TRAVERSE authored FROM users(1) DIRECTION IN");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

// =============================================================================
// Meta-column PK type derivation across different tables
// =============================================================================

TEST_F(QA_GDB264, MetaNodePKTypeFromItemsTable) {
    // items PK is INT64 → __node should be INT64.
    auto bound = bind_ok("SELECT __node FROM TRAVERSE contains FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(QA_GDB264, MetaSourcePKTypeFromItemsTable) {
    auto bound = bind_ok("SELECT __source FROM TRAVERSE contains FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(QA_GDB264, MetaNodePKTypeFromUsersTable) {
    auto bound = bind_ok("SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

// =============================================================================
// Edge property type parsing
// =============================================================================

TEST_F(QA_GDB264, PropertyTypeINT32) {
    auto bound = bind_ok("SELECT contains.quantity FROM TRAVERSE contains "
                         "FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(QA_GDB264, PropertyTypeSTRING) {
    auto bound = bind_ok("SELECT contains.note FROM TRAVERSE contains FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

TEST_F(QA_GDB264, PropertyTypeFLOAT64) {
    auto bound = bind_ok("SELECT follows.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

// =============================================================================
// Combining target columns, meta-columns, and edge properties in one query
// =============================================================================

TEST_F(QA_GDB264, CombineTargetMetaAndEdgeProperty) {
    auto bound = bind_ok("SELECT name, __depth, follows.weight FROM TRAVERSE follows "
                         "FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 3u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
    EXPECT_EQ(bound.output_columns[1].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(bound.output_columns[2].column_name, "weight");
    EXPECT_EQ(bound.output_columns[2].type_id, TypeId::FLOAT64);
}

// =============================================================================
// WHERE clause: non-meta column should fail in TRAVERSE WHERE
// =============================================================================

TEST_F(QA_GDB264, WhereExprWithNonMetaColumnFails) {
    // GDB-265 changed the WHERE scope to include enriched target table columns
    // (not just meta-columns).  "name" is a users column, so it now binds
    // successfully in the enriched scope.
    auto bound =
        bind_ok("SELECT id FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE name = 'alice'");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
}

TEST_F(QA_GDB264, WhereExprDepthComparisonBinds) {
    bind_ok("SELECT id FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE __depth <= 5");
}

TEST_F(QA_GDB264, WhereExprComplexMetaExpression) {
    bind_ok("SELECT id FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
            "WHERE __depth > 0 AND __source IS NOT NULL");
}

// =============================================================================
// Default direction (omitting DIRECTION clause)
// =============================================================================

TEST_F(QA_GDB264, DefaultDirectionIsOut) {
    // When DIRECTION is omitted, default is OUT.
    auto bound = bind_ok("SELECT title FROM TRAVERSE authored FROM users(1)");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    // OUT → target = posts → title exists.
    EXPECT_EQ(bound.output_columns[0].column_name, "title");
}

// =============================================================================
// MAX_DEPTH has no effect on binding (but parses correctly)
// =============================================================================

TEST_F(QA_GDB264, MaxDepthDoesNotAffectBinding) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) MAX_DEPTH 3");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

// =============================================================================
// FETCH flag has no effect on binding
// =============================================================================

TEST_F(QA_GDB264, FetchDoesNotAffectBinding) {
    auto bound =
        bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

// =============================================================================
// Alias behavior
// =============================================================================

TEST_F(QA_GDB264, QualifiedColumnWithAlias) {
    auto bound = bind_ok("SELECT t.name FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

TEST_F(QA_GDB264, StarWithAlias) {
    auto bound = bind_ok("SELECT t.* FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");
    // Same columns as full star: 5 user + 3 meta + 1 prop = 9
    EXPECT_EQ(bound.output_columns.size(), 9u);
}

} // namespace sixseven
