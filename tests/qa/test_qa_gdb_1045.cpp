#include "sixseven/common/coercion.h"
#include "sixseven/common/value.h"

#include <gtest/gtest.h>

#include <cstdint>

using sixseven::fit_to_storage;
using sixseven::TypeId;
using sixseven::Value;

// GDB-1045 (audit C4): fit_to_storage()'s integer narrowing branch called
// int64_to_value() with NO range check, while its sibling explicit_cast()
// validates via fits_in_integer() and errors. So INSERT 300 into an INT8 column
// silently stored 44, and -1 into a UINT32 column silently stored 4294967295 --
// silent data corruption with a success result, reachable via INSERT/UPDATE.
// The narrowing branch now delegates to explicit_cast, so out-of-range integer
// narrowing errors loudly. Each EXPECT below FAILS under the old silent wrap.

TEST(QA_GDB1045, IntOverflowNarrowingErrorsNotWraps) {
    // 300 does not fit in INT8 (range -128..127); was silently stored as 44.
    auto r = fit_to_storage(Value(int64_t{300}), TypeId::INT8);
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB1045, NegativeIntoUnsignedErrorsNotWraps) {
    // -1 does not fit in UINT32; was silently stored as 4294967295.
    auto r = fit_to_storage(Value(int32_t{-1}), TypeId::UINT32);
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB1045, Int64OverflowIntoInt32Errors) {
    auto r = fit_to_storage(Value(int64_t{5000000000}), TypeId::INT32);
    EXPECT_FALSE(r.has_value());
}

TEST(QA_GDB1045, InRangeIntegerNarrowingStillSucceeds) {
    auto r = fit_to_storage(Value(int64_t{42}), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 42);
}

TEST(QA_GDB1045, BoundaryValueFitsExactly) {
    // 127 is the max INT8; must still succeed (not an off-by-one rejection).
    auto r = fit_to_storage(Value(int64_t{127}), TypeId::INT8);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int8(), 127);
}
