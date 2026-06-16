/// @file test_qa_gdb_661.cpp
/// QA adversarial tests for GDB-661: SELECT-without-FROM unary expressions.
///
/// Targets the constant-folding fast path added to QueryEngine::execute that
/// folds LiteralExpr and UnaryExpr(NEGATE|NOT) for INT32/INT64/FLOAT64/BOOL.
///
/// Probes:
///   - INT_MIN negation (signed-overflow UB potential).
///   - `-NULL` and `NOT NULL` type / null behavior.
///   - Double / triple negation chains.
///   - Mixed literal + unary expressions in one SELECT list.
///   - Large literals that exceed INT32 (parser/std::stoi rejection path).
///   - Float infinity / NaN-ish round-trip via `-0.0` and very large floats.
///   - Default column-name derivation ("-5", "NOT TRUE", "-NULL").
///   - NOT applied to non-bool literals (must NOT be folded into wrong type).
///   - Empty / whitespace-only string literal as operand of unary minus.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace sixseven {
namespace {

class QA_GDB661 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb661";
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// -----------------------------------------------------------------------------
// Baseline: ticket repro
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, BasicNegativeIntegerNoLongerErrors) {
    auto r = engine_->execute("SELECT -5 AS neg");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
    EXPECT_EQ(r->column_types[0], TypeId::INT32);
    EXPECT_EQ(r->rows[0][0].as_int32(), -5);
}

// -----------------------------------------------------------------------------
// INT_MIN negation — guards against signed-overflow UB.
// -----------------------------------------------------------------------------
//
// `SELECT -2147483648` parses as UnaryExpr(NEGATE, LiteralExpr("2147483648")).
// The literal "2147483648" overflows int32 in std::stoi → throws → fold returns
// nullopt → fast path falls through (we expect a clean error, NOT UB / crash).
TEST_F(QA_GDB661, IntMinViaOverflowingPositiveLiteralErrorsCleanly) {
    auto r = engine_->execute("SELECT -2147483648 AS x");
    // Either rejected (preferred) or evaluated by planner; must not crash.
    if (r.has_value()) {
        ASSERT_EQ(r->rows.size(), 1u);
    } else {
        // Planner currently returns "SELECT without FROM is not yet supported"
        // or PARSE_ERROR; either is acceptable — just no UB.
        SUCCEED();
    }
}

TEST_F(QA_GDB661, IntMaxLiteralFoldsCleanly) {
    auto r = engine_->execute("SELECT -2147483647 AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows[0][0].as_int32(), -2147483647);
}

// Folding INT32_MIN itself by negating: `-(-2147483648)` is UB if computed in
// int32. The current fold path negates only after std::stoi succeeds; since
// "2147483648" overflows std::stoi, the inner fold returns nullopt, the outer
// NEGATE never runs, and the whole expression falls through to the planner
// which returns an error.  We must NOT get back a silently-wrong constant
// (e.g. 0 or INT32_MIN from wrapping).
TEST_F(QA_GDB661, DoubleNegateIntMinDoesNotCrash) {
    auto r = engine_->execute("SELECT - -2147483648 AS x");
    // Fold cannot handle the inner literal (stoi overflow) → planner error.
    // If a future change lets this succeed it MUST return 2147483648 as INT64
    // (or be rejected), never an INT32 wrap-around value.
    if (r.has_value()) {
        // Succeeding is only acceptable if the value is the mathematically
        // correct result: - (-2147483648) = +2147483648, which requires INT64.
        ASSERT_EQ(r->rows.size(), 1u);
        EXPECT_NE(r->column_types[0], TypeId::INT32)
            << "INT32 cannot hold +2147483648; must not silently wrap";
        EXPECT_EQ(r->rows[0][0].as_int64(), static_cast<int64_t>(2147483648LL));
    } else {
        // Currently expected: planner rejects the unfolded expression.
        EXPECT_FALSE(r.has_value());
    }
}

// -----------------------------------------------------------------------------
// Overflow literals — parser rejects, fast path must fall through cleanly.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, LargePositiveOverflowsInt32) {
    auto r = engine_->execute("SELECT 9999999999 AS x");
    // std::stoi throws → fold returns nullopt → planner is invoked → error.
    EXPECT_FALSE(r.has_value());
}

TEST_F(QA_GDB661, LargeNegativeOverflowsInt32) {
    auto r = engine_->execute("SELECT -9999999999 AS x");
    EXPECT_FALSE(r.has_value());
}

// -----------------------------------------------------------------------------
// `-NULL` / `NOT NULL` — null propagation through unary ops.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, NegateNullProducesNull) {
    auto r = engine_->execute("SELECT -NULL AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
    EXPECT_TRUE(r->rows[0][0].is_null());
}

TEST_F(QA_GDB661, NegateNullDefaultName) {
    auto r = engine_->execute("SELECT -NULL");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->column_names.size(), 1u);
    // Default name is built as "-" + inner_default_name. Inner is "NULL".
    EXPECT_EQ(r->column_names[0], "-NULL");
}

TEST_F(QA_GDB661, NotNullProducesNullBool) {
    auto r = engine_->execute("SELECT NOT NULL AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
    EXPECT_TRUE(r->rows[0][0].is_null());
    EXPECT_EQ(r->column_types[0], TypeId::BOOL);
}

// -----------------------------------------------------------------------------
// Multiple negations.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, TripleNegateInteger) {
    auto r = engine_->execute("SELECT - - -7 AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows[0][0].as_int32(), -7);
}

TEST_F(QA_GDB661, QuadrupleNegateInteger) {
    auto r = engine_->execute("SELECT - - - -7 AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows[0][0].as_int32(), 7);
}

TEST_F(QA_GDB661, DoubleNotBoolean) {
    auto r = engine_->execute("SELECT NOT NOT TRUE AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->rows[0][0].as_bool());
}

TEST_F(QA_GDB661, TripleNotBoolean) {
    auto r = engine_->execute("SELECT NOT NOT NOT FALSE AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->rows[0][0].as_bool());
}

// -----------------------------------------------------------------------------
// NOT applied to non-bool — must NOT be silently folded.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, NotIntegerNotFolded) {
    // NOT 1: try_fold_const_expr sees NOT on an INT32 inner result and returns
    // nullopt (line: `if (inner->type_id != TypeId::BOOL) return std::nullopt`).
    // The expression must NOT be silently folded to a bool constant (e.g. false
    // because 1 is truthy in C++) — that would be a type-coercion bug.
    // The planner currently returns an error for SELECT-without-FROM on a
    // non-constant select list; either outcome is correct, but a successful
    // result MUST NOT carry a wrong constant value.
    auto r = engine_->execute("SELECT NOT 1 AS x");
    EXPECT_FALSE(r.has_value())
        << "NOT applied to an integer literal must not be silently folded to a "
           "bool constant; expected a planner/type error";
}

TEST_F(QA_GDB661, NotStringNotFolded) {
    // NOT 'hello': try_fold_const_expr sees NOT on a STRING inner result and
    // returns nullopt.  The fold must NOT coerce the string to bool (e.g. by
    // treating any non-empty string as truthy) and silently produce `false`.
    // The correct outcome is a hard error.
    auto r = engine_->execute("SELECT NOT 'hello' AS x");
    EXPECT_FALSE(r.has_value())
        << "NOT applied to a string literal must not be silently folded; "
           "expected a type/planner error";
}

// -----------------------------------------------------------------------------
// Negate non-numeric — must NOT be silently folded.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, NegateStringNotFolded) {
    auto r = engine_->execute("SELECT -'hello' AS x");
    EXPECT_FALSE(r.has_value());
}

TEST_F(QA_GDB661, NegateBoolNotFolded) {
    // -TRUE: try_fold_const_expr hits the NEGATE default branch which returns
    // nullopt for non-numeric types (BOOL is not INT32/INT64/FLOAT64).
    // The fold must NOT silently cast TRUE to 1 and produce -1, which would be
    // a type-coercion bug masquerading as a correct result.
    // The correct outcome is a hard error from the planner.
    auto r = engine_->execute("SELECT -TRUE AS x");
    EXPECT_FALSE(r.has_value())
        << "Unary minus applied to a bool literal must not be silently folded "
           "to a numeric constant; expected a type/planner error";
}

// -----------------------------------------------------------------------------
// Mixed expressions in select-list.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, MixedAllConstFolded) {
    auto r = engine_->execute("SELECT -1 AS a, 'x' AS b, NOT FALSE AS c, NULL AS d");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
    ASSERT_EQ(r->column_names.size(), 4u);
    EXPECT_EQ(r->rows[0][0].as_int32(), -1);
    EXPECT_EQ(r->rows[0][1].as_string(), "x");
    EXPECT_TRUE(r->rows[0][2].as_bool());
    EXPECT_TRUE(r->rows[0][3].is_null());
}

TEST_F(QA_GDB661, MixedNegativeFloatAndInt) {
    auto r = engine_->execute("SELECT -1 AS a, -2.5 AS b");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->column_types[0], TypeId::INT32);
    EXPECT_EQ(r->column_types[1], TypeId::FLOAT64);
    EXPECT_EQ(r->rows[0][0].as_int32(), -1);
    EXPECT_DOUBLE_EQ(r->rows[0][1].as_float64(), -2.5);
}

// -----------------------------------------------------------------------------
// Float edge cases.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, NegateZeroFloat) {
    auto r = engine_->execute("SELECT -0.0 AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->column_types[0], TypeId::FLOAT64);
    // -0.0 == 0.0 numerically; just verify no crash and value is zero.
    EXPECT_EQ(r->rows[0][0].as_float64(), 0.0);
}

TEST_F(QA_GDB661, VeryLargeNegativeFloat) {
    auto r = engine_->execute("SELECT -1.7976931348623157e308 AS x");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(std::isfinite(r->rows[0][0].as_float64()));
    EXPECT_LT(r->rows[0][0].as_float64(), 0.0);
}

// -----------------------------------------------------------------------------
// Default column name rendering.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, DefaultNameForNegInt) {
    auto r = engine_->execute("SELECT -42");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->column_names[0], "-42");
}

TEST_F(QA_GDB661, DefaultNameForNotTrue) {
    auto r = engine_->execute("SELECT NOT TRUE");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Default is "NOT " + inner.default_name. Inner is "true" or "TRUE"
    // depending on lexer casing. Accept either.
    const auto& n = r->column_names[0];
    EXPECT_TRUE(n == "NOT true" || n == "NOT TRUE")
        << "got default name: " << n;
}

TEST_F(QA_GDB661, DefaultNameForDoubleNeg) {
    auto r = engine_->execute("SELECT - -7");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->column_names[0], "--7");
}

// -----------------------------------------------------------------------------
// Stress: deeply nested unary chain (parser depth).
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, DeeplyNestedNegationDoesNotCrash) {
    std::string sql = "SELECT ";
    for (int i = 0; i < 64; ++i) {
        sql += "- ";
    }
    sql += "1";
    auto r = engine_->execute(sql);
    // Even count → 1, odd count → -1. We have 64 (even).
    if (r.has_value()) {
        EXPECT_EQ(r->rows[0][0].as_int32(), 1);
    } else {
        // Parser depth limit / planner fallback — must not crash.
        SUCCEED();
    }
}

TEST_F(QA_GDB661, DeeplyNestedNotDoesNotCrash) {
    std::string sql = "SELECT ";
    for (int i = 0; i < 32; ++i) {
        sql += "NOT ";
    }
    sql += "TRUE";
    auto r = engine_->execute(sql);
    if (r.has_value()) {
        // 32 NOTs of TRUE → TRUE.
        EXPECT_TRUE(r->rows[0][0].as_bool());
    } else {
        SUCCEED();
    }
}

// -----------------------------------------------------------------------------
// Aliases interact with default names.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB661, AliasOverridesDefaultName) {
    auto r = engine_->execute("SELECT -5 AS my_col");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->column_names[0], "my_col");
}

TEST_F(QA_GDB661, NegateNullWithAlias) {
    auto r = engine_->execute("SELECT -NULL AS n");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->column_names[0], "n");
    EXPECT_TRUE(r->rows[0][0].is_null());
}

} // namespace
} // namespace sixseven
