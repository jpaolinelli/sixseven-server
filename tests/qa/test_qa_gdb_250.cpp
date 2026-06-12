#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Expr evaluator helpers (unit-level adversarial tests)
// =============================================================================

namespace {

ExprPtr fn_call(const std::string& name) {
    auto e = std::make_unique<FunctionCallExpr>();
    e->name = name;
    return e;
}

static BoundStatement empty_bound() {
    return BoundStatement{};
}

} // namespace

// =============================================================================
// QueryEngine test fixture (end-to-end adversarial tests)
// =============================================================================

class QA_GDB250 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb250";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed for: " << sql
                          << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error but got success for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// UUID generation adversarial tests (unit level)
// =============================================================================

TEST(QA_GenUuid, ManyUuidsAreAllUnique) {
    // Stress: generate 100 UUIDs and verify all are distinct.
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    std::set<Uuid> seen;
    for (int i = 0; i < 100; ++i) {
        auto result = evaluate_expr(*fn_call("gen_uuid"), tuple, schema, bound);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->type_id(), TypeId::UUID);
        auto [it, inserted] = seen.insert(result->as_uuid());
        EXPECT_TRUE(inserted) << "duplicate UUID at iteration " << i;
    }
    EXPECT_EQ(seen.size(), 100u);
}

TEST(QA_GenUuid, AllUuidsHaveCorrectVersionAndVariant) {
    // Verify that every generated UUID has v4 version and variant-1 bits.
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    for (int i = 0; i < 50; ++i) {
        auto result = evaluate_expr(*fn_call("gen_uuid"), tuple, schema, bound);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        const auto& uuid = result->as_uuid();
        // Version 4: byte 6 high nibble == 0x4.
        EXPECT_EQ((uuid[6] >> 4) & 0x0F, 4) << "version mismatch at iteration " << i;
        // Variant 1: byte 8 high 2 bits == 10.
        EXPECT_EQ((uuid[8] >> 6) & 0x03, 2) << "variant mismatch at iteration " << i;
    }
}

// =============================================================================
// Temporal function adversarial tests (unit level)
// =============================================================================

TEST(QA_Temporal, CurrentTimestampIsMonotonic) {
    // Two consecutive calls should produce non-decreasing timestamps.
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto r1 = evaluate_expr(*fn_call("now"), tuple, schema, bound);
    auto r2 = evaluate_expr(*fn_call("now"), tuple, schema, bound);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_LE(r1->as_timestamp().microseconds, r2->as_timestamp().microseconds);
}

TEST(QA_Temporal, CurrentDateInReasonableRange) {
    // The date should be after 2020-01-01 and before 2100-01-01.
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("CURRENT_DATE"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    int32_t days = result->as_date().days_since_epoch;
    // 2020-01-01 is about 18262 days since epoch (1970-01-01).
    EXPECT_GT(days, 18262);
    // 2100-01-01 is about 47482 days since epoch.
    EXPECT_LT(days, 47482);
}

TEST(QA_Temporal, CurrentTimeInValidRange) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("CURRENT_TIME"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto us = result->as_time().microseconds;
    EXPECT_GE(us, 0);
    EXPECT_LT(us, 86400000000LL); // < 24 hours in microseconds
}

TEST(QA_Temporal, NowAndCurrentTimestampAreEquivalent) {
    // Both should return TIMESTAMP type.
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto r1 = evaluate_expr(*fn_call("now"), tuple, schema, bound);
    auto r2 = evaluate_expr(*fn_call("CURRENT_TIMESTAMP"), tuple, schema, bound);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    EXPECT_EQ(r1->type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(r2->type_id(), TypeId::TIMESTAMP);

    // Should be within 1 second of each other.
    auto diff = std::abs(r1->as_timestamp().microseconds - r2->as_timestamp().microseconds);
    EXPECT_LT(diff, 1000000);
}

// =============================================================================
// E2E: Multi-row single INSERT with defaults
// =============================================================================

TEST_F(QA_GDB250, MultiRowInsertWithDefaultsInSingleStatement) {
    // Insert multiple rows in one statement, each should get unique defaults.
    exec_ok("CREATE TABLE events ("
            "  id UUID DEFAULT gen_uuid(),"
            "  name TEXT NOT NULL,"
            "  ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
            ")");

    exec_ok("INSERT INTO events (name) VALUES ('event1'), ('event2'), ('event3')");

    auto qr = exec_ok("SELECT id, name, ts FROM events ORDER BY name");
    ASSERT_EQ(qr.rows.size(), 3u);

    // All IDs should be valid UUIDs and all different.
    std::set<Uuid> ids;
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(qr.rows[i][0].is_null()) << "row " << i << " id is null";
        EXPECT_EQ(qr.rows[i][0].type_id(), TypeId::UUID) << "row " << i;
        ids.insert(qr.rows[i][0].as_uuid());

        // Timestamps should all be non-null.
        EXPECT_FALSE(qr.rows[i][2].is_null()) << "row " << i << " ts is null";
        EXPECT_EQ(qr.rows[i][2].type_id(), TypeId::TIMESTAMP) << "row " << i;
    }
    EXPECT_EQ(ids.size(), 3u) << "UUIDs should be unique across rows";
}

// =============================================================================
// E2E: DEFAULT CURRENT_DATE and CURRENT_TIME
// =============================================================================

TEST_F(QA_GDB250, InsertWithDefaultCurrentDate) {
    exec_ok("CREATE TABLE daily_log ("
            "  id INT,"
            "  log_date DATE DEFAULT CURRENT_DATE,"
            "  msg TEXT"
            ")");

    auto before_days = std::chrono::duration_cast<std::chrono::days>(
                           std::chrono::floor<std::chrono::days>(
                               std::chrono::system_clock::now().time_since_epoch()))
                           .count();

    exec_ok("INSERT INTO daily_log (id, msg) VALUES (1, 'hello')");

    auto qr = exec_ok("SELECT id, log_date, msg FROM daily_log");
    ASSERT_EQ(qr.rows.size(), 1u);

    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].type_id(), TypeId::DATE);

    auto days = qr.rows[0][1].as_date().days_since_epoch;
    auto diff = std::abs(static_cast<int64_t>(days) - static_cast<int64_t>(before_days));
    EXPECT_LE(diff, 1) << "date should be today";
}

TEST_F(QA_GDB250, InsertWithDefaultCurrentTime) {
    exec_ok("CREATE TABLE timed_log ("
            "  id INT,"
            "  log_time TIME DEFAULT CURRENT_TIME,"
            "  msg TEXT"
            ")");

    exec_ok("INSERT INTO timed_log (id, msg) VALUES (1, 'hello')");

    auto qr = exec_ok("SELECT id, log_time, msg FROM timed_log");
    ASSERT_EQ(qr.rows.size(), 1u);

    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].type_id(), TypeId::TIME);

    auto us = qr.rows[0][1].as_time().microseconds;
    EXPECT_GE(us, 0);
    EXPECT_LT(us, 86400000000LL);
}

// =============================================================================
// E2E: DEFAULT with arithmetic expression
// =============================================================================

TEST_F(QA_GDB250, InsertWithDefaultArithmeticExpression) {
    exec_ok("CREATE TABLE with_calc ("
            "  id INT,"
            "  score INT DEFAULT 10 + 20"
            ")");

    exec_ok("INSERT INTO with_calc (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, score FROM with_calc");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 30);
}

TEST_F(QA_GDB250, InsertWithDefaultNegativeNumber) {
    exec_ok("CREATE TABLE neg_default ("
            "  id INT,"
            "  val INT DEFAULT -1"
            ")");

    exec_ok("INSERT INTO neg_default (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, val FROM neg_default");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), -1);
}

// =============================================================================
// E2E: All columns have defaults — INSERT with empty explicit column list
// =============================================================================

TEST_F(QA_GDB250, InsertAllDefaultColumns) {
    // Table where every column has a default.
    exec_ok("CREATE TABLE all_defaults ("
            "  id UUID DEFAULT gen_uuid(),"
            "  ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
            "  val INT DEFAULT 42"
            ")");

    // This is tricky: how do you INSERT with no provided values?
    // SQL standard: INSERT INTO t DEFAULT VALUES
    // Our system might not support that syntax, so we test with an empty column list.
    // Actually, INSERT INTO t (val) VALUES (99) omitting id and ts should work.
    exec_ok("INSERT INTO all_defaults (val) VALUES (99)");

    auto qr = exec_ok("SELECT id, ts, val FROM all_defaults");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(qr.rows[0][2].as_int32(), 99);
}

// =============================================================================
// E2E: Multiple inserts accumulate correctly
// =============================================================================

TEST_F(QA_GDB250, ManyInsertsWithDefaultsStress) {
    exec_ok("CREATE TABLE stress ("
            "  id UUID DEFAULT gen_uuid(),"
            "  seq INT"
            ")");

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        exec_ok("INSERT INTO stress (seq) VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT id, seq FROM stress ORDER BY seq");
    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(N));

    std::set<Uuid> uuids;
    for (size_t i = 0; i < qr.rows.size(); ++i) {
        EXPECT_FALSE(qr.rows[i][0].is_null()) << "row " << i;
        EXPECT_EQ(qr.rows[i][0].type_id(), TypeId::UUID) << "row " << i;
        uuids.insert(qr.rows[i][0].as_uuid());
        EXPECT_EQ(qr.rows[i][1].as_int32(), static_cast<int32_t>(i)) << "row " << i;
    }
    EXPECT_EQ(uuids.size(), static_cast<size_t>(N)) << "all UUIDs should be unique";
}

// =============================================================================
// E2E: Mix of columns with and without defaults, nullable and non-nullable
// =============================================================================

TEST_F(QA_GDB250, ComplexMixOfDefaultsAndNulls) {
    exec_ok("CREATE TABLE complex ("
            "  id UUID DEFAULT gen_uuid(),"
            "  name TEXT NOT NULL,"
            "  bio TEXT," // nullable, no default -> NULL
            "  score INT DEFAULT 0,"
            "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
            ")");

    // Provide only name — id, score, created_at get defaults, bio gets NULL.
    exec_ok("INSERT INTO complex (name) VALUES ('Alice')");

    auto qr = exec_ok("SELECT id, name, bio, score, created_at FROM complex");
    ASSERT_EQ(qr.rows.size(), 1u);

    EXPECT_FALSE(qr.rows[0][0].is_null()); // id: UUID default
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Alice");
    EXPECT_TRUE(qr.rows[0][2].is_null());   // bio: nullable, no default
    EXPECT_EQ(qr.rows[0][3].as_int32(), 0); // score: default 0
    EXPECT_FALSE(qr.rows[0][4].is_null());  // created_at: CURRENT_TIMESTAMP default
    EXPECT_EQ(qr.rows[0][4].type_id(), TypeId::TIMESTAMP);
}

// =============================================================================
// E2E: Override defaults — explicit values take precedence
// =============================================================================

TEST_F(QA_GDB250, ExplicitValueOverridesDefault) {
    exec_ok("CREATE TABLE override_test ("
            "  id INT,"
            "  score INT DEFAULT 100,"
            "  level INT DEFAULT 1"
            ")");

    // Provide all values — none should use defaults.
    exec_ok("INSERT INTO override_test VALUES (1, 999, 50)");

    auto qr = exec_ok("SELECT id, score, level FROM override_test");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 999);
    EXPECT_EQ(qr.rows[0][2].as_int32(), 50);
}

TEST_F(QA_GDB250, PartialOverrideOfDefaults) {
    exec_ok("CREATE TABLE partial_override ("
            "  id INT,"
            "  a INT DEFAULT 10,"
            "  b INT DEFAULT 20,"
            "  c INT DEFAULT 30"
            ")");

    // Provide id and b; a and c should use defaults.
    exec_ok("INSERT INTO partial_override (id, b) VALUES (1, 99)");

    auto qr = exec_ok("SELECT id, a, b, c FROM partial_override");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 10); // default
    EXPECT_EQ(qr.rows[0][2].as_int32(), 99); // provided
    EXPECT_EQ(qr.rows[0][3].as_int32(), 30); // default
}

// =============================================================================
// E2E: Error cases
// =============================================================================

TEST_F(QA_GDB250, ErrorOnMissingNonNullableWithNoDefault) {
    exec_ok("CREATE TABLE strict ("
            "  id INT,"
            "  name TEXT NOT NULL,"
            "  email TEXT NOT NULL"
            ")");

    // Omit email — non-nullable, no default → must error.
    exec_error("INSERT INTO strict (id, name) VALUES (1, 'Alice')", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB250, ErrorOnMissingMultipleNonNullableColumns) {
    exec_ok("CREATE TABLE multi_strict ("
            "  id INT,"
            "  a TEXT NOT NULL,"
            "  b TEXT NOT NULL,"
            "  c TEXT NOT NULL"
            ")");

    // Only provide id — all of a, b, c are missing.
    exec_error("INSERT INTO multi_strict (id) VALUES (1)", StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// E2E: String default with single-quote character (BUG candidate)
// =============================================================================

TEST_F(QA_GDB250, StringDefaultWithSingleQuote) {
    // A string default containing a single quote must survive the round-trip
    // through expr_to_sql() → catalog storage → re-parsing.
    // If expr_to_sql doesn't escape single quotes, this will fail.
    exec_ok("CREATE TABLE quoted ("
            "  id INT,"
            "  label TEXT DEFAULT 'it''s a test'"
            ")");

    // GDB-758 fixed: expr_to_sql now escapes single quotes so the round-trip
    // through catalog storage and re-parsing succeeds.
    exec_ok("INSERT INTO quoted (id) VALUES (1)");
    auto qr = exec_ok("SELECT id, label FROM quoted");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "it's a test");
}

// =============================================================================
// E2E: Boolean default
// =============================================================================

TEST_F(QA_GDB250, BooleanDefault) {
    exec_ok("CREATE TABLE with_bool ("
            "  id INT,"
            "  active BOOLEAN DEFAULT true"
            ")");

    exec_ok("INSERT INTO with_bool (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, active FROM with_bool");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_bool(), true);
}

// =============================================================================
// E2E: Float default
// =============================================================================

TEST_F(QA_GDB250, FloatDefault) {
    exec_ok("CREATE TABLE with_float ("
            "  id INT,"
            "  ratio DOUBLE DEFAULT 3.14"
            ")");

    exec_ok("INSERT INTO with_float (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, ratio FROM with_float");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 3.14, 0.001);
}

TEST_F(QA_GDB250, Float32DefaultCoercion) {
    // FLOAT column is FLOAT32, default literal 3.14 evaluates as FLOAT64.
    // fit_to_storage should narrow it to FLOAT32.
    exec_ok("CREATE TABLE with_float32 ("
            "  id INT,"
            "  ratio FLOAT DEFAULT 3.14"
            ")");

    exec_ok("INSERT INTO with_float32 (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, ratio FROM with_float32");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    // After narrowing to FLOAT32 and back, should be close to 3.14.
    EXPECT_NEAR(qr.rows[0][1].as_float32(), 3.14f, 0.01f);
}

// =============================================================================
// E2E: String default (simple, no special chars)
// =============================================================================

TEST_F(QA_GDB250, StringDefault) {
    exec_ok("CREATE TABLE with_string ("
            "  id INT,"
            "  status TEXT DEFAULT 'active'"
            ")");

    exec_ok("INSERT INTO with_string (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, status FROM with_string");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "active");
}

// =============================================================================
// E2E: NULL literal default
// =============================================================================

TEST_F(QA_GDB250, ExplicitNullDefault) {
    exec_ok("CREATE TABLE with_null_default ("
            "  id INT,"
            "  info TEXT DEFAULT NULL"
            ")");

    exec_ok("INSERT INTO with_null_default (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, info FROM with_null_default");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

// =============================================================================
// E2E: NOW() as default (function call form, with parens)
// =============================================================================

TEST_F(QA_GDB250, NowFunctionAsDefault) {
    exec_ok("CREATE TABLE with_now ("
            "  id INT,"
            "  ts TIMESTAMP DEFAULT NOW()"
            ")");

    auto before = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    exec_ok("INSERT INTO with_now (id) VALUES (1)");

    auto after = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();

    auto qr = exec_ok("SELECT id, ts FROM with_now");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].type_id(), TypeId::TIMESTAMP);

    auto ts = qr.rows[0][1].as_timestamp().microseconds;
    EXPECT_GE(ts, before);
    EXPECT_LE(ts, after);
}

// =============================================================================
// E2E: Insert then SELECT with WHERE on defaulted columns
// =============================================================================

TEST_F(QA_GDB250, SelectWhereOnDefaultedIntColumn) {
    exec_ok("CREATE TABLE scored ("
            "  id INT,"
            "  score INT DEFAULT 0"
            ")");

    exec_ok("INSERT INTO scored (id) VALUES (1)");
    exec_ok("INSERT INTO scored (id, score) VALUES (2, 50)");

    auto qr = exec_ok("SELECT id, score FROM scored WHERE score = 0");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 0);
}

// =============================================================================
// E2E: UPDATE after INSERT with defaults
// =============================================================================

TEST_F(QA_GDB250, UpdateRowInsertedWithDefaults) {
    exec_ok("CREATE TABLE updatable ("
            "  id INT,"
            "  name TEXT NOT NULL,"
            "  status TEXT DEFAULT 'pending'"
            ")");

    exec_ok("INSERT INTO updatable (id, name) VALUES (1, 'task1')");

    // Verify default was applied.
    auto qr1 = exec_ok("SELECT status FROM updatable WHERE id = 1");
    ASSERT_EQ(qr1.rows.size(), 1u);
    EXPECT_EQ(qr1.rows[0][0].as_string(), "pending");

    // Update the defaulted column.
    exec_ok("UPDATE updatable SET status = 'done' WHERE id = 1");

    auto qr2 = exec_ok("SELECT status FROM updatable WHERE id = 1");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_string(), "done");
}

// =============================================================================
// E2E: DELETE after INSERT with defaults
// =============================================================================

TEST_F(QA_GDB250, DeleteRowInsertedWithDefaults) {
    exec_ok("CREATE TABLE deletable ("
            "  id UUID DEFAULT gen_uuid(),"
            "  val INT"
            ")");

    exec_ok("INSERT INTO deletable (val) VALUES (1)");
    exec_ok("INSERT INTO deletable (val) VALUES (2)");

    auto qr1 = exec_ok("SELECT val FROM deletable ORDER BY val");
    ASSERT_EQ(qr1.rows.size(), 2u);

    exec_ok("DELETE FROM deletable WHERE val = 1");

    auto qr2 = exec_ok("SELECT val FROM deletable");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_int32(), 2);
}

// =============================================================================
// E2E: Column order doesn't matter in explicit column list
// =============================================================================

TEST_F(QA_GDB250, InsertColumnsInDifferentOrderThanSchema) {
    exec_ok("CREATE TABLE reorder ("
            "  id UUID DEFAULT gen_uuid(),"
            "  name TEXT NOT NULL,"
            "  age INT DEFAULT 0,"
            "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
            ")");

    // Provide columns in reverse order of schema (age, name).
    exec_ok("INSERT INTO reorder (age, name) VALUES (25, 'Bob')");

    auto qr = exec_ok("SELECT id, name, age, created_at FROM reorder");
    ASSERT_EQ(qr.rows.size(), 1u);

    EXPECT_FALSE(qr.rows[0][0].is_null()); // id defaulted
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Bob");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 25);
    EXPECT_FALSE(qr.rows[0][3].is_null()); // created_at defaulted
}

// =============================================================================
// E2E: Multiple tables with defaults are independent
// =============================================================================

TEST_F(QA_GDB250, MultipleTablesWithDefaultsAreIndependent) {
    exec_ok("CREATE TABLE t1 ("
            "  id UUID DEFAULT gen_uuid(),"
            "  val INT DEFAULT 10"
            ")");
    exec_ok("CREATE TABLE t2 ("
            "  id UUID DEFAULT gen_uuid(),"
            "  val INT DEFAULT 20"
            ")");

    exec_ok("INSERT INTO t1 (val) VALUES (1)");
    exec_ok("INSERT INTO t2 (val) VALUES (2)");

    // Defaults should not leak between tables. Specifically check that
    // t1's default val (10) doesn't show up when val is explicitly provided.
    auto qr1 = exec_ok("SELECT id, val FROM t1");
    auto qr2 = exec_ok("SELECT id, val FROM t2");

    ASSERT_EQ(qr1.rows.size(), 1u);
    ASSERT_EQ(qr2.rows.size(), 1u);

    EXPECT_EQ(qr1.rows[0][1].as_int32(), 1); // explicit
    EXPECT_EQ(qr2.rows[0][1].as_int32(), 2); // explicit

    // Now insert using defaults.
    exec_ok("INSERT INTO t1 (id) VALUES (gen_uuid())");
    exec_ok("INSERT INTO t2 (id) VALUES (gen_uuid())");

    auto qr1b = exec_ok("SELECT val FROM t1 ORDER BY val");
    auto qr2b = exec_ok("SELECT val FROM t2 ORDER BY val");

    ASSERT_EQ(qr1b.rows.size(), 2u);
    ASSERT_EQ(qr2b.rows.size(), 2u);

    // One row has explicit val, one has default.
    EXPECT_EQ(qr1b.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr1b.rows[1][0].as_int32(), 10); // default for t1
    EXPECT_EQ(qr2b.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr2b.rows[1][0].as_int32(), 20); // default for t2
}

// =============================================================================
// E2E: Zero default for integer
// =============================================================================

TEST_F(QA_GDB250, ZeroIntDefault) {
    exec_ok("CREATE TABLE zero_default ("
            "  id INT,"
            "  count INT DEFAULT 0"
            ")");

    exec_ok("INSERT INTO zero_default (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, count FROM zero_default");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 0);
}

// =============================================================================
// E2E: Empty string default
// =============================================================================

TEST_F(QA_GDB250, EmptyStringDefault) {
    exec_ok("CREATE TABLE empty_str ("
            "  id INT,"
            "  tag TEXT DEFAULT ''"
            ")");

    exec_ok("INSERT INTO empty_str (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, tag FROM empty_str");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "");
    EXPECT_FALSE(qr.rows[0][1].is_null()); // empty string != null
}
