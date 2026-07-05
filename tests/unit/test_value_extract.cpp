#include "sixseven/graph/value_extract.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace sixseven {
namespace {

// -- value_to_int64 -----------------------------------------------------

TEST(ValueExtract, Int64FromSignedIntegerTypes) {
    EXPECT_EQ(*value_to_int64(Value(static_cast<int8_t>(5)), "ctx"), 5);
    EXPECT_EQ(*value_to_int64(Value(static_cast<int16_t>(-7)), "ctx"), -7);
    EXPECT_EQ(*value_to_int64(Value(static_cast<int32_t>(1234)), "ctx"), 1234);
    EXPECT_EQ(*value_to_int64(Value(static_cast<int64_t>(-99999)), "ctx"), -99999);
}

TEST(ValueExtract, Int64FromUnsignedIntegerTypes) {
    EXPECT_EQ(*value_to_int64(Value(static_cast<uint8_t>(5)), "ctx"), 5);
    EXPECT_EQ(*value_to_int64(Value(static_cast<uint16_t>(500)), "ctx"), 500);
    EXPECT_EQ(*value_to_int64(Value(static_cast<uint32_t>(70000)), "ctx"), 70000);
}

// Documents the current (latent, behavior-preserving) quirk: a uint64_t value
// larger than INT64_MAX is truncated by static_cast<int64_t> rather than
// producing an error. This is intentional pass-through of pre-existing
// behavior from the 11 duplicated copies -- not a new decision.
TEST(ValueExtract, Int64FromUint64BoundaryWrapsSilently) {
    const uint64_t huge = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1000;
    auto result = value_to_int64(Value(huge), "ctx");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, static_cast<int64_t>(huge));
    EXPECT_LT(*result, 0) << "uint64 value beyond INT64_MAX wraps to negative, as-implemented";
}

TEST(ValueExtract, Int64FromUint64WithinRangeSucceeds) {
    const uint64_t small = 42;
    auto result = value_to_int64(Value(small), "ctx");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(ValueExtract, Int64NullReturnsInvalidArgument) {
    auto result = value_to_int64(Value(), "ctx");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error().message, "NULL node key in edge");
}

TEST(ValueExtract, Int64TypeErrorIncludesContextLabel) {
    auto result = value_to_int64(Value(std::string("not-a-number")), "pagerank");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "pagerank requires integer node keys");
}

TEST(ValueExtract, Int64TypeErrorLabelVariesByCaller) {
    auto result = value_to_int64(Value(3.14), "community_detect");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "community_detect requires integer node keys");
}

// -- value_to_double ------------------------------------------------------

TEST(ValueExtract, DoubleFromArithmeticTypes) {
    EXPECT_DOUBLE_EQ(*value_to_double(Value(3.5), "ctx"), 3.5);
    EXPECT_DOUBLE_EQ(*value_to_double(Value(static_cast<int32_t>(7)), "ctx"), 7.0);
    EXPECT_DOUBLE_EQ(*value_to_double(Value(static_cast<float>(1.5)), "ctx"), 1.5);
    EXPECT_DOUBLE_EQ(*value_to_double(Value(true), "ctx"), 1.0);
}

TEST(ValueExtract, DoubleNullReturnsInvalidArgument) {
    auto result = value_to_double(Value(), "damping parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error().message, "NULL parameter value");
}

TEST(ValueExtract, DoubleTypeErrorIncludesContextLabel) {
    auto result = value_to_double(Value(std::string("nope")), "damping parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "expected numeric value for damping parameter");
}

TEST(ValueExtract, DoubleTypeErrorLabelVariesByCaller) {
    auto result = value_to_double(Value(std::string("nope")), "tolerance parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "expected numeric value for tolerance parameter");
}

// -- value_to_string ------------------------------------------------------

TEST(ValueExtract, StringFromStringValue) {
    auto result = value_to_string(Value(std::string("out")), "direction parameter");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "out");
}

TEST(ValueExtract, StringNullReturnsInvalidArgument) {
    auto result = value_to_string(Value(), "direction parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error().message, "NULL parameter value");
}

TEST(ValueExtract, StringTypeErrorIncludesContextLabel) {
    auto result = value_to_string(Value(static_cast<int64_t>(42)), "direction parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_EQ(result.error().message, "expected string value for direction parameter");
}

TEST(ValueExtract, StringTypeErrorLabelVariesByCaller) {
    auto result = value_to_string(Value(static_cast<int64_t>(42)), "variant parameter");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "expected string value for variant parameter");
}

} // namespace
} // namespace sixseven
