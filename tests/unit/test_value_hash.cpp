#include "sixseven/common/value.h"
#include "sixseven/common/value_hash.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

using sixseven::Blob;
using sixseven::Date;
using sixseven::Decimal128;
using sixseven::Interval;
using sixseven::JsonString;
using sixseven::Point;
using sixseven::Time;
using sixseven::Timestamp;
using sixseven::Uuid;
using sixseven::Value;
using sixseven::ValueHash;

namespace {
size_t H(const Value& v) {
    return ValueHash{}(v);
}
} // namespace

// GDB-1042: ValueHash::operator() returned a constant 0 for Decimal128, Date,
// Time, Timestamp, Interval, Point, JsonString, Uuid, and Blob -- so a hash
// index on any of those columns collapsed every key into one ever-growing
// bucket (GDB-244's same-hash split guard never fired), making the "O(1)" hash
// index a linear scan. Each test below pins distinct values of a type to
// DISTINCT hashes: every EXPECT_NE FAILS under the old constant-0 (the mutation
// guard) and passes with the per-type hashing.

TEST(ValueHash, DecimalDistinctAcrossHiAndLo) {
    EXPECT_NE(H(Value{Decimal128{1, 2}}), H(Value{Decimal128{2, 1}}));
    EXPECT_NE(H(Value{Decimal128{0, 0}}), H(Value{Decimal128{0, 1}}));
}

TEST(ValueHash, DateTimeTimestampDistinctPerValue) {
    EXPECT_NE(H(Value{Date{1}}), H(Value{Date{2}}));
    EXPECT_NE(H(Value{Time{1}}), H(Value{Time{2}}));
    EXPECT_NE(H(Value{Timestamp{1}}), H(Value{Timestamp{2}}));
}

TEST(ValueHash, IntervalAndPointDistinctPerValue) {
    EXPECT_NE(H(Value{Interval{1, 0}}), H(Value{Interval{0, 1}}));
    EXPECT_NE(H(Value{Point{1.0, 2.0}}), H(Value{Point{2.0, 1.0}}));
}

TEST(ValueHash, JsonUuidBlobDistinctPerValue) {
    EXPECT_NE(H(Value{JsonString{"a"}}), H(Value{JsonString{"b"}}));

    Uuid a{};
    Uuid b{};
    a[0] = 0x01;
    b[0] = 0x02;
    EXPECT_NE(H(Value{a}), H(Value{b}));

    EXPECT_NE(H(Value{Blob{1, 2}}), H(Value{Blob{2, 1}}));
}

// The hash-set contract: values that compare equal must hash equal.
TEST(ValueHash, EqualValuesProduceEqualHashes) {
    EXPECT_EQ(H(Value{Date{42}}), H(Value{Date{42}}));
    EXPECT_EQ(H(Value{Decimal128{5, 9}}), H(Value{Decimal128{5, 9}}));
    EXPECT_EQ(H(Value{Timestamp{123456}}), H(Value{Timestamp{123456}}));

    Uuid u{};
    for (size_t i = 0; i < u.size(); ++i) {
        u[i] = static_cast<uint8_t>(i);
    }
    Uuid copy = u;
    EXPECT_EQ(H(Value{u}), H(Value{copy}));
}
