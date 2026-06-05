#include "sixseven/catalog/catalog.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/planner/type_resolver.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {

// ===========================================================================
// Test fixture — shared catalog with standard test tables
// ===========================================================================

class BinderTest : public ::testing::Test {
protected:
    Catalog catalog;
    std::unique_ptr<Binder> binder;

    void SetUp() override {
        init_test_catalog(catalog);
        // Table: users(id INT32, name STRING, email STRING, age INT32, active BOOL)
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
            ASSERT_TRUE(r.has_value());
        }

        // Table: orders(id INT32, user_id INT32, product_id INT32,
        //               amount FLOAT64, created_at TIMESTAMP)
        {
            TableSchema s;
            s.name = "orders";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "user_id", TypeId::INT32, false, ""},
                {2, "product_id", TypeId::INT32, false, ""},
                {3, "amount", TypeId::FLOAT64, true, ""},
                {4, "created_at", TypeId::TIMESTAMP, true, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value());
        }

        // Table: products(id INT32, name STRING, price DECIMAL,
        //                  description STRING, desc_vector EMBEDDING)
        {
            TableSchema s;
            s.name = "products";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "name", TypeId::STRING, true, ""},
                {2, "price", TypeId::DECIMAL, true, ""},
                {3, "description", TypeId::STRING, true, ""},
                {4, "desc_vector", TypeId::EMBEDDING, true, ""},
            };
            s.pk_columns = "id";
            auto r = catalog.create_table(default_database_id, std::move(s));
            ASSERT_TRUE(r.has_value());
        }

        // Table: posts(id INT32, title STRING, body STRING)
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
            ASSERT_TRUE(r.has_value());
        }

        // Edge type: purchased (FROM users TO products)
        {
            EdgeTypeDef e;
            e.name = "purchased";
            e.source_table_id = 1; // users
            e.target_table_id = 3; // products
            auto r = catalog.create_edge_type(default_database_id, std::move(e));
            ASSERT_TRUE(r.has_value());
        }

        // Edge type: follows (users -> users) with weight property
        {
            EdgeTypeDef e;
            e.name = "follows";
            e.source_table_id = 1; // users
            e.target_table_id = 1; // users (self-referential)
            e.properties = "weight:FLOAT64";
            auto r = catalog.create_edge_type(default_database_id, std::move(e));
            ASSERT_TRUE(r.has_value());
        }

        // Edge type: authored (users -> posts) — heterogeneous edge
        {
            EdgeTypeDef e;
            e.name = "authored";
            e.source_table_id = 1; // users
            e.target_table_id = 4; // posts
            auto r = catalog.create_edge_type(default_database_id, std::move(e));
            ASSERT_TRUE(r.has_value());
        }

        binder = std::make_unique<Binder>(catalog);
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

    /// Parse and bind, assert success. Returns default-constructed BoundStatement on failure.
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
// Type Resolver tests
// ===========================================================================

TEST(TypeResolver, ResolvesIntegerTypes) {
    TypeSpec spec;
    spec.name = "INT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INT32);

    spec.name = "INTEGER";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INT32);

    spec.name = "BIGINT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INT64);

    spec.name = "SMALLINT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INT16);

    spec.name = "TINYINT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INT8);
}

TEST(TypeResolver, ResolvesFloatTypes) {
    TypeSpec spec;
    spec.name = "FLOAT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::FLOAT32);

    spec.name = "DOUBLE";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::FLOAT64);

    spec.name = "REAL";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::FLOAT32);
}

TEST(TypeResolver, ResolvesStringTypes) {
    TypeSpec spec;
    spec.name = "TEXT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::STRING);

    spec.name = "VARCHAR";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::STRING);

    spec.name = "CHAR";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::STRING);
}

TEST(TypeResolver, ResolvesBoolDecimalBlob) {
    TypeSpec spec;
    spec.name = "BOOL";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::BOOL);

    spec.name = "BOOLEAN";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::BOOL);

    spec.name = "DECIMAL";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::DECIMAL);

    spec.name = "NUMERIC";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::DECIMAL);

    spec.name = "BLOB";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::BLOB);

    spec.name = "BYTEA";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::BLOB);
}

TEST(TypeResolver, ResolvesTemporalAndOtherTypes) {
    TypeSpec spec;
    spec.name = "DATE";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::DATE);

    spec.name = "TIME";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::TIME);

    spec.name = "TIMESTAMP";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::TIMESTAMP);

    spec.name = "INTERVAL";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INTERVAL);

    spec.name = "POINT";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::POINT);

    spec.name = "JSON";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::JSON);

    spec.name = "UUID";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::UUID);

    spec.name = "EMBEDDING";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::EMBEDDING);
}

TEST(TypeResolver, CaseInsensitive) {
    TypeSpec spec;
    spec.name = "int";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::INT32);

    spec.name = "VarChar";
    EXPECT_EQ(*resolve_type_spec(spec), TypeId::STRING);
}

TEST(TypeResolver, UnknownTypeError) {
    TypeSpec spec;
    spec.name = "FOOBAR";
    auto result = resolve_type_spec(spec);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, CommonTypeNumericWidening) {
    EXPECT_EQ(*common_type(TypeId::INT8, TypeId::INT32), TypeId::INT32);
    EXPECT_EQ(*common_type(TypeId::INT32, TypeId::INT64), TypeId::INT64);
    EXPECT_EQ(*common_type(TypeId::INT32, TypeId::FLOAT64), TypeId::FLOAT64);
    EXPECT_EQ(*common_type(TypeId::FLOAT32, TypeId::FLOAT64), TypeId::FLOAT64);
}

TEST(TypeResolver, CommonTypeSame) {
    EXPECT_EQ(*common_type(TypeId::STRING, TypeId::STRING), TypeId::STRING);
    EXPECT_EQ(*common_type(TypeId::BOOL, TypeId::BOOL), TypeId::BOOL);
}

TEST(TypeResolver, CommonTypeError) {
    auto result = common_type(TypeId::STRING, TypeId::INT32);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(TypeResolver, IsAggregateFunction) {
    EXPECT_TRUE(is_aggregate_function("COUNT"));
    EXPECT_TRUE(is_aggregate_function("count"));
    EXPECT_TRUE(is_aggregate_function("SUM"));
    EXPECT_TRUE(is_aggregate_function("AVG"));
    EXPECT_TRUE(is_aggregate_function("MIN"));
    EXPECT_TRUE(is_aggregate_function("MAX"));
    EXPECT_FALSE(is_aggregate_function("LOWER"));
    EXPECT_FALSE(is_aggregate_function("ABS"));
}

TEST(TypeResolver, AggregateReturnTypes) {
    EXPECT_EQ(*aggregate_return_type("COUNT", TypeId::INT32), TypeId::INT64);
    EXPECT_EQ(*aggregate_return_type("SUM", TypeId::INT32), TypeId::INT64);
    EXPECT_EQ(*aggregate_return_type("SUM", TypeId::FLOAT64), TypeId::FLOAT64);
    EXPECT_EQ(*aggregate_return_type("AVG", TypeId::INT32), TypeId::FLOAT64);
    EXPECT_EQ(*aggregate_return_type("MIN", TypeId::STRING), TypeId::STRING);
    EXPECT_EQ(*aggregate_return_type("MAX", TypeId::INT64), TypeId::INT64);
}

TEST(TypeResolver, AggregateTypeErrors) {
    auto r1 = aggregate_return_type("SUM", TypeId::STRING);
    EXPECT_FALSE(r1.has_value());

    auto r2 = aggregate_return_type("AVG", TypeId::BOOL);
    EXPECT_FALSE(r2.has_value());
}

TEST(TypeResolver, ScalarFunctionReturnTypes) {
    EXPECT_EQ(*function_return_type("LOWER", {}), TypeId::STRING);
    EXPECT_EQ(*function_return_type("LENGTH", {}), TypeId::INT64);
    EXPECT_EQ(*function_return_type("ABS", {TypeId::INT32}), TypeId::INT32);
    EXPECT_EQ(*function_return_type("SQRT", {}), TypeId::FLOAT64);
    EXPECT_EQ(*function_return_type("NOW", {}), TypeId::TIMESTAMP);
    EXPECT_EQ(*function_return_type("COALESCE", {TypeId::STRING}), TypeId::STRING);
}

TEST(TypeResolver, UnknownFunctionError) {
    auto result = function_return_type("NONEXISTENT_FUNC", {});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// Scope tests
// ===========================================================================

TEST_F(BinderTest, ScopeSingleTableResolve) {
    auto schema = catalog.get_table(default_database_id, "users");
    ASSERT_TRUE(schema.has_value());

    Scope scope;
    ScopeTable st;
    st.table_id = schema->table_id;
    st.alias = "users";
    for (auto& col : schema->columns) {
        ResolvedColumn rc;
        rc.table_id = schema->table_id;
        rc.ordinal = col.ordinal;
        rc.table_name = "users";
        rc.column_name = col.name;
        rc.type_id = col.type_id;
        rc.nullable = col.nullable;
        st.columns.push_back(rc);
    }
    scope.add_table(std::move(st));

    auto id_col = scope.resolve_column("id");
    ASSERT_TRUE(id_col.has_value());
    EXPECT_EQ(id_col->type_id, TypeId::INT32);
    EXPECT_EQ(id_col->column_name, "id");

    auto name_col = scope.resolve_column("name");
    ASSERT_TRUE(name_col.has_value());
    EXPECT_EQ(name_col->type_id, TypeId::STRING);
}

TEST_F(BinderTest, ScopeQualifiedResolve) {
    auto schema = catalog.get_table(default_database_id, "users");
    ASSERT_TRUE(schema.has_value());

    Scope scope;
    ScopeTable st;
    st.table_id = schema->table_id;
    st.alias = "users";
    for (auto& col : schema->columns) {
        ResolvedColumn rc;
        rc.table_id = schema->table_id;
        rc.ordinal = col.ordinal;
        rc.table_name = "users";
        rc.column_name = col.name;
        rc.type_id = col.type_id;
        rc.nullable = col.nullable;
        st.columns.push_back(rc);
    }
    scope.add_table(std::move(st));

    auto result = scope.resolve_column("users", "id");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id, TypeId::INT32);
}

TEST_F(BinderTest, ScopeAmbiguousColumn) {
    // Two tables with same column name "id".
    Scope scope;
    ScopeTable st1;
    st1.alias = "users";
    st1.columns = {{1, 0, "users", "id", TypeId::INT32, false}};
    scope.add_table(std::move(st1));

    ScopeTable st2;
    st2.alias = "orders";
    st2.columns = {{2, 0, "orders", "id", TypeId::INT32, false}};
    scope.add_table(std::move(st2));

    auto result = scope.resolve_column("id");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(BinderTest, ScopeColumnNotFound) {
    Scope scope;
    auto result = scope.resolve_column("nonexistent");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, ScopeStarExpansion) {
    Scope scope;
    ScopeTable st;
    st.alias = "users";
    st.columns = {
        {1, 0, "users", "id", TypeId::INT32, false},
        {1, 1, "users", "name", TypeId::STRING, true},
    };
    scope.add_table(std::move(st));

    auto all = scope.all_columns();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].column_name, "id");
    EXPECT_EQ(all[1].column_name, "name");
}

TEST_F(BinderTest, ScopeColumnsFor) {
    Scope scope;
    ScopeTable st;
    st.alias = "users";
    st.columns = {
        {1, 0, "users", "id", TypeId::INT32, false},
        {1, 1, "users", "name", TypeId::STRING, true},
    };
    scope.add_table(std::move(st));

    auto cols = scope.columns_for("users");
    ASSERT_TRUE(cols.has_value());
    EXPECT_EQ(cols->size(), 2u);

    auto missing = scope.columns_for("nonexistent");
    EXPECT_FALSE(missing.has_value());
}

TEST_F(BinderTest, ScopeParentScope) {
    Scope parent;
    ScopeTable st;
    st.alias = "users";
    st.columns = {{1, 0, "users", "id", TypeId::INT32, false}};
    parent.add_table(std::move(st));

    Scope child(&parent);
    auto result = child.resolve_column("id");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id, TypeId::INT32);
}

// ===========================================================================
// Expression binding tests
// ===========================================================================

TEST_F(BinderTest, LiteralIntegerType) {
    auto bound = bind_ok("SELECT 42 FROM users");
    EXPECT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(BinderTest, LiteralFloatType) {
    auto bound = bind_ok("SELECT 3.14 FROM users");
    EXPECT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

TEST_F(BinderTest, LiteralStringType) {
    auto bound = bind_ok("SELECT 'hello' FROM users");
    EXPECT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

TEST_F(BinderTest, LiteralBoolType) {
    auto bound = bind_ok("SELECT TRUE FROM users");
    EXPECT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(BinderTest, ColumnRefResolves) {
    auto bound = bind_ok("SELECT id, name FROM users");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].column_name, "name");
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::STRING);
}

TEST_F(BinderTest, QualifiedColumnRef) {
    auto bound = bind_ok("SELECT users.id FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(BinderTest, ArithmeticPromotion) {
    auto bound = bind_ok("SELECT id + 1 FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    // INT32 + INT64 → INT64
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(BinderTest, ComparisonReturnsBool) {
    auto bound = bind_ok("SELECT id > 5 FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(BinderTest, TypeMismatchError) {
    // STRING + INT → error (no common type)
    bind_error("SELECT name + 1 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, CastExpression) {
    auto bound = bind_ok("SELECT CAST(id AS DOUBLE) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

TEST_F(BinderTest, CaseExpressionCommonType) {
    auto bound = bind_ok("SELECT CASE WHEN active THEN 1 ELSE 2 END FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(BinderTest, InExpressionReturnsBool) {
    auto bound = bind_ok("SELECT id IN (1, 2, 3) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(BinderTest, BetweenExpressionReturnsBool) {
    auto bound = bind_ok("SELECT age BETWEEN 18 AND 65 FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(BinderTest, IsNullReturnsBool) {
    auto bound = bind_ok("SELECT name IS NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(BinderTest, LikeReturnsBool) {
    auto bound = bind_ok("SELECT name LIKE '%john%' FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(BinderTest, AggregateCountType) {
    auto bound = bind_ok("SELECT COUNT(*) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(BinderTest, AggregateSumType) {
    auto bound = bind_ok("SELECT SUM(age) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    // SUM(INT32) → INT64
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

TEST_F(BinderTest, AggregateAvgType) {
    auto bound = bind_ok("SELECT AVG(age) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

// ===========================================================================
// SELECT binding tests
// ===========================================================================

TEST_F(BinderTest, SimpleSelect) {
    auto bound = bind_ok("SELECT id, name FROM users");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, SelectStar) {
    auto bound = bind_ok("SELECT * FROM users");
    EXPECT_EQ(bound.output_columns.size(), 5u); // id, name, email, age, active
}

TEST_F(BinderTest, SelectTableStar) {
    auto bound = bind_ok("SELECT users.* FROM users");
    EXPECT_EQ(bound.output_columns.size(), 5u);
}

TEST_F(BinderTest, SelectWithAlias) {
    auto bound = bind_ok("SELECT id AS user_id FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "user_id");
}

TEST_F(BinderTest, SelectWithJoin) {
    auto bound = bind_ok("SELECT users.id, orders.amount "
                         "FROM users JOIN orders ON users.id = orders.user_id");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::FLOAT64);
}

TEST_F(BinderTest, SelectWithWhereTypeCheck) {
    // WHERE must be boolean.
    bind_ok("SELECT id FROM users WHERE active");
    bind_ok("SELECT id FROM users WHERE id > 5");
}

TEST_F(BinderTest, SelectWhereNonBoolError) {
    bind_error("SELECT id FROM users WHERE name", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, SelectGroupByValid) {
    auto bound = bind_ok("SELECT age, COUNT(*) FROM users GROUP BY age");
    ASSERT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(BinderTest, SelectGroupByMissingColumn) {
    bind_error("SELECT name, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, SelectHaving) {
    auto bound = bind_ok("SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 5");
    EXPECT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(BinderTest, SelectOrderBy) {
    auto bound = bind_ok("SELECT id, name FROM users ORDER BY id ASC");
    EXPECT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(BinderTest, SelectOrderByAlias) {
    auto bound = bind_ok("SELECT name AS n FROM users ORDER BY n ASC");
    EXPECT_EQ(bound.output_columns.size(), 1u);
}

TEST_F(BinderTest, SelectOrderByAggregateAlias) {
    auto bound = bind_ok("SELECT age, COUNT(*) AS cnt FROM users GROUP BY age ORDER BY cnt DESC");
    EXPECT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(BinderTest, SelectOrderByAliasWithHaving) {
    auto bound = bind_ok("SELECT age, COUNT(*) AS cnt, AVG(age) AS avg_age FROM users "
                         "GROUP BY age HAVING COUNT(*) > 1 ORDER BY avg_age DESC");
    EXPECT_EQ(bound.output_columns.size(), 3u);
}

TEST_F(BinderTest, SelectOrderByUnknownAlias) {
    bind_error("SELECT name FROM users ORDER BY nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, SelectLimitOffset) {
    auto bound = bind_ok("SELECT id FROM users LIMIT 10 OFFSET 5");
    EXPECT_EQ(bound.output_columns.size(), 1u);
}

TEST_F(BinderTest, SelectUnionColumnMatch) {
    auto bound = bind_ok("SELECT id, name FROM users UNION SELECT id, name FROM products");
    EXPECT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(BinderTest, SelectUnionColumnCountMismatch) {
    bind_error("SELECT id, name FROM users UNION SELECT id FROM products", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, SelectTableNotFound) {
    bind_error("SELECT * FROM nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, SelectColumnNotFound) {
    bind_error("SELECT nonexistent FROM users", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, SelectSubqueryInFrom) {
    auto bound = bind_ok("SELECT sub.id FROM (SELECT id FROM users) AS sub");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(BinderTest, SelectWithCTE) {
    auto bound = bind_ok("WITH active_users AS (SELECT id, name FROM users WHERE active) "
                         "SELECT id FROM active_users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
}

// ===========================================================================
// DML binding tests
// ===========================================================================

TEST_F(BinderTest, InsertValid) {
    auto bound = bind_ok("INSERT INTO users (id, name) VALUES (1, 'Alice')");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, InsertColumnCountMismatch) {
    bind_error("INSERT INTO users (id, name) VALUES (1)", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, InsertTableNotFound) {
    bind_error("INSERT INTO nonexistent (id) VALUES (1)", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, InsertColumnNotFound) {
    bind_error("INSERT INTO users (id, nonexistent) VALUES (1, 2)", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, InsertReturning) {
    auto bound = bind_ok("INSERT INTO users (id, name) VALUES (1, 'Alice') RETURNING id");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "id");
}

TEST_F(BinderTest, UpdateValid) {
    auto bound = bind_ok("UPDATE users SET name = 'Bob' WHERE id = 1");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, UpdateColumnNotFound) {
    bind_error("UPDATE users SET nonexistent = 1 WHERE id = 1", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, UpdateTableNotFound) {
    bind_error("UPDATE nonexistent SET id = 1", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, DeleteValid) {
    auto bound = bind_ok("DELETE FROM users WHERE id = 1");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, DeleteTableNotFound) {
    bind_error("DELETE FROM nonexistent WHERE id = 1", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, DeleteReturning) {
    auto bound = bind_ok("DELETE FROM users WHERE id = 1 RETURNING id, name");
    ASSERT_EQ(bound.output_columns.size(), 2u);
}

// ===========================================================================
// DDL binding tests
// ===========================================================================

TEST_F(BinderTest, CreateTableValid) {
    auto bound = bind_ok("CREATE TABLE new_table (id INT PRIMARY KEY, name TEXT)");
    EXPECT_NE(bound.stmt, nullptr);
}

TEST_F(BinderTest, CreateTableDuplicate) {
    bind_error("CREATE TABLE users (id INT)", StatusCode::ALREADY_EXISTS);
}

TEST_F(BinderTest, CreateTableIfNotExists) {
    auto bound = bind_ok("CREATE TABLE IF NOT EXISTS users (id INT)");
    EXPECT_NE(bound.stmt, nullptr);
}

TEST_F(BinderTest, CreateTableFKUnknownTable) {
    bind_error("CREATE TABLE bad (id INT, user_id INT REFERENCES nonexistent(id))",
               StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, DropTableValid) {
    auto bound = bind_ok("DROP TABLE users");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, DropTableNotFound) {
    bind_error("DROP TABLE nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, DropTableIfExists) {
    auto bound = bind_ok("DROP TABLE IF EXISTS nonexistent");
    EXPECT_NE(bound.stmt, nullptr);
}

TEST_F(BinderTest, CreateIndexValid) {
    // Create a test index first.
    auto bound = bind_ok("CREATE INDEX idx_users_name ON users (name)");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, CreateIndexColumnNotFound) {
    bind_error("CREATE INDEX idx ON users (nonexistent)", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, CreateIndexTableNotFound) {
    bind_error("CREATE INDEX idx ON nonexistent (id)", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, CreateEdgeTypeValid) {
    auto bound = bind_ok("CREATE EDGE TYPE mentors (since TIMESTAMP) FROM users TO users");
    EXPECT_EQ(bound.referenced_tables.size(), 2u);
}

TEST_F(BinderTest, CreateEdgeTypeFromTableNotFound) {
    bind_error("CREATE EDGE TYPE bad FROM nonexistent TO users", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, CreateEdgeTypeToTableNotFound) {
    bind_error("CREATE EDGE TYPE bad FROM users TO nonexistent", StatusCode::NOT_FOUND);
}

// ===========================================================================
// Graph / Vector binding tests
// ===========================================================================

TEST_F(BinderTest, LinkValid) {
    auto bound = bind_ok("LINK users(1) TO products(1) VIA purchased");
    EXPECT_FALSE(bound.referenced_edge_types.empty());
    EXPECT_EQ(bound.referenced_tables.size(), 2u);
}

TEST_F(BinderTest, LinkEdgeNotFound) {
    bind_error("LINK users(1) TO products(1) VIA nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, LinkSourceTableMismatch) {
    bind_error("LINK products(1) TO products(1) VIA purchased", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, LinkTargetTableMismatch) {
    bind_error("LINK users(1) TO users(1) VIA purchased", StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, UnlinkValid) {
    auto bound = bind_ok("UNLINK users(1) FROM products(1) VIA purchased");
    EXPECT_FALSE(bound.referenced_edge_types.empty());
}

TEST_F(BinderTest, TraverseValid) {
    auto bound = bind_ok("TRAVERSE purchased FROM users(1)");
    EXPECT_FALSE(bound.referenced_edge_types.empty());
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, TraverseEdgeNotFound) {
    bind_error("TRAVERSE nonexistent FROM users(1)", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, TraverseTableNotFound) {
    bind_error("TRAVERSE purchased FROM nonexistent(1)", StatusCode::NOT_FOUND);
}

// ===========================================================================
// SELECT ... FROM TRAVERSE binding tests (GDB-264)
// ===========================================================================

// AC1: SELECT name resolves from the target table (self-referential: users→users).
TEST_F(BinderTest, TraverseFromResolvesTargetColumn) {
    auto bound = bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

// AC2: SELECT __depth resolves the meta-column with correct type.
TEST_F(BinderTest, TraverseFromResolvesMetaDepth) {
    auto bound = bind_ok("SELECT __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__depth");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
}

// AC2: __node meta-column resolves with PK type.
TEST_F(BinderTest, TraverseFromResolvesMetaNode) {
    auto bound = bind_ok("SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__node");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32); // users PK type
}

// AC2: __source meta-column is nullable.
TEST_F(BinderTest, TraverseFromResolvesMetaSource) {
    auto bound = bind_ok("SELECT __source FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "__source");
    EXPECT_TRUE(bound.output_columns[0].nullable);
}

// AC3: SELECT nonexistent column produces a binding error.
TEST_F(BinderTest, TraverseFromNonexistentColumnError) {
    bind_error("SELECT nonexistent FROM TRAVERSE follows FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

// AC4: Heterogeneous edges — OUT traversal exposes target table columns.
TEST_F(BinderTest, TraverseFromHeterogeneousOutExposesTargetColumns) {
    auto bound = bind_ok("SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "title");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

// AC4: Heterogeneous edges — user columns are NOT accessible from OUT traversal.
TEST_F(BinderTest, TraverseFromHeterogeneousOutDoesNotExposeSourceColumns) {
    // 'email' is a users column, not a posts column.
    bind_error("SELECT email FROM TRAVERSE authored FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

// AC5: Error when FROM table is not an endpoint of the edge type.
TEST_F(BinderTest, TraverseFromTableNotEndpointError) {
    bind_error("SELECT id FROM TRAVERSE authored FROM orders(1) DIRECTION OUT",
               StatusCode::TYPE_ERROR);
}

// AC6: Edge property columns accessible via edge type qualifier.
TEST_F(BinderTest, TraverseFromEdgePropertyQualified) {
    auto bound = bind_ok("SELECT follows.weight FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "weight");
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

// AC7: Internal from_key and where_expr are bound correctly.
TEST_F(BinderTest, TraverseFromWithWhereBindsCorrectly) {
    auto bound =
        bind_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT WHERE __depth < 3");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].column_name, "name");
}

// Edge not found in SELECT FROM TRAVERSE.
TEST_F(BinderTest, TraverseFromEdgeNotFoundError) {
    bind_error("SELECT id FROM TRAVERSE nonexistent FROM users(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

// FROM table not found in SELECT FROM TRAVERSE.
TEST_F(BinderTest, TraverseFromSourceTableNotFoundError) {
    bind_error("SELECT id FROM TRAVERSE follows FROM nonexistent(1) DIRECTION OUT",
               StatusCode::NOT_FOUND);
}

// Star expansion includes target columns + meta-columns + edge properties.
TEST_F(BinderTest, TraverseFromStarExpansion) {
    auto bound = bind_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    // users has 5 cols + 3 meta (__node, __depth, __source) + 1 edge prop (weight) = 9
    EXPECT_EQ(bound.output_columns.size(), 9u);
}

TEST_F(BinderTest, NearestValid) {
    auto bound = bind_ok("SELECT * FROM products WHERE NEAREST(desc_vector, 5) TO [1.0, 2.0, 3.0]");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, NearestNonEmbeddingError) {
    bind_error("SELECT * FROM products WHERE NEAREST(name, 5) TO [1.0, 2.0]",
               StatusCode::TYPE_ERROR);
}

TEST_F(BinderTest, NearestColumnNotFound) {
    bind_error("SELECT * FROM products WHERE NEAREST(nonexistent, 5) TO [1.0]",
               StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, NearestTableNotFound) {
    bind_error("SELECT * FROM nonexistent WHERE NEAREST(col, 5) TO [1.0]", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, ShortestPathValid) {
    auto bound = bind_ok("SHORTEST PATH FROM users(1) TO users(2) VIA purchased");
    EXPECT_FALSE(bound.referenced_edge_types.empty());
    EXPECT_EQ(bound.referenced_tables.size(), 2u);
}

TEST_F(BinderTest, ShortestPathEdgeNotFound) {
    bind_error("SHORTEST PATH FROM users(1) TO users(2) VIA nonexistent", StatusCode::NOT_FOUND);
}

// ===========================================================================
// Admin binding tests
// ===========================================================================

TEST_F(BinderTest, ExplainBindsInner) {
    auto bound = bind_ok("EXPLAIN SELECT id FROM users");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, ExplainPropagatesError) {
    bind_error("EXPLAIN SELECT * FROM nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, DescribeValid) {
    auto bound = bind_ok("DESCRIBE users");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, DescribeTableNotFound) {
    bind_error("DESCRIBE nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, ShowColumnsValid) {
    auto bound = bind_ok("SHOW COLUMNS FROM users");
    EXPECT_FALSE(bound.referenced_tables.empty());
}

TEST_F(BinderTest, ShowColumnsTableNotFound) {
    bind_error("SHOW COLUMNS FROM nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(BinderTest, ShowTablesPassthrough) {
    auto bound = bind_ok("SHOW TABLES");
    EXPECT_NE(bound.stmt, nullptr);
}

TEST_F(BinderTest, BeginPassthrough) {
    auto bound = bind_ok("BEGIN");
    EXPECT_NE(bound.stmt, nullptr);
}

TEST_F(BinderTest, CommitPassthrough) {
    auto bound = bind_ok("COMMIT");
    EXPECT_NE(bound.stmt, nullptr);
}

TEST_F(BinderTest, RollbackPassthrough) {
    auto bound = bind_ok("ROLLBACK");
    EXPECT_NE(bound.stmt, nullptr);
}

} // namespace sixseven
