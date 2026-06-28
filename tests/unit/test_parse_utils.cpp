#include "sixseven/common/parse_utils.h"

#include "sixseven/common/status.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace sixseven {

// ---------------------------------------------------------------------------
// safe_stoi
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStoiValidPositive) {
    auto r = safe_stoi("42");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(ParseUtils, SafeStoiValidNegative) {
    auto r = safe_stoi("-7");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, -7);
}

TEST(ParseUtils, SafeStoiGarbage) {
    auto r = safe_stoi("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStoiEmpty) {
    auto r = safe_stoi("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStoiOutOfRange) {
    auto r = safe_stoi("99999999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// safe_stoll
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStollValidPositive) {
    auto r = safe_stoll("1234567890123");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{1234567890123});
}

TEST(ParseUtils, SafeStollValidNegative) {
    auto r = safe_stoll("-9876543210");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{-9876543210});
}

TEST(ParseUtils, SafeStollGarbage) {
    auto r = safe_stoll("not-a-number");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStollEmpty) {
    auto r = safe_stoll("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStollOutOfRange) {
    auto r = safe_stoll("99999999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// safe_stou32
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStou32Valid) {
    auto r = safe_stou32("65535");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, uint32_t{65535});
}

TEST(ParseUtils, SafeStou32Zero) {
    auto r = safe_stou32("0");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, uint32_t{0});
}

TEST(ParseUtils, SafeStou32Max) {
    auto r = safe_stou32("4294967295");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<uint32_t>::max());
}

TEST(ParseUtils, SafeStou32TooLarge) {
    auto r = safe_stou32("4294967296");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStou32VeryLarge) {
    auto r = safe_stou32("99999999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStou32Garbage) {
    auto r = safe_stou32("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStou32Empty) {
    auto r = safe_stou32("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// safe_stoul
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStoaulValid) {
    auto r = safe_stoul("12345");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 12345UL);
}

TEST(ParseUtils, SafeStoaulGarbage) {
    auto r = safe_stoul("xyz");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStoaulEmpty) {
    auto r = safe_stoul("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// safe_stoull
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStoullValid) {
    auto r = safe_stoull("18446744073709551615");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::numeric_limits<uint64_t>::max());
}

TEST(ParseUtils, SafeStoullGarbage) {
    auto r = safe_stoull("!!!!");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStoullEmpty) {
    auto r = safe_stoull("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// safe_stod
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStodValid) {
    auto r = safe_stod("3.14");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 3.14);
}

TEST(ParseUtils, SafeStodValidNegative) {
    auto r = safe_stod("-2.718");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, -2.718);
}

TEST(ParseUtils, SafeStodGarbage) {
    auto r = safe_stod("nope");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStodEmpty) {
    auto r = safe_stod("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// safe_stof
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStofValid) {
    auto r = safe_stof("1.5");
    ASSERT_TRUE(r.has_value());
    EXPECT_FLOAT_EQ(*r, 1.5f);
}

TEST(ParseUtils, SafeStofGarbage) {
    auto r = safe_stof("???");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(ParseUtils, SafeStofEmpty) {
    auto r = safe_stof("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// Error message checks
// ---------------------------------------------------------------------------

TEST(ParseUtils, SafeStollOutOfRangeMessage) {
    auto r = safe_stoll("99999999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "integer literal out of range");
}

TEST(ParseUtils, SafeStollInvalidMessage) {
    auto r = safe_stoll("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "invalid integer literal");
}

TEST(ParseUtils, SafeStodInvalidMessage) {
    auto r = safe_stod("xyz");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "invalid float literal");
}

} // namespace sixseven
