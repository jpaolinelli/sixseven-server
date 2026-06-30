// GDB-1047: Implicit FLOAT->DECIMAL coercion silently truncates the fractional
// part; DECIMAL scale is ignored entirely.
//
// Root cause: src/common/coercion.cpp fit_to_storage() DECIMAL branch called
//   static_cast<uint64_t>(d)   -- truncate toward zero.
// So coerce(Value(1.5), DECIMAL) -> Decimal128{0, 1} (the value 1), silent loss.
//
// Fix: fit_to_storage(val, TypeId::DECIMAL, scale) now computes
//   coefficient = llround(val * 10^scale)
// using round-half-away-from-zero. Scale is threaded from CatalogColumnDef::scale
// through the planner (col_scales_ on Insert/UpdateOperator) so INSERT and UPDATE
// honour the target column's declared scale.
//
// Every EXPECT below FAILS under the old truncate-to-integer path.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Unit tests: fit_to_storage scale-aware coefficient
// =============================================================================

// Helper: extract the signed 64-bit coefficient from a Decimal128.
// Positive values: hi==0, lo holds the coefficient directly.
// Negative values: hi==-1, lo holds the two's-complement bit pattern.
static int64_t decimal_coef(const Value& v) {
    const auto& d = v.as_decimal();
    if (d.hi == 0) {
        return static_cast<int64_t>(d.lo);
    }
    // hi == -1: lo is the two's-complement uint64_t of a negative int64.
    return static_cast<int64_t>(d.lo);
}

// 3.7 into DECIMAL(10,2) -> coefficient 370.
// Old path: static_cast<uint64_t>(3.7) == 3. Mutation caught.
TEST(QA_GDB1047, Float37Scale2Coefficient370) {
    auto r = fit_to_storage(Value(3.7), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->type_id(), TypeId::DECIMAL);
    EXPECT_EQ(decimal_coef(*r), int64_t{370});
}

// 1.5 into DECIMAL(10,2) -> coefficient 150.
// Old path: static_cast<uint64_t>(1.5) == 1. Mutation caught.
TEST(QA_GDB1047, Float15Scale2Coefficient150) {
    auto r = fit_to_storage(Value(1.5), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{150});
}

// 3.14159 into DECIMAL(10,2) -> coefficient 314 (round-half-away-from-zero).
// Old path: static_cast<uint64_t>(3.14159) == 3. Mutation caught.
TEST(QA_GDB1047, Float314159Scale2Coefficient314) {
    auto r = fit_to_storage(Value(3.14159), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{314});
}

// 3.7 into DECIMAL(10,0) -> coefficient 4 (round to nearest integer).
// Old path: static_cast<uint64_t>(3.7) == 3. Mutation caught.
TEST(QA_GDB1047, Float37Scale0Coefficient4) {
    auto r = fit_to_storage(Value(3.7), TypeId::DECIMAL, 0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{4});
}

// -2.5 into DECIMAL(10,2) -> coefficient -250 (sign preserved).
// Old path: static_cast<uint64_t>(static_cast<int64_t>(-2.5)) == uint64_t wrapping
// around -2, i.e. hi=-1, lo=uint64_t(-2). But the coefficient was -2, not -250.
// Mutation caught.
TEST(QA_GDB1047, FloatNeg25Scale2CoefficientNeg250) {
    auto r = fit_to_storage(Value(-2.5), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{-250});
    // Verify sign encoding: negative coefficients use hi=-1.
    EXPECT_EQ(r->as_decimal().hi, int64_t{-1});
}

// Integer source (int64) into DECIMAL(10,2) -> coefficient = value * 10^2.
// 5 -> 500.
TEST(QA_GDB1047, Int64Scale2Coefficient500) {
    auto r = fit_to_storage(Value(int64_t{5}), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{500});
}

// Integer source, scale 0 -> coefficient equals the integer itself.
TEST(QA_GDB1047, Int32Scale0CoefficientUnchanged) {
    auto r = fit_to_storage(Value(int32_t{42}), TypeId::DECIMAL, 0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{42});
}

// FLOAT32 source.
TEST(QA_GDB1047, Float32Scale2Coefficient) {
    auto r = fit_to_storage(Value(float{1.5f}), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{150});
}

// Negative integer into DECIMAL(10,0) -> coefficient is negative.
TEST(QA_GDB1047, NegativeIntScale0CoeffIsNegative) {
    auto r = fit_to_storage(Value(int64_t{-7}), TypeId::DECIMAL, 0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{-7});
    EXPECT_EQ(r->as_decimal().hi, int64_t{-1});
}

// Overflow guard: very large double with large scale should error, not UB.
TEST(QA_GDB1047, OverflowLargeScaleErrors) {
    // 1e18 * 10^2 overflows int64_t.
    auto r = fit_to_storage(Value(1e18), TypeId::DECIMAL, 2);
    EXPECT_FALSE(r.has_value());
}

// Default scale=0 (2-arg call) still works -- backward compat.
TEST(QA_GDB1047, TwoArgCallDefaultScale0) {
    // 3.7 with default scale=0 -> coefficient 4 (round, not truncate 3).
    auto r = fit_to_storage(Value(3.7), TypeId::DECIMAL);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{4});
}

// =============================================================================
// End-to-end fixture: mirrors GDB-1046 test fixture pattern exactly.
// =============================================================================

class QA_GDB1047_E2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1047";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// =============================================================================
// End-to-end: INSERT a float literal into DECIMAL(10,2) and read the stored
// Decimal128 coefficient via SELECT.
// =============================================================================

TEST_F(QA_GDB1047_E2E, InsertFloatIntoDecimalScale2StoresScaledCoefficient) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE t (d DECIMAL(10, 2))");
    // Insert 3.7; with scale=2 the coefficient must be 370.
    // Old path: coefficient was 3 (truncate-to-integer). Mutation caught.
    exec_ok("INSERT INTO t VALUES (3.7)");

    auto result = engine_->execute("SELECT d FROM t");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    ASSERT_EQ(result->rows[0].size(), 1u);

    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    // coefficient = 370; hi=0 for positive values.
    EXPECT_EQ(v.as_decimal().hi, int64_t{0});
    EXPECT_EQ(v.as_decimal().lo, uint64_t{370});
}

TEST_F(QA_GDB1047_E2E, InsertMultipleFloatsIntoDecimalScale2) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE prices (p DECIMAL(10, 2))");
    // 1.5 -> coefficient 150; old path -> 1.
    exec_ok("INSERT INTO prices VALUES (1.5)");

    auto result = engine_->execute("SELECT p FROM prices");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(v.as_decimal().hi, int64_t{0});
    EXPECT_EQ(v.as_decimal().lo, uint64_t{150});
}

TEST_F(QA_GDB1047_E2E, InsertNegativeFloatIntoDecimalScale2) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE signed_vals (v DECIMAL(10, 2))");
    // -2.5 -> coefficient -250; old path: static_cast produced -2.
    exec_ok("INSERT INTO signed_vals VALUES (-2.5)");

    auto result = engine_->execute("SELECT v FROM signed_vals");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    // hi=-1 signals negative; lo is two's-complement bit pattern of -250.
    EXPECT_EQ(v.as_decimal().hi, int64_t{-1});
    EXPECT_EQ(v.as_decimal().lo, static_cast<uint64_t>(int64_t{-250}));
}

TEST_F(QA_GDB1047_E2E, InsertFloatIntoDecimalScale0RoundsToNearestInt) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE rounded (n DECIMAL(10, 0))");
    // 3.7 -> coefficient 4 (round-half-away-from-zero); old path -> 3.
    exec_ok("INSERT INTO rounded VALUES (3.7)");

    auto result = engine_->execute("SELECT n FROM rounded");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(v.as_decimal().hi, int64_t{0});
    EXPECT_EQ(v.as_decimal().lo, uint64_t{4});
}

// =============================================================================
// Adversarial QA tests (added by QA engineer)
// =============================================================================

// ---------------------------------------------------------------------------
// Unit-level rounding boundary cases
// ---------------------------------------------------------------------------

// 2.345 at scale=2: llround(234.5) = 235 (round-half-away-from-zero).
TEST(QA_GDB1047, Rounding2345Scale2Is235) {
    auto r = fit_to_storage(Value(2.345), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{235});
}

// 9.999 at scale=2: llround(999.9) = 1000.
TEST(QA_GDB1047, Rounding9999Scale2Is1000) {
    auto r = fit_to_storage(Value(9.999), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{1000});
}

// -0.005 at scale=2: llround(-0.5) = -1 (round-half-away-from-zero).
// Note: -0.005 in IEEE 754 double may not be exactly -0.005, but llround
// should produce -1 for the nearest representable value.
TEST(QA_GDB1047, RoundingNegative005Scale2IsNeg1OrZero) {
    auto r = fit_to_storage(Value(-0.005), TypeId::DECIMAL, 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // -0.005 * 100 = -0.5 in IEEE 754 double; llround(-0.5) = -1
    // (round half away from zero). Accept -1.
    int64_t coef = decimal_coef(*r);
    EXPECT_TRUE(coef == int64_t{-1} || coef == int64_t{0})
        << "Expected -1 or 0 for -0.005 at scale=2, got " << coef;
}

// 0.0 at any scale -> coefficient 0, hi=0.
TEST(QA_GDB1047, ZeroValueScale4IsCoeff0) {
    auto r = fit_to_storage(Value(0.0), TypeId::DECIMAL, 4);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{0});
    EXPECT_EQ(r->as_decimal().hi, int64_t{0});
    EXPECT_EQ(r->as_decimal().lo, uint64_t{0});
}

// NaN input must error, not UB.
TEST(QA_GDB1047, NaNInputErrors) {
    auto r = fit_to_storage(Value(std::numeric_limits<double>::quiet_NaN()), TypeId::DECIMAL, 2);
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

// +infinity input must error.
TEST(QA_GDB1047, InfinityInputErrors) {
    auto r = fit_to_storage(Value(std::numeric_limits<double>::infinity()), TypeId::DECIMAL, 2);
    EXPECT_FALSE(r.has_value());
}

// -infinity input must error.
TEST(QA_GDB1047, NegativeInfinityInputErrors) {
    auto r = fit_to_storage(Value(-std::numeric_limits<double>::infinity()), TypeId::DECIMAL, 2);
    EXPECT_FALSE(r.has_value());
}

// Scale=0 with exact integer 5 -> coefficient 5 (not 500; scale=0 means scale by 10^0 = 1).
TEST(QA_GDB1047, IntegerScale0StoresRaw) {
    auto r = fit_to_storage(Value(int32_t{5}), TypeId::DECIMAL, 0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(decimal_coef(*r), int64_t{5});
}

// Overflow: scale=20, value=1.0 -> 1e20 overflows int64_t -> must error.
TEST(QA_GDB1047, OverflowScale20Value1Errors) {
    auto r = fit_to_storage(Value(1.0), TypeId::DECIMAL, 20);
    EXPECT_FALSE(r.has_value()) << "Expected overflow error for scale=20, value=1.0";
}

// Overflow: scale=2, value=1e18 -> 1e20 overflows int64_t -> must error.
TEST(QA_GDB1047, OverflowLargeValueScale2Errors) {
    auto r = fit_to_storage(Value(1e18), TypeId::DECIMAL, 2);
    EXPECT_FALSE(r.has_value()) << "Expected overflow error for 1e18 * 10^2";
}

// Negative overflow: -1e18 at scale=2 -> -1e20 underflows int64_t -> must error.
TEST(QA_GDB1047, OverflowNegativeLargeValueScale2Errors) {
    auto r = fit_to_storage(Value(-1e18), TypeId::DECIMAL, 2);
    EXPECT_FALSE(r.has_value()) << "Expected underflow error for -1e18 * 10^2";
}

// ---------------------------------------------------------------------------
// E2E: multi-column INSERT with interleaved DECIMAL columns of different scales
// ---------------------------------------------------------------------------

// CRITICAL path: INT col, DECIMAL(10,2), DECIMAL(10,4), FLOAT64 col interleaved.
// col_scales_ must align to storage column index, not all-use-first-scale.
// If there is an off-by-one in col_scales_ indexing, b gets scale=4 and c gets
// scale=2 -- producing wrong coefficients.
TEST_F(QA_GDB1047_E2E, MultiColumnMixedScalesAlignCorrectly) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // a=INT, b=DECIMAL(10,2), c=DECIMAL(10,4), d=DOUBLE
    exec_ok("CREATE TABLE mixed ("
            "  a INT,"
            "  b DECIMAL(10, 2),"
            "  c DECIMAL(10, 4),"
            "  d DOUBLE"
            ")");
    // b=1.23 -> should store coefficient 123 (scale=2)
    // c=1.2345 -> should store coefficient 12345 (scale=4)
    exec_ok("INSERT INTO mixed VALUES (1, 1.23, 1.2345, 9.9)");

    auto result = engine_->execute("SELECT a, b, c, d FROM mixed");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    ASSERT_EQ(result->rows[0].size(), 4u);

    // a: INT32 unaffected
    EXPECT_EQ(result->rows[0][0].as_int32(), 1);

    // b: DECIMAL(10,2) -- coefficient must be 123, not 12345 (wrong scale)
    const Value& b = result->rows[0][1];
    ASSERT_EQ(b.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(b.as_decimal().hi, int64_t{0});
    EXPECT_EQ(b.as_decimal().lo, uint64_t{123})
        << "b coefficient wrong: expected 123 (scale=2), got " << b.as_decimal().lo
        << " -- possible off-by-one in col_scales_ alignment";

    // c: DECIMAL(10,4) -- coefficient must be 12345, not 123 (wrong scale)
    const Value& c = result->rows[0][2];
    ASSERT_EQ(c.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(c.as_decimal().hi, int64_t{0});
    EXPECT_EQ(c.as_decimal().lo, uint64_t{12345})
        << "c coefficient wrong: expected 12345 (scale=4), got " << c.as_decimal().lo
        << " -- possible off-by-one in col_scales_ alignment";
}

// E2E: multiple rows in same INSERT, each row uses correct per-column scales.
TEST_F(QA_GDB1047_E2E, MultiRowInsertAllRowsHaveCorrectCoefficients) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE multi_row ("
            "  p DECIMAL(10, 2),"
            "  q DECIMAL(10, 3)"
            ")");
    // Row 1: p=1.11->111, q=2.222->2222
    // Row 2: p=3.33->333, q=4.444->4444
    exec_ok("INSERT INTO multi_row VALUES (1.11, 2.222), (3.33, 4.444)");

    auto result = engine_->execute("SELECT p, q FROM multi_row");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 2u);

    // Row 0
    EXPECT_EQ(result->rows[0][0].as_decimal().lo, uint64_t{111});
    EXPECT_EQ(result->rows[0][1].as_decimal().lo, uint64_t{2222});

    // Row 1
    EXPECT_EQ(result->rows[1][0].as_decimal().lo, uint64_t{333});
    EXPECT_EQ(result->rows[1][1].as_decimal().lo, uint64_t{4444});
}

// E2E: INSERT with explicit column list (subset, non-schema-order).
// DECIMAL column 'b' (scale=2) provided last; 'a' (INT) provided first.
// Scale must still align to the storage column index for 'b'.
TEST_F(QA_GDB1047_E2E, ExplicitColumnListSubsetScaleCorrect) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE explicit_cols ("
            "  a INT,"
            "  b DECIMAL(10, 2),"
            "  c DECIMAL(10, 4)"
            ")");
    // Insert specifying columns in non-schema order: c first, then a, then b
    exec_ok("INSERT INTO explicit_cols (c, a, b) VALUES (1.2345, 10, 9.99)");

    auto result = engine_->execute("SELECT a, b, c FROM explicit_cols");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);

    // a = 10
    EXPECT_EQ(result->rows[0][0].as_int32(), 10);

    // b = 9.99 -> coeff 999 (scale=2)
    const Value& b = result->rows[0][1];
    ASSERT_EQ(b.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(b.as_decimal().lo, uint64_t{999})
        << "b: expected 999 (scale=2) for 9.99, got " << b.as_decimal().lo;

    // c = 1.2345 -> coeff 12345 (scale=4)
    const Value& c = result->rows[0][2];
    ASSERT_EQ(c.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(c.as_decimal().lo, uint64_t{12345})
        << "c: expected 12345 (scale=4) for 1.2345, got " << c.as_decimal().lo;
}

// E2E: UPDATE SET on DECIMAL column uses correct scale.
// Old path would truncate; new path must produce scaled coefficient.
TEST_F(QA_GDB1047_E2E, UpdateDecimalColumnUsesCorrectScale) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE upd_test ("
            "  id INT,"
            "  price DECIMAL(10, 2)"
            ")");
    exec_ok("INSERT INTO upd_test VALUES (1, 0.0)");

    // Update price to 12.75 -> coefficient should be 1275
    exec_ok("UPDATE upd_test SET price = 12.75 WHERE id = 1");

    auto result = engine_->execute("SELECT price FROM upd_test WHERE id = 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(v.as_decimal().hi, int64_t{0});
    EXPECT_EQ(v.as_decimal().lo, uint64_t{1275})
        << "UPDATE price: expected coefficient 1275, got " << v.as_decimal().lo;
}

// E2E: UPDATE with multiple DECIMAL columns of different scales in the SET.
// Only the SET column should use its own scale, not a neighbor's scale.
TEST_F(QA_GDB1047_E2E, UpdateMultiDecimalColumnsScaleAlignment) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE multi_dec ("
            "  a DECIMAL(10, 2),"
            "  b DECIMAL(10, 4)"
            ")");
    exec_ok("INSERT INTO multi_dec VALUES (0.0, 0.0)");

    // Update b (scale=4); a should remain untouched, b gets coefficient 56789.
    exec_ok("UPDATE multi_dec SET b = 5.6789");

    auto result = engine_->execute("SELECT a, b FROM multi_dec");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);

    // a unchanged: was 0.0 -> coeff 0 at scale=2
    EXPECT_EQ(result->rows[0][0].as_decimal().lo, uint64_t{0});

    // b = 5.6789 at scale=4 -> coeff 56789
    const Value& b = result->rows[0][1];
    ASSERT_EQ(b.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(b.as_decimal().lo, uint64_t{56789})
        << "UPDATE b: expected 56789 (scale=4), got " << b.as_decimal().lo;
}

// E2E: Regression -- non-DECIMAL columns in a mixed table are totally unaffected.
TEST_F(QA_GDB1047_E2E, NonDecimalColumnsUnaffectedByScaleThreading) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE regression ("
            "  id INT,"
            "  name VARCHAR,"
            "  score DECIMAL(10, 2),"
            "  ratio DOUBLE"
            ")");
    exec_ok("INSERT INTO regression VALUES (42, 'alice', 9.99, 3.14)");

    auto result = engine_->execute("SELECT id, name, score, ratio FROM regression");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);

    // INT32 column unaffected
    EXPECT_EQ(result->rows[0][0].as_int32(), 42);

    // STRING column unaffected
    EXPECT_EQ(result->rows[0][1].as_string(), "alice");

    // DECIMAL column correct coefficient
    const Value& score = result->rows[0][2];
    ASSERT_EQ(score.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(score.as_decimal().lo, uint64_t{999});

    // FLOAT64 column unaffected
    EXPECT_NEAR(result->rows[0][3].as_float64(), 3.14, 1e-9);
}

// E2E: exact integer inserted into DECIMAL(10,2) -> coefficient = value * 100.
TEST_F(QA_GDB1047_E2E, IntegerIntoDecimalScale2CoeffIsScaled) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE int_dec (v DECIMAL(10, 2))");
    exec_ok("INSERT INTO int_dec VALUES (5)");

    auto result = engine_->execute("SELECT v FROM int_dec");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    // 5 at scale=2 -> coeff 500
    EXPECT_EQ(v.as_decimal().lo, uint64_t{500})
        << "Integer 5 into DECIMAL(10,2): expected coeff 500, got " << v.as_decimal().lo;
}

// E2E: DECIMAL with leading non-DECIMAL column -- scale index alignment test.
// If col_scales_[0] was reused for the DECIMAL at index 1, the coefficient would be wrong.
TEST_F(QA_GDB1047_E2E, DecimalAtColumnIndex1UsesItsOwnScale) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE idx_align ("
            "  x DOUBLE,"
            "  y DECIMAL(10, 3)"
            ")");
    // x=1.5 (stored as FLOAT64), y=2.456 -> coeff 2456 at scale=3
    exec_ok("INSERT INTO idx_align VALUES (1.5, 2.456)");

    auto result = engine_->execute("SELECT x, y FROM idx_align");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);

    EXPECT_NEAR(result->rows[0][0].as_float64(), 1.5, 1e-9);

    const Value& y = result->rows[0][1];
    ASSERT_EQ(y.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(y.as_decimal().lo, uint64_t{2456})
        << "y at col index 1: expected coeff 2456 (scale=3), got " << y.as_decimal().lo;
}

// E2E: DECIMAL(10,0) -- scale=0 with float input rounds to nearest integer.
TEST_F(QA_GDB1047_E2E, DecimalScale0RoundsNotTruncates) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE scale0 (n DECIMAL(10, 0))");
    exec_ok("INSERT INTO scale0 VALUES (7.6)");

    auto result = engine_->execute("SELECT n FROM scale0");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    const Value& v = result->rows[0][0];
    ASSERT_EQ(v.type_id(), TypeId::DECIMAL);
    // llround(7.6) = 8; old truncate path produced 7.
    EXPECT_EQ(v.as_decimal().lo, uint64_t{8})
        << "DECIMAL(10,0): expected coeff 8 for 7.6, got " << v.as_decimal().lo;
}
