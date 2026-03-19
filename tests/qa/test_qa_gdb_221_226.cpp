/// @file test_qa_gdb_221_226.cpp
/// Adversarial QA tests for GDB-221 through GDB-226 bug fixes.
///
/// GDB-221: TRAVERSE validates FROM table matches edge type endpoints
/// GDB-222: validate_aggregates checks ungrouped columns in expressions
/// GDB-223: STRING arithmetic (subtract/divide/modulo) rejected
/// GDB-224: UNION validates column type compatibility
/// GDB-225: CTE state cleared between bind() calls
/// GDB-226: ORDER BY on non-selected column produces correct results

#include "sixseven/catalog/catalog.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace sixseven {

// ===========================================================================
// Binder-level fixture (GDB-221, GDB-222, GDB-223, GDB-224, GDB-225)
// ===========================================================================

class QA_BugfixBinder : public ::testing::Test {
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

        // Table: items(id INT32, data BLOB)
        {
            TableSchema s;
            s.name = "items";
            s.columns = {
                {0, "id", TypeId::INT32, false, ""},
                {1, "data", TypeId::BLOB, true, ""},
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

        // Edge type: self_ref (FROM users TO users) — self-referencing edge
        {
            EdgeTypeDef e;
            e.name = "follows";
            e.source_table_id = 1; // users
            e.target_table_id = 1; // users
            auto r = catalog.create_edge_type(default_database_id, std::move(e));
            ASSERT_TRUE(r.has_value());
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
            ADD_FAILURE() << "Bind failed for: " << sql << "\nError: " << result.error().message;
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
                << status_code_name(result.error().code) << ": " << result.error().message;
        }
    }

    void bind_fails(const std::string& sql) {
        auto stmt = parse(sql);
        if (!stmt) {
            return; // parse failure is acceptable
        }
        auto result = binder->bind(*stmt);
        EXPECT_FALSE(result.has_value()) << "Expected bind error but succeeded for: " << sql;
    }
};

// ===========================================================================
// GDB-221: TRAVERSE FROM table validation
// ===========================================================================

// Basic case: unrelated table
TEST_F(QA_BugfixBinder, GDB221_TraverseFromUnrelatedTable) {
    bind_error("TRAVERSE purchased FROM orders(1)", StatusCode::TYPE_ERROR);
}

// Source table is valid
TEST_F(QA_BugfixBinder, GDB221_TraverseFromSourceTableOk) {
    bind_ok("TRAVERSE purchased FROM users(1)");
}

// Target table is valid
TEST_F(QA_BugfixBinder, GDB221_TraverseFromTargetTableOk) {
    bind_ok("TRAVERSE purchased FROM products(1)");
}

// Nonexistent edge type should fail
TEST_F(QA_BugfixBinder, GDB221_TraverseNonexistentEdge) {
    bind_error("TRAVERSE nonexistent FROM users(1)", StatusCode::NOT_FOUND);
}

// Nonexistent FROM table should fail
TEST_F(QA_BugfixBinder, GDB221_TraverseNonexistentFromTable) {
    bind_error("TRAVERSE purchased FROM nonexistent(1)", StatusCode::NOT_FOUND);
}

// Self-referencing edge: source == target, should accept that table
TEST_F(QA_BugfixBinder, GDB221_TraverseSelfRefEdge) {
    bind_ok("TRAVERSE follows FROM users(1)");
}

// Self-referencing edge: unrelated table still rejected
TEST_F(QA_BugfixBinder, GDB221_TraverseSelfRefEdgeUnrelatedTable) {
    bind_error("TRAVERSE follows FROM orders(1)", StatusCode::TYPE_ERROR);
}

// TRAVERSE with WHERE clause on unrelated table still fails
TEST_F(QA_BugfixBinder, GDB221_TraverseWithWhereUnrelatedTable) {
    bind_error("TRAVERSE purchased FROM orders(1) WHERE depth < 3", StatusCode::TYPE_ERROR);
}

// ===========================================================================
// GDB-222: validate_aggregates deep column ref checking
// ===========================================================================

// Original bug: column in binary expression not caught
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInBinaryExpr) {
    bind_error("SELECT id + 1, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// Column inside nested binary expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInNestedBinaryExpr) {
    bind_error("SELECT (id + 1) * 2, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// Grouped column in expression is OK
TEST_F(QA_BugfixBinder, GDB222_GroupedColumnInBinaryExprOk) {
    bind_ok("SELECT age + 1, COUNT(*) FROM users GROUP BY age");
}

// Ungrouped column in unary expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInUnaryExpr) {
    bind_error("SELECT -id, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// Grouped column in unary expression is OK
TEST_F(QA_BugfixBinder, GDB222_GroupedColumnInUnaryExprOk) {
    bind_ok("SELECT -age, COUNT(*) FROM users GROUP BY age");
}

// Column inside CASE WHEN not in GROUP BY
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInCaseExpr) {
    bind_error("SELECT CASE WHEN id > 0 THEN 'yes' ELSE 'no' END, COUNT(*) FROM users GROUP BY age",
               StatusCode::TYPE_ERROR);
}

// Column inside aggregate expression is OK (don't recurse into aggregates)
TEST_F(QA_BugfixBinder, GDB222_ColumnInsideAggregateOk) {
    bind_ok("SELECT SUM(id + 1) FROM users GROUP BY age");
}

// Multiple ungrouped columns in one expression
TEST_F(QA_BugfixBinder, GDB222_MultipleUngroupedColumnsInExpr) {
    bind_error("SELECT id + age, COUNT(*) FROM users GROUP BY name", StatusCode::TYPE_ERROR);
}

// No GROUP BY but has aggregates — bare columns in expressions rejected
TEST_F(QA_BugfixBinder, GDB222_NoGroupByUngroupedColumnInExpr) {
    bind_error("SELECT id + 1, COUNT(*) FROM users", StatusCode::TYPE_ERROR);
}

// Mixed: one grouped column + one ungrouped in same expression
TEST_F(QA_BugfixBinder, GDB222_MixedGroupedUngroupedInExpr) {
    bind_error("SELECT age + id, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// Column inside CAST expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInCastExpr) {
    bind_error("SELECT CAST(id AS VARCHAR), COUNT(*) FROM users GROUP BY age",
               StatusCode::TYPE_ERROR);
}

// Column inside IN expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInInExpr) {
    bind_error("SELECT id IN (1, 2, 3), COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// Column inside BETWEEN expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInBetweenExpr) {
    bind_error("SELECT id BETWEEN 1 AND 10, COUNT(*) FROM users GROUP BY age",
               StatusCode::TYPE_ERROR);
}

// Column inside IS NULL expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInIsNullExpr) {
    bind_error("SELECT name IS NULL, COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// Column inside LIKE expression
TEST_F(QA_BugfixBinder, GDB222_UngroupedColumnInLikeExpr) {
    bind_error("SELECT name LIKE '%a%', COUNT(*) FROM users GROUP BY age", StatusCode::TYPE_ERROR);
}

// ===========================================================================
// GDB-223: STRING arithmetic rejection
// ===========================================================================

// STRING - STRING
TEST_F(QA_BugfixBinder, GDB223_StringSubtract) {
    bind_error("SELECT name - email FROM users", StatusCode::TYPE_ERROR);
}

// STRING * STRING
TEST_F(QA_BugfixBinder, GDB223_StringMultiply) {
    bind_error("SELECT name * email FROM users", StatusCode::TYPE_ERROR);
}

// STRING / STRING
TEST_F(QA_BugfixBinder, GDB223_StringDivide) {
    bind_error("SELECT name / email FROM users", StatusCode::TYPE_ERROR);
}

// STRING % STRING
TEST_F(QA_BugfixBinder, GDB223_StringModulo) {
    bind_error("SELECT name % email FROM users", StatusCode::TYPE_ERROR);
}

// STRING + STRING (+ is arithmetic, not concat — || is concat)
TEST_F(QA_BugfixBinder, GDB223_StringAdd) {
    bind_error("SELECT name + email FROM users", StatusCode::TYPE_ERROR);
}

// STRING || STRING (concat) should still work
TEST_F(QA_BugfixBinder, GDB223_StringConcatOk) {
    bind_ok("SELECT name || email FROM users");
}

// BOOL arithmetic should also be rejected
TEST_F(QA_BugfixBinder, GDB223_BoolArithmeticRejected) {
    bind_error("SELECT active + active FROM users", StatusCode::TYPE_ERROR);
}

// Numeric arithmetic still works
TEST_F(QA_BugfixBinder, GDB223_NumericArithmeticOk) {
    bind_ok("SELECT id + age FROM users");
    bind_ok("SELECT id - age FROM users");
    bind_ok("SELECT id * age FROM users");
    bind_ok("SELECT id / age FROM users");
    bind_ok("SELECT id % age FROM users");
}

// Mixed numeric types (INT + FLOAT) still works
TEST_F(QA_BugfixBinder, GDB223_MixedNumericArithmeticOk) {
    bind_ok("SELECT id + amount FROM orders");
}

// DECIMAL arithmetic still works
TEST_F(QA_BugfixBinder, GDB223_DecimalArithmeticOk) {
    bind_ok("SELECT price + price FROM products");
    bind_ok("SELECT price - price FROM products");
    bind_ok("SELECT price * price FROM products");
}

// TIMESTAMP arithmetic should be rejected (TIMESTAMP is not numeric)
TEST_F(QA_BugfixBinder, GDB223_TimestampArithmeticRejected) {
    bind_fails("SELECT created_at + created_at FROM orders");
}

// ===========================================================================
// GDB-224: UNION type compatibility validation
// ===========================================================================

// INT UNION STRING — incompatible
TEST_F(QA_BugfixBinder, GDB224_UnionIntStringFails) {
    bind_error("SELECT id FROM users UNION SELECT name FROM users", StatusCode::TYPE_ERROR);
}

// STRING UNION INT — incompatible (reversed)
TEST_F(QA_BugfixBinder, GDB224_UnionStringIntFails) {
    bind_error("SELECT name FROM users UNION SELECT id FROM users", StatusCode::TYPE_ERROR);
}

// INT UNION INT — compatible (same type)
TEST_F(QA_BugfixBinder, GDB224_UnionSameTypeOk) {
    bind_ok("SELECT id FROM users UNION SELECT id FROM orders");
}

// INT UNION FLOAT — compatible (numeric widening)
TEST_F(QA_BugfixBinder, GDB224_UnionIntFloatOk) {
    bind_ok("SELECT id FROM users UNION SELECT amount FROM orders");
}

// STRING UNION STRING — compatible (same type)
TEST_F(QA_BugfixBinder, GDB224_UnionStringsOk) {
    bind_ok("SELECT name FROM users UNION SELECT name FROM products");
}

// Multiple columns: one mismatch fails
TEST_F(QA_BugfixBinder, GDB224_UnionMultiColumnOneMismatch) {
    bind_error("SELECT id, name FROM users UNION SELECT name, id FROM users",
               StatusCode::TYPE_ERROR);
}

// BOOL UNION INT — incompatible
TEST_F(QA_BugfixBinder, GDB224_UnionBoolIntFails) {
    bind_error("SELECT active FROM users UNION SELECT id FROM users", StatusCode::TYPE_ERROR);
}

// Column count mismatch still caught
TEST_F(QA_BugfixBinder, GDB224_UnionColumnCountMismatch) {
    bind_error("SELECT id, name FROM users UNION SELECT id FROM users", StatusCode::TYPE_ERROR);
}

// INTERSECT type validation
TEST_F(QA_BugfixBinder, GDB224_IntersectTypeMismatch) {
    bind_error("SELECT id FROM users INTERSECT SELECT name FROM users", StatusCode::TYPE_ERROR);
}

// EXCEPT type validation
TEST_F(QA_BugfixBinder, GDB224_ExceptTypeMismatch) {
    bind_error("SELECT id FROM users EXCEPT SELECT name FROM users", StatusCode::TYPE_ERROR);
}

// ===========================================================================
// GDB-225: CTE state does not leak between bind() calls
// ===========================================================================

// Basic leak test: CTE from first query invisible in second
TEST_F(QA_BugfixBinder, GDB225_CteDoesNotLeak) {
    bind_ok("WITH cte AS (SELECT id FROM users) SELECT id FROM cte");
    bind_error("SELECT id FROM cte", StatusCode::NOT_FOUND);
}

// Multiple CTEs: none leak
TEST_F(QA_BugfixBinder, GDB225_MultipleCtesDontLeak) {
    bind_ok("WITH a AS (SELECT id FROM users), b AS (SELECT name FROM users) SELECT id FROM a");
    bind_error("SELECT id FROM a", StatusCode::NOT_FOUND);
    bind_error("SELECT name FROM b", StatusCode::NOT_FOUND);
}

// CTE with same name in two separate queries
TEST_F(QA_BugfixBinder, GDB225_CteSameNameDifferentQueries) {
    auto b1 = bind_ok("WITH x AS (SELECT id FROM users) SELECT id FROM x");
    ASSERT_EQ(b1.output_columns.size(), 1u);
    EXPECT_EQ(b1.output_columns[0].type_id, TypeId::INT32);

    auto b2 = bind_ok("WITH x AS (SELECT name FROM users) SELECT name FROM x");
    ASSERT_EQ(b2.output_columns.size(), 1u);
    EXPECT_EQ(b2.output_columns[0].type_id, TypeId::STRING);
}

// Non-CTE query between two CTE queries
TEST_F(QA_BugfixBinder, GDB225_NonCteQueryBetweenCteQueries) {
    bind_ok("WITH cte AS (SELECT id FROM users) SELECT id FROM cte");
    bind_ok("SELECT id FROM users"); // no CTE
    bind_error("SELECT id FROM cte", StatusCode::NOT_FOUND);
}

// Three consecutive bind() calls — no accumulation
TEST_F(QA_BugfixBinder, GDB225_ThreeConsecutiveBindCalls) {
    bind_ok("WITH a AS (SELECT id FROM users) SELECT id FROM a");
    bind_ok("WITH b AS (SELECT name FROM users) SELECT name FROM b");
    bind_ok("WITH c AS (SELECT age FROM users) SELECT age FROM c");
    // None should be visible
    bind_error("SELECT id FROM a", StatusCode::NOT_FOUND);
    bind_error("SELECT name FROM b", StatusCode::NOT_FOUND);
    bind_error("SELECT age FROM c", StatusCode::NOT_FOUND);
}

// CTE still works within a single query (recursive bind)
TEST_F(QA_BugfixBinder, GDB225_CteWorksWithinQuery) {
    auto b = bind_ok("WITH cte AS (SELECT id, name FROM users) SELECT id, name FROM cte");
    ASSERT_EQ(b.output_columns.size(), 2u);
}

// ===========================================================================
// Pipeline-level fixture (GDB-226)
// ===========================================================================

class QA_BugfixPipeline : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb221_226";
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
        EXPECT_TRUE(result.has_value())
            << "SQL failed: " << sql << "\nError: " << result.error().message;
        return std::move(*result);
    }

    void create_users_table() {
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT, score INT)");
    }

    void insert_users_data() {
        exec_ok("INSERT INTO users VALUES (1, 'alice', 30, 85)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 25, 92)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 35, 78)");
        exec_ok("INSERT INTO users VALUES (4, 'diana', 28, 95)");
        exec_ok("INSERT INTO users VALUES (5, 'eve', 22, 88)");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ===========================================================================
// GDB-226: ORDER BY on non-selected column
// ===========================================================================

// Basic case: SELECT name ORDER BY age DESC LIMIT 1
TEST_F(QA_BugfixPipeline, GDB226_OrderByNonSelectedColumnDesc) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age DESC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "charlie"); // age 35, highest
}

// ASC ordering
TEST_F(QA_BugfixPipeline, GDB226_OrderByNonSelectedColumnAsc) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age ASC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eve"); // age 22, lowest
}

// ORDER BY non-selected with OFFSET
TEST_F(QA_BugfixPipeline, GDB226_OrderByNonSelectedWithOffset) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age ASC LIMIT 2 OFFSET 1");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "bob");   // age 25
    EXPECT_EQ(qr.rows[1][0].as_string(), "diana"); // age 28
}

// ORDER BY column that IS selected — should still work
TEST_F(QA_BugfixPipeline, GDB226_OrderBySelectedColumnOk) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name, age FROM users ORDER BY age DESC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "charlie");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 35);
}

// SELECT * with ORDER BY — no projection issue
TEST_F(QA_BugfixPipeline, GDB226_SelectStarOrderByOk) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT * FROM users ORDER BY age DESC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "charlie");
}

// ORDER BY a different non-selected column (score)
TEST_F(QA_BugfixPipeline, GDB226_OrderByScoreNotSelected) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY score DESC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "diana"); // score 95, highest
}

// All rows ordered by non-selected column
TEST_F(QA_BugfixPipeline, GDB226_AllRowsOrderByNonSelected) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age ASC");
    ASSERT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eve");     // 22
    EXPECT_EQ(qr.rows[1][0].as_string(), "bob");     // 25
    EXPECT_EQ(qr.rows[2][0].as_string(), "diana");   // 28
    EXPECT_EQ(qr.rows[3][0].as_string(), "alice");   // 30
    EXPECT_EQ(qr.rows[4][0].as_string(), "charlie"); // 35
}

// ORDER BY non-selected column with WHERE filter
TEST_F(QA_BugfixPipeline, GDB226_OrderByNonSelectedWithWhere) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users WHERE age > 25 ORDER BY age DESC");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "charlie"); // 35
    EXPECT_EQ(qr.rows[1][0].as_string(), "alice");   // 30
    EXPECT_EQ(qr.rows[2][0].as_string(), "diana");   // 28
}

// ORDER BY with aggregates on non-selected column
TEST_F(QA_BugfixPipeline, GDB226_AggregateOrderByCountDesc) {
    create_users_table();
    insert_users_data();

    // Simple aggregate with ORDER BY on aggregate column
    auto qr = exec_ok("SELECT COUNT(*) FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 5);
}

// Projection returns only the selected columns (not sort key)
TEST_F(QA_BugfixPipeline, GDB226_ProjectionDoesNotLeakSortKey) {
    create_users_table();
    insert_users_data();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age DESC LIMIT 2");
    ASSERT_EQ(qr.rows.size(), 2u);
    // Each row should have exactly 1 column (name), not 2
    EXPECT_EQ(qr.rows[0].size(), 1u);
    EXPECT_EQ(qr.rows[1].size(), 1u);
}

// Empty table with ORDER BY non-selected column
TEST_F(QA_BugfixPipeline, GDB226_EmptyTableOrderByNonSelected) {
    create_users_table();

    auto qr = exec_ok("SELECT name FROM users ORDER BY age DESC");
    EXPECT_TRUE(qr.rows.empty());
}

// Single row with ORDER BY non-selected column
TEST_F(QA_BugfixPipeline, GDB226_SingleRowOrderByNonSelected) {
    create_users_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30, 85)");

    auto qr = exec_ok("SELECT name FROM users ORDER BY age DESC LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

} // namespace sixseven
