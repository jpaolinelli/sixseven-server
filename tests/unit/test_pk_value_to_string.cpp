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
    EXPECT_EQ(pk_value_to_string(Value{int64_t{9223372036854775807LL}}), "9223372036854775807");

    EXPECT_EQ(pk_value_to_string(Value{uint8_t{255}}), "255");
    EXPECT_EQ(pk_value_to_string(Value{uint16_t{65535}}), "65535");
    EXPECT_EQ(pk_value_to_string(Value{uint32_t{4294967295U}}), "4294967295");
    EXPECT_EQ(pk_value_to_string(Value{uint64_t{18446744073709551615ULL}}), "18446744073709551615");
}

TEST(PkValueToString, StringIsReturnedAsIs) {
    EXPECT_EQ(pk_value_to_string(Value{std::string{"hello"}}), "hello");
    EXPECT_EQ(pk_value_to_string(Value{std::string{}}), "");
    EXPECT_EQ(pk_value_to_string(Value{std::string{"with spaces and !@#"}}), "with spaces and !@#");
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

// GDB-1011: before the fix, every non-int/string/UUID PK type fell through to
// "?<type_id>", so all values of such a type collided to one key and
// verify_pk_exists (LINK referential integrity) returned true for any probed
// PK. These tests assert each scalar type now serializes injectively: distinct
// values of the same type produce DISTINCT keys. Each EXPECT_NE FAILS under the
// old colliding fallback (both sides were "?<type_id>") and PASSES with the
// per-type serialization -- the mutation guard for this fix.

using sixseven::Blob;
using sixseven::Date;
using sixseven::Decimal128;
using sixseven::Interval;
using sixseven::JsonString;
using sixseven::Point;
using sixseven::Time;
using sixseven::Timestamp;

TEST(PkValueToString, BoolSerializesDistinctly) {
    EXPECT_EQ(pk_value_to_string(Value{true}), "1");
    EXPECT_EQ(pk_value_to_string(Value{false}), "0");
    EXPECT_NE(pk_value_to_string(Value{true}), pk_value_to_string(Value{false}));
}

TEST(PkValueToString, FloatTypesKeyByBitPatternNotRoundedDecimals) {
    // Two doubles that std::to_string would both render as "1.000000" must key
    // distinctly -- the old "?<id>" fallback collided every double.
    EXPECT_NE(pk_value_to_string(Value{1.0000001}), pk_value_to_string(Value{1.0000002}));
    EXPECT_EQ(pk_value_to_string(Value{1.0}), pk_value_to_string(Value{1.0}));
    EXPECT_NE(pk_value_to_string(Value{1.0f}), pk_value_to_string(Value{2.0f}));
}

TEST(PkValueToString, DecimalIsInjectiveAcrossHiAndLo) {
    EXPECT_NE(pk_value_to_string(Value{Decimal128{1, 2}}),
              pk_value_to_string(Value{Decimal128{2, 1}}));
    EXPECT_NE(pk_value_to_string(Value{Decimal128{0, 0}}),
              pk_value_to_string(Value{Decimal128{0, 1}}));
}

TEST(PkValueToString, DateTimeTimestampIntervalAreDistinctPerValue) {
    EXPECT_NE(pk_value_to_string(Value{Date{19000}}), pk_value_to_string(Value{Date{19001}}));
    EXPECT_NE(pk_value_to_string(Value{Time{1}}), pk_value_to_string(Value{Time{2}}));
    EXPECT_NE(pk_value_to_string(Value{Timestamp{1}}), pk_value_to_string(Value{Timestamp{2}}));
    EXPECT_NE(pk_value_to_string(Value{Interval{1, 0}}), pk_value_to_string(Value{Interval{0, 1}}));
}

TEST(PkValueToString, PointJsonBlobAreDistinctPerValue) {
    EXPECT_NE(pk_value_to_string(Value{Point{1.0, 2.0}}),
              pk_value_to_string(Value{Point{2.0, 1.0}}));
    EXPECT_NE(pk_value_to_string(Value{JsonString{"a"}}),
              pk_value_to_string(Value{JsonString{"b"}}));
    EXPECT_NE(pk_value_to_string(Value{Blob{1, 2}}), pk_value_to_string(Value{Blob{2, 1}}));
    // Length-prefix guards against a short blob colliding with a longer one.
    EXPECT_NE(pk_value_to_string(Value{Blob{0x12}}), pk_value_to_string(Value{Blob{0x01, 0x02}}));
}

TEST(PkValueToString, ScalarPkTypesNeverHitTheNonInjectiveSentinel) {
    // None of the scalar PK types should fall through to the "?<id>" branch.
    EXPECT_NE(pk_value_to_string(Value{true}).front(), '?');
    EXPECT_NE(pk_value_to_string(Value{1.5}).front(), '?');
    EXPECT_NE(pk_value_to_string(Value{Date{0}}).front(), '?');
    EXPECT_NE(pk_value_to_string(Value{Decimal128{0, 0}}).front(), '?');
    EXPECT_NE(pk_value_to_string(Value{Point{0.0, 0.0}}).front(), '?');
}
