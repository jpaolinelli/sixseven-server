#include "sixseven/common/value.h"
#include "sixseven/executor/pk_value_string.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

using sixseven::pk_value_null_sentinel;
using sixseven::pk_value_to_string;
using sixseven::Uuid;
using sixseven::Value;

// Regression test for the bug that crashed the server during heavy LINK load:
// pk_value_to_string was switching on Value::type_id() without checking
// is_null() first. Because Value::type_id() returns TypeId::INT8 as a
// placeholder for NULL (variant index 0 = std::monostate), the INT8 case
// called Value::as_int8() -> std::get<int8_t> on a monostate variant ->
// std::bad_variant_access -> std::terminate. See query_engine.cpp:1031 (old)
// and value.h:169-175.
TEST(PkValueToString, NullValueReturnsSentinelInsteadOfCrashing) {
    Value null_v = Value::make_null();
    ASSERT_TRUE(null_v.is_null());

    // The critical assertion: this must NOT throw bad_variant_access.
    std::string s;
    ASSERT_NO_THROW({ s = pk_value_to_string(null_v); });

    EXPECT_EQ(s, std::string(pk_value_null_sentinel));
}

TEST(PkValueToString, NullSentinelDoesNotCollideWithRealPkStrings) {
    // The sentinel begins with 0x01 which is not a printable character and
    // cannot be produced by std::to_string for any integer, by a UUID hex
    // dump, or by an empty/normal string. Verify against representative
    // values from each supported type.
    const std::string sentinel = pk_value_null_sentinel;

    EXPECT_NE(sentinel, pk_value_to_string(Value{int8_t{0}}));
    EXPECT_NE(sentinel, pk_value_to_string(Value{int64_t{-1}}));
    EXPECT_NE(sentinel, pk_value_to_string(Value{uint64_t{0}}));
    EXPECT_NE(sentinel, pk_value_to_string(Value{std::string{}}));
    EXPECT_NE(sentinel, pk_value_to_string(Value{std::string{"NULL"}}));
    EXPECT_NE(sentinel, pk_value_to_string(Value{std::string{"\x01NUL"}}));
}

TEST(PkValueToString, IntegerTypesRoundTripToDecimalString) {
    EXPECT_EQ(pk_value_to_string(Value{int8_t{-128}}), "-128");
    EXPECT_EQ(pk_value_to_string(Value{int16_t{32767}}), "32767");
    EXPECT_EQ(pk_value_to_string(Value{int32_t{-1}}), "-1");
    EXPECT_EQ(pk_value_to_string(Value{int64_t{9223372036854775807LL}}),
              "9223372036854775807");

    EXPECT_EQ(pk_value_to_string(Value{uint8_t{255}}), "255");
    EXPECT_EQ(pk_value_to_string(Value{uint16_t{65535}}), "65535");
    EXPECT_EQ(pk_value_to_string(Value{uint32_t{4294967295U}}), "4294967295");
    EXPECT_EQ(pk_value_to_string(Value{uint64_t{18446744073709551615ULL}}),
              "18446744073709551615");
}

TEST(PkValueToString, StringIsReturnedAsIs) {
    EXPECT_EQ(pk_value_to_string(Value{std::string{"hello"}}), "hello");
    EXPECT_EQ(pk_value_to_string(Value{std::string{}}), "");
    EXPECT_EQ(pk_value_to_string(Value{std::string{"with spaces and !@#"}}),
              "with spaces and !@#");
}

TEST(PkValueToString, UuidIsHexEncoded32Chars) {
    Uuid u{};
    for (size_t i = 0; i < u.size(); ++i) {
        u[i] = static_cast<uint8_t>(i);
    }
    std::string s = pk_value_to_string(Value{u});
    EXPECT_EQ(s.size(), 32u);
    EXPECT_EQ(s, "000102030405060708090a0b0c0d0e0f");
}

TEST(PkValueToString, DistinctUuidsProduceDistinctStrings) {
    Uuid a{};
    Uuid b{};
    a[0] = 0x01;
    b[0] = 0x02;
    EXPECT_NE(pk_value_to_string(Value{a}), pk_value_to_string(Value{b}));
}

TEST(PkValueToString, BoolFallsThroughToTypePrefixSentinel) {
    // BOOL is intentionally not a supported PK type — verify the default
    // branch produces a "?<type_id>" string and never throws.
    std::string s;
    ASSERT_NO_THROW({ s = pk_value_to_string(Value{true}); });
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.front(), '?');
}
