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

#include <cstdint>
#include <filesystem>
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
