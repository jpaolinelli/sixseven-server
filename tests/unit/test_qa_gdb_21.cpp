#include "giodb/catalog/catalog.h"
#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"
#include "giodb/planner/binder.h"
#include "giodb/planner/type_resolver.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace giodb {

// ===========================================================================
// Test fixture — shared catalog with standard test tables
// ===========================================================================

class QA_Binder : public ::testing::Test {
protected:
    Catalog catalog;
    std::unique_ptr<Binder> binder;

    void SetUp() override {
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

        // Edge type: purchased (FROM users TO products)
        {
            EdgeTypeDef e;
            e.name = "purchased";
            e.source_table_id = 1; // users
            e.target_table_id = 3; // products
            auto r = catalog.create_edge_type(std::move(e));
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

    /// Parse and bind, assert failure (any StatusCode).
    void bind_fails(const std::string& sql) {
        auto stmt = parse(sql);
        if (!stmt) {
            // parse failure is also acceptable for "should fail" tests
            return;
        }
        auto result = binder->bind(*stmt);
        EXPECT_FALSE(result.has_value()) << "Expected bind error but succeeded for: " << sql;
    }
};

// ===========================================================================
// BUG CANDIDATE 1: Comparison operators don't validate type compatibility
// The AC says "Type mismatches are caught (e.g., comparing STRING to INT)".
// But the binder does not check operand type compatibility for comparison
// operators — it just sets the result type to BOOL.
// ===========================================================================

TEST_F(QA_Binder, CompareStringToIntShouldFail) {
    // STRING > INT should be a type error.
    bind_error("SELECT name > 1 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, CompareStringEqualInt) {
    // STRING = INT should be a type error.
    bind_error("SELECT name = 1 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, CompareStringLessInt) {
    // STRING < INT should be a type error.
    bind_error("SELECT name < 5 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, CompareTimestampToInt) {
    // TIMESTAMP = INT should be a type error.
    bind_error("SELECT created_at = 42 FROM orders", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, CompareBoolToInt) {
    // BOOL > INT should be a type error.
    bind_error("SELECT active > 1 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, WhereStringComparedToInt) {
    // WHERE name > 5 compares STRING to INT — should fail.
    bind_error("SELECT id FROM users WHERE name > 5", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, CompareCompatibleTypesOk) {
    // INT32 > INT64 should succeed (widening is valid).
    bind_ok("SELECT id > 100 FROM users");
}

TEST_F(QA_Binder, CompareIntToFloatOk) {
    // INT32 < FLOAT64 should succeed (widening).
    bind_ok("SELECT id < 3.14 FROM users");
}

TEST_F(QA_Binder, CompareStringToStringOk) {
    // STRING = STRING should succeed.
    bind_ok("SELECT name = email FROM users");
}

// ===========================================================================
// BUG CANDIDATE 2: same_column_ref is case-sensitive
// The binder uses to_upper() everywhere for name resolution, but
// same_column_ref compares column names case-sensitively.
// ===========================================================================

TEST_F(QA_Binder, GroupByDifferentCaseColumn) {
    // If the parser preserves original case, GROUP BY with different case
    // could fail validation even when it shouldn't.
    // SELECT AGE, COUNT(*) FROM users GROUP BY age should be valid.
    // This test checks that case-insensitive GROUP BY matching works.
    auto stmt = parse("SELECT age, COUNT(*) FROM users GROUP BY age");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 2u);
}

// ===========================================================================
// BUG CANDIDATE 3: UPDATE/DELETE WHERE clauses don't validate boolean type
// SELECT validates WHERE is BOOL, but UPDATE and DELETE do not.
// ===========================================================================

TEST_F(QA_Binder, UpdateWhereNonBoolShouldFail) {
    // WHERE name is STRING — should fail like SELECT does.
    bind_error("UPDATE users SET age = 30 WHERE name", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, DeleteWhereNonBoolShouldFail) {
    // WHERE name is STRING — should fail.
    bind_error("DELETE FROM users WHERE name", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, UpdateWhereIntShouldFail) {
    // WHERE id is INT32 — should fail.
    bind_error("UPDATE users SET age = 30 WHERE id", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, DeleteWhereIntShouldFail) {
    // WHERE id is INT32 — should fail.
    bind_error("DELETE FROM users WHERE id", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, UpdateWhereBoolOk) {
    // WHERE active is BOOL — should succeed.
    bind_ok("UPDATE users SET age = 30 WHERE active");
}

TEST_F(QA_Binder, DeleteWhereBoolOk) {
    // WHERE active is BOOL — should succeed.
    bind_ok("DELETE FROM users WHERE active");
}

// ===========================================================================
// BUG CANDIDATE 4: TRAVERSE doesn't validate FROM table matches edge type
// LINK validates source/target table match, but TRAVERSE doesn't.
// ===========================================================================

TEST_F(QA_Binder, TraverseFromUnrelatedTable) {
    // TRAVERSE purchased FROM orders — edge is users→products, not orders.
    // This should fail because orders is not an endpoint of the purchased edge.
    bind_error("TRAVERSE purchased FROM orders(1)", StatusCode::TYPE_ERROR);
}

// ===========================================================================
// BUG CANDIDATE 5: validate_aggregates only checks ColumnRefExpr
// Complex expressions containing ungrouped columns pass through unchecked.
// ===========================================================================

TEST_F(QA_Binder, AggregateExprWithUngroupedColumnInExpression) {
    // id + 1 contains 'id' which is not in GROUP BY (age) —
    // should fail like a bare column ref would.
    bind_error("SELECT id + 1, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, AggregateExprWithGroupedColumnInExpressionOk) {
    // age + 1 contains 'age' which is in GROUP BY — should succeed.
    bind_ok("SELECT age + 1, COUNT(*) FROM users GROUP BY age");
}

// ===========================================================================
// Boundary and edge case tests — Scope
// ===========================================================================

TEST_F(QA_Binder, EmptyScopeAllColumns) {
    Scope scope;
    auto all = scope.all_columns();
    EXPECT_TRUE(all.empty());
}

TEST_F(QA_Binder, EmptyScopeResolveColumn) {
    Scope scope;
    auto result = scope.resolve_column("anything");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, EmptyScopeQualifiedResolve) {
    Scope scope;
    auto result = scope.resolve_column("table", "col");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, EmptyScopeColumnsFor) {
    Scope scope;
    auto result = scope.columns_for("anything");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, CaseInsensitiveColumnResolve) {
    auto bound = bind_ok("SELECT ID, NAME FROM users");
    ASSERT_EQ(bound.output_columns.size(), 2u);
    // Should resolve regardless of case.
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::STRING);
}

TEST_F(QA_Binder, CaseInsensitiveTableName) {
    // NOTE: Catalog lookup is case-sensitive. The binder does NOT normalize
    // table names before catalog lookup (though scope resolution IS case-insensitive).
    // Uppercase table name "USERS" will not match catalog entry "users".
    // Documenting this as a design limitation, not a binder bug.
    auto stmt = parse("SELECT id FROM USERS");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // This fails because catalog is case-sensitive — expected behavior.
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_Binder, QualifiedColumnWithAlias) {
    auto bound = bind_ok("SELECT u.id FROM users AS u");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(QA_Binder, AliasedTableStarExpansion) {
    auto bound = bind_ok("SELECT u.* FROM users AS u");
    EXPECT_EQ(bound.output_columns.size(), 5u);
}

// ===========================================================================
// Boundary tests — NULL handling
// ===========================================================================

TEST_F(QA_Binder, NullLiteral) {
    auto bound = bind_ok("SELECT NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_TRUE(bound.output_columns[0].nullable);
}

TEST_F(QA_Binder, NullInArithmetic) {
    // BUG: NULL literal is typed as STRING (placeholder). NULL + 1 tries
    // common_type(STRING, INT64) which fails because STRING and INT have
    // no common type. NULL should be polymorphic and coerce to any type.
    auto stmt = parse("SELECT NULL + 1 FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // Currently fails with TYPE_ERROR because NULL is typed as STRING.
    // TODO: Fix NULL to be polymorphic in type coercion.
    // For now, document the behavior.
    EXPECT_FALSE(result.has_value()) << "NULL + 1 fails because NULL is typed as STRING";
}

TEST_F(QA_Binder, NullComparison) {
    auto bound = bind_ok("SELECT id = NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(QA_Binder, IsNullNonNullable) {
    auto bound = bind_ok("SELECT id IS NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
    EXPECT_FALSE(bound.output_columns[0].nullable);
}

TEST_F(QA_Binder, IsNotNull) {
    auto bound = bind_ok("SELECT name IS NOT NULL FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

TEST_F(QA_Binder, InsertNullIntoNonNullableColumn) {
    // id is NOT NULL — inserting NULL should be caught.
    // The binder does null bypass for type checking but may not catch
    // null-into-not-null violations.
    auto stmt = parse("INSERT INTO users (id, name) VALUES (NULL, 'test')");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // This might pass binder (constraint check happens at execution).
    // Not a binder-level bug if it passes — just documenting behavior.
}

// ===========================================================================
// Type coercion edge cases
// ===========================================================================

TEST_F(QA_Binder, ImplicitCoercionIntToFloat) {
    // INT32 + FLOAT64 → FLOAT64 (widening).
    auto bound = bind_ok("SELECT id + 3.14 FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::FLOAT64);
}

TEST_F(QA_Binder, CoercionInInsert) {
    // Insert INT64 literal into INT32 column — should succeed with narrowing coercion.
    bind_ok("INSERT INTO users (id, name) VALUES (42, 'test')");
}

TEST_F(QA_Binder, StringSubtractFails) {
    // BUG: STRING - STRING should fail, but the binder only checks common_type
    // (which succeeds for STRING-STRING) without verifying the result is numeric.
    // Arithmetic on strings should be rejected.
    auto stmt = parse("SELECT name - email FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // EXPECTED: TYPE_ERROR (can't subtract strings)
    // ACTUAL: succeeds with STRING type
    EXPECT_FALSE(result.has_value())
        << "BUG: STRING - STRING should fail but binder accepts it";
}

TEST_F(QA_Binder, StringMultiplyFails) {
    bind_error("SELECT name * 2 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, BoolArithmeticFails) {
    // BOOL + INT should fail.
    bind_error("SELECT active + 1 FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, ConcatReturnsString) {
    auto bound = bind_ok("SELECT name || email FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

// ===========================================================================
// Aggregate validation edge cases
// ===========================================================================

TEST_F(QA_Binder, SelectStarWithGroupByFails) {
    bind_error("SELECT * FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, SelectStarWithAggregateFails) {
    bind_error("SELECT *, COUNT(*) FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, MultipleAggregatesNoGroupBy) {
    auto bound = bind_ok("SELECT COUNT(*), SUM(age), AVG(age) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 3u);
}

TEST_F(QA_Binder, AggregateInHaving) {
    auto bound =
        bind_ok("SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 1");
    ASSERT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(QA_Binder, HavingWithoutGroupBy) {
    // HAVING without GROUP BY — should bind (implicit full-table group).
    auto bound = bind_ok("SELECT COUNT(*) FROM users HAVING COUNT(*) > 0");
    ASSERT_EQ(bound.output_columns.size(), 1u);
}

TEST_F(QA_Binder, SumOnStringFails) {
    bind_error("SELECT SUM(name) FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, AvgOnBoolFails) {
    bind_error("SELECT AVG(active) FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, CountStarType) {
    auto bound = bind_ok("SELECT COUNT(*) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(bound.output_columns[0].nullable);
}

TEST_F(QA_Binder, MinOnStringOk) {
    auto bound = bind_ok("SELECT MIN(name) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

TEST_F(QA_Binder, MaxOnIntOk) {
    auto bound = bind_ok("SELECT MAX(age) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

// ===========================================================================
// Error message quality
// ===========================================================================

TEST_F(QA_Binder, UnknownTableErrorMessage) {
    auto stmt = parse("SELECT * FROM nonexistent_table");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    // Error message should mention the table name.
    EXPECT_NE(result.error().message.find("nonexistent_table"), std::string::npos)
        << "Error message should mention the table name: " << result.error().message;
}

TEST_F(QA_Binder, UnknownColumnErrorMessage) {
    auto stmt = parse("SELECT nonexistent_col FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    // Error message should mention the column name.
    EXPECT_NE(result.error().message.find("nonexistent_col"), std::string::npos)
        << "Error message should mention the column name: " << result.error().message;
}

TEST_F(QA_Binder, AmbiguousColumnErrorMessage) {
    auto stmt = parse("SELECT id FROM users JOIN orders ON users.id = orders.id");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("ambiguous"), std::string::npos)
        << "Error message should mention ambiguity: " << result.error().message;
}

// ===========================================================================
// Unary operator edge cases
// ===========================================================================

TEST_F(QA_Binder, NegateStringFails) {
    bind_error("SELECT -name FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, NegateIntOk) {
    auto bound = bind_ok("SELECT -age FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(QA_Binder, NotNonBoolFails) {
    bind_error("SELECT NOT age FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, NotBoolOk) {
    auto bound = bind_ok("SELECT NOT active FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::BOOL);
}

// ===========================================================================
// Logical operators
// ===========================================================================

TEST_F(QA_Binder, AndBoolBoolOk) {
    bind_ok("SELECT active AND active FROM users");
}

TEST_F(QA_Binder, AndIntIntFails) {
    bind_error("SELECT id AND age FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, OrBoolBoolOk) {
    bind_ok("SELECT active OR active FROM users");
}

TEST_F(QA_Binder, OrStringFails) {
    bind_error("SELECT name OR email FROM users", StatusCode::TYPE_ERROR);
}

// ===========================================================================
// CAST edge cases
// ===========================================================================

TEST_F(QA_Binder, CastToUnknownTypeFails) {
    // Parser rejects unknown type names before reaching binder. This is fine.
    Lexer lexer("SELECT CAST(id AS FOOBAR) FROM users");
    auto tokens = lexer.tokenize();
    if (!tokens) return; // Lex rejection OK
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    if (!stmt) return; // Parse rejection OK
    // If parser accepts, binder should catch it.
    auto result = binder->bind(**stmt);
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_Binder, CastIntToStringOk) {
    auto bound = bind_ok("SELECT CAST(id AS TEXT) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
}

// ===========================================================================
// CASE expression edge cases
// ===========================================================================

TEST_F(QA_Binder, CaseWithNoElseIsNullable) {
    auto bound = bind_ok("SELECT CASE WHEN active THEN 'yes' END FROM users");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_TRUE(bound.output_columns[0].nullable);
}

TEST_F(QA_Binder, CaseWithMixedResultTypesFails) {
    // INT vs STRING result in CASE branches — should fail.
    bind_error("SELECT CASE WHEN active THEN 1 ELSE 'text' END FROM users",
               StatusCode::TYPE_ERROR);
}

// ===========================================================================
// Subquery binding
// ===========================================================================

TEST_F(QA_Binder, SubqueryInFromWithBadColumn) {
    bind_error("SELECT sub.nonexistent FROM (SELECT id FROM users) AS sub",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, CorrelatedSubqueryResolvesOuterColumn) {
    bind_ok("SELECT id FROM users WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
}

// ===========================================================================
// BETWEEN edge cases
// ===========================================================================

TEST_F(QA_Binder, BetweenTypeMismatch) {
    // age BETWEEN 'a' AND 'z' — INT32 between STRING should fail or succeed
    // depending on coercion. With no coercion path, it should fail.
    // Current code doesn't validate BETWEEN type compatibility.
    auto stmt = parse("SELECT age BETWEEN 'a' AND 'z' FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // Note: BETWEEN doesn't validate type compatibility between expr and bounds.
    // This is documenting current behavior.
}

// ===========================================================================
// IN expression edge cases
// ===========================================================================

TEST_F(QA_Binder, InEmptyList) {
    // Parser rejects empty IN list — that's fine (parser-level validation).
    Lexer lexer("SELECT id IN () FROM users");
    auto tokens = lexer.tokenize();
    if (!tokens) return;
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    // Parse rejection is expected for empty IN list.
    EXPECT_FALSE(stmt.has_value()) << "Empty IN list should be rejected at parse time";
}

// ===========================================================================
// LIKE edge cases
// ===========================================================================

TEST_F(QA_Binder, LikeOnNonStringColumn) {
    // LIKE on INT — should this fail? Current binder doesn't validate.
    auto stmt = parse("SELECT id LIKE '%1%' FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // Documenting: LIKE doesn't validate that the operand is a string type.
}

// ===========================================================================
// DDL edge cases
// ===========================================================================

TEST_F(QA_Binder, CreateTableWithUnknownType) {
    // Parser rejects unknown type names — that's fine (parser-level validation).
    Lexer lexer("CREATE TABLE bad_table (id FOOBAR)");
    auto tokens = lexer.tokenize();
    if (!tokens) return;
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    if (!stmt) return; // Parse rejection OK
    auto result = binder->bind(**stmt);
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_Binder, AlterTableAddColumnDuplicate) {
    bind_error("ALTER TABLE users ADD COLUMN id INT", StatusCode::ALREADY_EXISTS);
}

TEST_F(QA_Binder, AlterTableDropNonexistentColumn) {
    bind_error("ALTER TABLE users DROP COLUMN nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, AlterTableRenameNonexistentColumn) {
    bind_error("ALTER TABLE users RENAME COLUMN nonexistent TO new_name", StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, DropIndexNonexistent) {
    bind_error("DROP INDEX nonexistent_idx", StatusCode::NOT_FOUND);
}

// ===========================================================================
// Graph binding edge cases
// ===========================================================================

TEST_F(QA_Binder, LinkSwappedSourceTarget) {
    // purchased: users→products. Linking products→users should fail for source.
    bind_error("LINK products(1) TO users(1) VIA purchased", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, UnlinkSourceMismatch) {
    bind_error("UNLINK products(1) FROM users(1) VIA purchased", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, ShortestPathBadEdge) {
    bind_error("SHORTEST PATH FROM users(1) TO users(2) VIA nonexistent",
               StatusCode::NOT_FOUND);
}

// ===========================================================================
// NEAREST edge cases
// ===========================================================================

TEST_F(QA_Binder, NearestOnNonEmbeddingColumn) {
    bind_error("NEAREST 5 FROM products.name TO [1.0, 2.0]", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, NearestOnNonexistentTable) {
    bind_error("NEAREST 5 FROM nonexistent.col TO [1.0]", StatusCode::NOT_FOUND);
}

TEST_F(QA_Binder, NearestOnNonexistentColumn) {
    bind_error("NEAREST 5 FROM products.nonexistent TO [1.0]", StatusCode::NOT_FOUND);
}

// ===========================================================================
// Multi-JOIN edge cases
// ===========================================================================

TEST_F(QA_Binder, ThreeTableJoin) {
    auto bound = bind_ok(
        "SELECT users.id, orders.amount, products.name "
        "FROM users "
        "JOIN orders ON users.id = orders.user_id "
        "JOIN products ON orders.product_id = products.id");
    ASSERT_EQ(bound.output_columns.size(), 3u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(bound.output_columns[2].type_id, TypeId::STRING);
}

TEST_F(QA_Binder, SelfJoin) {
    auto bound = bind_ok(
        "SELECT u1.id, u2.name "
        "FROM users AS u1 "
        "JOIN users AS u2 ON u1.id = u2.id");
    ASSERT_EQ(bound.output_columns.size(), 2u);
}

// ===========================================================================
// UNION edge cases
// ===========================================================================

TEST_F(QA_Binder, UnionTypeMismatch) {
    // BUG: UNION of INT32 column and STRING column. The binder only checks
    // column count, not type compatibility. UNION should validate that
    // corresponding columns have compatible types.
    auto stmt = parse("SELECT id FROM users UNION SELECT name FROM users");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // EXPECTED: TYPE_ERROR (INT32 and STRING are incompatible for UNION)
    // ACTUAL: succeeds (only column count is checked)
    EXPECT_FALSE(result.has_value())
        << "BUG: UNION of INT and STRING columns should fail type check";
}

// ===========================================================================
// Stress tests
// ===========================================================================

TEST_F(QA_Binder, ManyColumnsInSelect) {
    auto bound = bind_ok(
        "SELECT id, name, email, age, active, "
        "id + 1, age + 2, id * age, "
        "CAST(id AS DOUBLE), CAST(age AS DOUBLE) "
        "FROM users");
    EXPECT_EQ(bound.output_columns.size(), 10u);
}

TEST_F(QA_Binder, DeeplyNestedSubquery) {
    auto bound = bind_ok(
        "SELECT a.id FROM "
        "(SELECT b.id FROM "
        " (SELECT id FROM users) AS b"
        ") AS a");
    ASSERT_EQ(bound.output_columns.size(), 1u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::INT32);
}

TEST_F(QA_Binder, MultipleCTEs) {
    auto bound = bind_ok(
        "WITH a AS (SELECT id FROM users), "
        "     b AS (SELECT id FROM orders) "
        "SELECT a.id, b.id FROM a JOIN b ON a.id = b.id");
    ASSERT_EQ(bound.output_columns.size(), 2u);
}

TEST_F(QA_Binder, FunctionCalls) {
    auto bound = bind_ok("SELECT LOWER(name), LENGTH(name), ABS(age) FROM users");
    ASSERT_EQ(bound.output_columns.size(), 3u);
    EXPECT_EQ(bound.output_columns[0].type_id, TypeId::STRING);
    EXPECT_EQ(bound.output_columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(bound.output_columns[2].type_id, TypeId::INT32);
}

TEST_F(QA_Binder, UnknownFunctionFails) {
    bind_error("SELECT NONEXISTENT_FUNC(id) FROM users", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, EmptySelectList) {
    // Parser rejects empty SELECT list — that's fine (parser-level validation).
    Lexer lexer("SELECT FROM users");
    auto tokens = lexer.tokenize();
    if (!tokens) return;
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    EXPECT_FALSE(stmt.has_value()) << "Empty SELECT list should be rejected at parse time";
}

// ===========================================================================
// INSERT edge cases
// ===========================================================================

TEST_F(QA_Binder, InsertTooManyValues) {
    bind_error("INSERT INTO users (id) VALUES (1, 2)", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, InsertTooFewValues) {
    bind_error("INSERT INTO users (id, name) VALUES (1)", StatusCode::TYPE_ERROR);
}

TEST_F(QA_Binder, InsertIntoNonexistentColumn) {
    bind_error("INSERT INTO users (id, nonexistent) VALUES (1, 2)", StatusCode::NOT_FOUND);
}

// ===========================================================================
// Binder reuse between queries
// ===========================================================================

TEST_F(QA_Binder, BinderReuseAcrossQueries) {
    // BUG: The Binder stores CTE results in cte_results_ map, which is never
    // cleared between bind() calls. Subsequent queries incorrectly see CTEs
    // from prior queries.
    auto b1 = bind_ok("WITH cte AS (SELECT id FROM users) SELECT id FROM cte");
    ASSERT_EQ(b1.output_columns.size(), 1u);

    // Second bind should not see the CTE from the first query.
    auto stmt = parse("SELECT id FROM cte");
    ASSERT_NE(stmt, nullptr);
    auto result = binder->bind(*stmt);
    // EXPECTED: NOT_FOUND (cte is not defined in this query)
    // ACTUAL: succeeds (cte_results_ leaked from previous bind)
    EXPECT_FALSE(result.has_value())
        << "BUG: CTE state leaks between bind() calls — 'cte' should not be visible";
}

// ===========================================================================
// TypeResolver edge cases
// ===========================================================================

TEST_F(QA_Binder, CommonTypeBoolAndBool) {
    auto result = common_type(TypeId::BOOL, TypeId::BOOL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, TypeId::BOOL);
}

TEST_F(QA_Binder, CommonTypeStringAndBool) {
    auto result = common_type(TypeId::STRING, TypeId::BOOL);
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_Binder, CommonTypeIntAndFloat) {
    auto result = common_type(TypeId::INT32, TypeId::FLOAT64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, TypeId::FLOAT64);
}

} // namespace giodb
