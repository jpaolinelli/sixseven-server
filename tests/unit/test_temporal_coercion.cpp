// tests/unit/test_temporal_coercion.cpp
//
// Unit tests for STRING -> DATE / TIME / TIMESTAMP implicit coercion.
// GDB-1097: These tests verify behaviour that does NOT exist on origin/main --
// can_coerce(STRING, DATE/TIME/TIMESTAMP) returned false on main, so the
// coerce() calls below would have returned a TYPE_ERROR instead of a value.
// Mutation note: removing the STRING->temporal branches from can_coerce() or
// coerce() causes every ASSERT_TRUE(result.has_value()) below to fail.
//
#include "sixseven/common/coercion.h"
#include "sixseven/common/value.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Value str(const char* s) {
    return Value(std::string(s));
}

// ---------------------------------------------------------------------------
// DATE coercion
// ---------------------------------------------------------------------------

TEST(TemporalCoercion, Date_Epoch_Zero) {
    auto r = coerce(str("1970-01-01"), TypeId::DATE);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_date().days_since_epoch, 0);
}

TEST(TemporalCoercion, Date_Known_Value) {
    // 2024-01-01: days since 1970-01-01.
    // 2024 - 1970 = 54 years; rough check: result is positive and reasonable.
    auto r = coerce(str("2024-01-01"), TypeId::DATE);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_GT(r->as_date().days_since_epoch, 19000);
}

TEST(TemporalCoercion, Date_LeapDay_Valid) {
    // 2024 is a leap year.
    auto r = coerce(str("2024-02-29"), TypeId::DATE);
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

TEST(TemporalCoercion, Date_LeapDay_Invalid_NonLeap) {
    // 2023 is not a leap year.
    auto r = coerce(str("2023-02-29"), TypeId::DATE);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Date_Invalid_Month) {
    auto r = coerce(str("2024-13-01"), TypeId::DATE);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Date_NonZeroPadded_Rejected) {
    // "2024-1-1" must be rejected (not zero-padded).
    auto r = coerce(str("2024-1-1"), TypeId::DATE);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Date_NotADate) {
    auto r = coerce(str("not-a-date"), TypeId::DATE);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Date_TrailingJunk_Rejected) {
    auto r = coerce(str("2024-01-01 extra"), TypeId::DATE);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ---------------------------------------------------------------------------
// TIME coercion
// ---------------------------------------------------------------------------

TEST(TemporalCoercion, Time_Midnight) {
    auto r = coerce(str("00:00:00"), TypeId::TIME);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_time().microseconds, 0);
}

TEST(TemporalCoercion, Time_OneSecond) {
    // 1 second = 1000000 microseconds.
    auto r = coerce(str("00:00:01"), TypeId::TIME);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_time().microseconds, 1000000LL);
}

TEST(TemporalCoercion, Time_WithFractional) {
    // 00:00:01.500000 = 1500000 us.
    auto r = coerce(str("00:00:01.500000"), TypeId::TIME);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_time().microseconds, 1500000LL);
}

TEST(TemporalCoercion, Time_InvalidHour) {
    auto r = coerce(str("25:00:00"), TypeId::TIME);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Time_InvalidMinute) {
    auto r = coerce(str("00:60:00"), TypeId::TIME);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ---------------------------------------------------------------------------
// TIMESTAMP coercion
// ---------------------------------------------------------------------------

TEST(TemporalCoercion, Timestamp_Epoch) {
    // "1970-01-01 00:00:00" = 0 microseconds.
    auto r = coerce(str("1970-01-01 00:00:00"), TypeId::TIMESTAMP);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_timestamp().microseconds, 0LL);
}

TEST(TemporalCoercion, Timestamp_OneSecondAfterEpoch) {
    auto r = coerce(str("1970-01-01 00:00:01"), TypeId::TIMESTAMP);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_timestamp().microseconds, 1000000LL);
}

TEST(TemporalCoercion, Timestamp_DateOnly_IsMidnight) {
    // "1970-01-01" alone => 0 us (midnight).
    auto r = coerce(str("1970-01-01"), TypeId::TIMESTAMP);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_timestamp().microseconds, 0LL);
}

TEST(TemporalCoercion, Timestamp_TSeparator) {
    // ISO 8601 'T' separator.
    auto r = coerce(str("1970-01-01T00:00:01"), TypeId::TIMESTAMP);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_timestamp().microseconds, 1000000LL);
}

TEST(TemporalCoercion, Timestamp_WithFractional) {
    // 1970-01-01 00:00:01.500000 = 1500000 us.
    auto r = coerce(str("1970-01-01 00:00:01.500000"), TypeId::TIMESTAMP);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_timestamp().microseconds, 1500000LL);
}

TEST(TemporalCoercion, Timestamp_2024_Jan_01) {
    // Round-trip: parse "2024-01-01 00:00:00" and verify via Date epoch alignment.
    auto date_r = coerce(str("2024-01-01"), TypeId::DATE);
    ASSERT_TRUE(date_r.has_value()) << date_r.error().message;
    int64_t expected_us =
        static_cast<int64_t>(date_r->as_date().days_since_epoch) * 86400000000LL;

    auto ts_r = coerce(str("2024-01-01 00:00:00"), TypeId::TIMESTAMP);
    ASSERT_TRUE(ts_r.has_value()) << ts_r.error().message;
    EXPECT_EQ(ts_r->as_timestamp().microseconds, expected_us);
}

TEST(TemporalCoercion, Timestamp_LeapDay_Valid) {
    auto r = coerce(str("2024-02-29 00:00:00"), TypeId::TIMESTAMP);
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

TEST(TemporalCoercion, Timestamp_LeapDay_Invalid) {
    auto r = coerce(str("2023-02-29 00:00:00"), TypeId::TIMESTAMP);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Timestamp_InvalidHour) {
    auto r = coerce(str("2024-01-01 25:00:00"), TypeId::TIMESTAMP);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(TemporalCoercion, Timestamp_NotADate) {
    auto r = coerce(str("not-a-date"), TypeId::TIMESTAMP);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ---------------------------------------------------------------------------
// can_coerce
// ---------------------------------------------------------------------------

TEST(TemporalCoercion, CanCoerce_StringToDate) {
    EXPECT_TRUE(can_coerce(TypeId::STRING, TypeId::DATE));
}

TEST(TemporalCoercion, CanCoerce_StringToTime) {
    EXPECT_TRUE(can_coerce(TypeId::STRING, TypeId::TIME));
}

TEST(TemporalCoercion, CanCoerce_StringToTimestamp) {
    EXPECT_TRUE(can_coerce(TypeId::STRING, TypeId::TIMESTAMP));
}

// ---------------------------------------------------------------------------
// Cross-type comparison: STRING vs TIMESTAMP
// ---------------------------------------------------------------------------

TEST(TemporalCoercion, Compare_StringVsTimestamp_Equal) {
    Value ts_val(Timestamp{1000000LL}); // 1970-01-01 00:00:01
    Value str_val = str("1970-01-01 00:00:01");

    // timestamp = string
    auto r1 = compare(ts_val, str_val);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    EXPECT_EQ(*r1, std::strong_ordering::equal);

    // string = timestamp (reversed)
    auto r2 = compare(str_val, ts_val);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_EQ(*r2, std::strong_ordering::equal);
}

TEST(TemporalCoercion, Compare_StringVsTimestamp_Less) {
    Value ts_val(Timestamp{2000000LL});
    Value str_val = str("1970-01-01 00:00:01"); // 1000000 us

    auto r = compare(str_val, ts_val);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(TemporalCoercion, Compare_StringVsDate_Equal) {
    // days_since_epoch for 2024-01-01 computed via coerce.
    auto date_r = coerce(str("2024-01-01"), TypeId::DATE);
    ASSERT_TRUE(date_r.has_value());
    Value date_val(*date_r);
    Value str_val = str("2024-01-01");

    auto r = compare(date_val, str_val);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, std::strong_ordering::equal);
}
