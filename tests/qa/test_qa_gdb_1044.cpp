#include "sixseven/common/coercion.h"
#include "sixseven/common/value.h"

#include <gtest/gtest.h>

#include <cstdint>

using sixseven::Decimal128;
using sixseven::fit_to_storage;
using sixseven::TypeId;
using sixseven::Value;

// GDB-1044 (critical): fit_to_storage()'s numeric->integer narrowing called
// to_int64(), whose default branch returned 0 for FLOAT32/FLOAT64/DECIMAL
// sources. So `INSERT INTO t(int_col) VALUES (3.7)` bound, returned success, and
// silently stored 0 instead of 3 or an error -- silent data corruption on the
// mainline INSERT/UPDATE path. Float/decimal sources now route through the
// validated explicit_cast path (truncate-toward-zero + range checks; a loud
// error for decimals). Each EXPECT below FAILS under the old silent-0 behavior.

TEST(QA_GDB1044, FloatToIntTruncatesTowardZeroNotZero) {
    auto r = fit_to_storage(Value(3.7), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 3); // was silently 0
}

TEST(QA_GDB1044, NegativeFloatTruncatesTowardZero) {
    auto r = fit_to_storage(Value(-3.9), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), -3); // truncate toward zero, not 0
}

TEST(QA_GDB1044, Float32SourceAlsoTruncates) {
    auto r = fit_to_storage(Value(2.9f), TypeId::INT16);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int16(), 2);
}

TEST(QA_GDB1044, OutOfRangeFloatErrorsInsteadOfStoringZero) {
    // 1e30 does not fit in INT32; must error loudly, not silently store 0.
    auto r = fit_to_storage(Value(1e30), TypeId::INT32);
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB1044, DecimalToIntDoesNotSilentlyStoreZero) {
    // No defined decimal->int truncation in explicit_cast, so it must either
    // produce a faithful value or error -- never the old silent ok(0).
    auto r = fit_to_storage(Value(Decimal128{0, 5}), TypeId::INT32);
    const bool silent_zero = r.has_value() && r->as_int32() == 0;
    EXPECT_FALSE(silent_zero);
}

TEST(QA_GDB1044, IntegerNarrowingStillWorks) {
    // The fix is scoped to float/decimal sources; integer narrowing is unchanged.
    auto r = fit_to_storage(Value(int64_t{42}), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 42);
}
