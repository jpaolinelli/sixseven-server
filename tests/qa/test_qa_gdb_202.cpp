/// @file test_qa_gdb_202.cpp
/// @brief QA adversarial tests for GDB-202: Binary format in DataRow based on Bind result_format_codes.

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

static int16_t read_be16(const std::vector<uint8_t>& buf, size_t offset = 0) {
    return static_cast<int16_t>((static_cast<uint16_t>(buf[offset]) << 8) |
                                static_cast<uint16_t>(buf[offset + 1]));
}

static int32_t read_be32(const std::vector<uint8_t>& buf, size_t offset = 0) {
    return static_cast<int32_t>((static_cast<uint32_t>(buf[offset]) << 24) |
                                (static_cast<uint32_t>(buf[offset + 1]) << 16) |
                                (static_cast<uint32_t>(buf[offset + 2]) << 8) |
                                static_cast<uint32_t>(buf[offset + 3]));
}

static int64_t read_be64(const std::vector<uint8_t>& buf, size_t offset = 0) {
    return static_cast<int64_t>((static_cast<uint64_t>(buf[offset]) << 56) |
                                (static_cast<uint64_t>(buf[offset + 1]) << 48) |
                                (static_cast<uint64_t>(buf[offset + 2]) << 40) |
                                (static_cast<uint64_t>(buf[offset + 3]) << 32) |
                                (static_cast<uint64_t>(buf[offset + 4]) << 24) |
                                (static_cast<uint64_t>(buf[offset + 5]) << 16) |
                                (static_cast<uint64_t>(buf[offset + 6]) << 8) |
                                static_cast<uint64_t>(buf[offset + 7]));
}

/// Reconstruct a float from big-endian binary bytes.
static float float_from_be(const std::vector<uint8_t>& buf) {
    uint32_t bits = static_cast<uint32_t>(read_be32(buf));
    float result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

/// Reconstruct a double from big-endian binary bytes.
static double double_from_be(const std::vector<uint8_t>& buf) {
    uint64_t bits = static_cast<uint64_t>(read_be64(buf));
    double result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// =============================================================================
// QA_GDB202_BinaryBoundary — Integer boundary values
// =============================================================================

TEST(QA_GDB202_BinaryBoundary, Int8MinMax) {
    // INT8 maps to PG int2 (2 bytes). Verify INT8_MIN and INT8_MAX.
    auto bin_min = value_to_pg_binary(Value(std::numeric_limits<int8_t>::min()));
    ASSERT_EQ(bin_min.size(), 2u);
    EXPECT_EQ(read_be16(bin_min), std::numeric_limits<int8_t>::min());

    auto bin_max = value_to_pg_binary(Value(std::numeric_limits<int8_t>::max()));
    ASSERT_EQ(bin_max.size(), 2u);
    EXPECT_EQ(read_be16(bin_max), std::numeric_limits<int8_t>::max());
}

TEST(QA_GDB202_BinaryBoundary, Int16MinMax) {
    auto bin_min = value_to_pg_binary(Value(std::numeric_limits<int16_t>::min()));
    ASSERT_EQ(bin_min.size(), 2u);
    EXPECT_EQ(read_be16(bin_min), std::numeric_limits<int16_t>::min());

    auto bin_max = value_to_pg_binary(Value(std::numeric_limits<int16_t>::max()));
    ASSERT_EQ(bin_max.size(), 2u);
    EXPECT_EQ(read_be16(bin_max), std::numeric_limits<int16_t>::max());
}

TEST(QA_GDB202_BinaryBoundary, Int64MinMax) {
    auto bin_min = value_to_pg_binary(Value(std::numeric_limits<int64_t>::min()));
    ASSERT_EQ(bin_min.size(), 8u);
    EXPECT_EQ(read_be64(bin_min), std::numeric_limits<int64_t>::min());

    auto bin_max = value_to_pg_binary(Value(std::numeric_limits<int64_t>::max()));
    ASSERT_EQ(bin_max.size(), 8u);
    EXPECT_EQ(read_be64(bin_max), std::numeric_limits<int64_t>::max());
}

TEST(QA_GDB202_BinaryBoundary, Uint8Max) {
    // UINT8 maps to PG int2 (2 bytes). UINT8_MAX=255 must fit in int16.
    auto bin = value_to_pg_binary(Value(std::numeric_limits<uint8_t>::max()));
    ASSERT_EQ(bin.size(), 2u);
    EXPECT_EQ(read_be16(bin), 255);
}

TEST(QA_GDB202_BinaryBoundary, Uint8Zero) {
    auto bin = value_to_pg_binary(Value(static_cast<uint8_t>(0)));
    ASSERT_EQ(bin.size(), 2u);
    EXPECT_EQ(read_be16(bin), 0);
}

TEST(QA_GDB202_BinaryBoundary, Uint16Max) {
    // UINT16 maps to PG int4 (4 bytes). UINT16_MAX=65535 must fit in int32.
    auto bin = value_to_pg_binary(Value(std::numeric_limits<uint16_t>::max()));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32(bin), 65535);
}

TEST(QA_GDB202_BinaryBoundary, Uint32Max) {
    // UINT32 maps to PG int8 (8 bytes). UINT32_MAX must fit in int64.
    auto bin = value_to_pg_binary(Value(std::numeric_limits<uint32_t>::max()));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(read_be64(bin), static_cast<int64_t>(std::numeric_limits<uint32_t>::max()));
}

TEST(QA_GDB202_BinaryBoundary, Int32AllZeroBits) {
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(0)));
    ASSERT_EQ(bin.size(), 4u);
    for (auto b : bin) {
        EXPECT_EQ(b, 0u);
    }
}

TEST(QA_GDB202_BinaryBoundary, Int32AllOneBits) {
    // -1 in two's complement is all 1 bits.
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(-1)));
    ASSERT_EQ(bin.size(), 4u);
    for (auto b : bin) {
        EXPECT_EQ(b, 0xFF);
    }
}

TEST(QA_GDB202_BinaryBoundary, Int64AllOneBits) {
    auto bin = value_to_pg_binary(Value(static_cast<int64_t>(-1)));
    ASSERT_EQ(bin.size(), 8u);
    for (auto b : bin) {
        EXPECT_EQ(b, 0xFF);
    }
}

// =============================================================================
// QA_GDB202_FloatSpecial — Float/Double special IEEE 754 values
// =============================================================================

TEST(QA_GDB202_FloatSpecial, Float32PositiveInfinity) {
    float inf = std::numeric_limits<float>::infinity();
    auto bin = value_to_pg_binary(Value(inf));
    ASSERT_EQ(bin.size(), 4u);
    float result = float_from_be(bin);
    EXPECT_TRUE(std::isinf(result));
    EXPECT_GT(result, 0.0f);
}

TEST(QA_GDB202_FloatSpecial, Float32NegativeInfinity) {
    float neg_inf = -std::numeric_limits<float>::infinity();
    auto bin = value_to_pg_binary(Value(neg_inf));
    ASSERT_EQ(bin.size(), 4u);
    float result = float_from_be(bin);
    EXPECT_TRUE(std::isinf(result));
    EXPECT_LT(result, 0.0f);
}

TEST(QA_GDB202_FloatSpecial, Float32NaN) {
    float nan_val = std::numeric_limits<float>::quiet_NaN();
    auto bin = value_to_pg_binary(Value(nan_val));
    ASSERT_EQ(bin.size(), 4u);
    float result = float_from_be(bin);
    EXPECT_TRUE(std::isnan(result));
}

TEST(QA_GDB202_FloatSpecial, Float32Denormalized) {
    float denorm = std::numeric_limits<float>::denorm_min();
    auto bin = value_to_pg_binary(Value(denorm));
    ASSERT_EQ(bin.size(), 4u);
    float result = float_from_be(bin);
    EXPECT_EQ(result, denorm);
}

TEST(QA_GDB202_FloatSpecial, Float32NegativeZero) {
    float neg_zero = -0.0f;
    auto bin = value_to_pg_binary(Value(neg_zero));
    ASSERT_EQ(bin.size(), 4u);
    float result = float_from_be(bin);
    EXPECT_EQ(result, 0.0f);
    // Verify the sign bit is preserved.
    EXPECT_TRUE(std::signbit(result));
}

TEST(QA_GDB202_FloatSpecial, Float32MaxMin) {
    float f_max = std::numeric_limits<float>::max();
    auto bin_max = value_to_pg_binary(Value(f_max));
    ASSERT_EQ(bin_max.size(), 4u);
    EXPECT_FLOAT_EQ(float_from_be(bin_max), f_max);

    float f_lowest = std::numeric_limits<float>::lowest();
    auto bin_low = value_to_pg_binary(Value(f_lowest));
    ASSERT_EQ(bin_low.size(), 4u);
    EXPECT_FLOAT_EQ(float_from_be(bin_low), f_lowest);
}

TEST(QA_GDB202_FloatSpecial, Float64PositiveInfinity) {
    double inf = std::numeric_limits<double>::infinity();
    auto bin = value_to_pg_binary(Value(inf));
    ASSERT_EQ(bin.size(), 8u);
    double result = double_from_be(bin);
    EXPECT_TRUE(std::isinf(result));
    EXPECT_GT(result, 0.0);
}

TEST(QA_GDB202_FloatSpecial, Float64NegativeInfinity) {
    double neg_inf = -std::numeric_limits<double>::infinity();
    auto bin = value_to_pg_binary(Value(neg_inf));
    ASSERT_EQ(bin.size(), 8u);
    double result = double_from_be(bin);
    EXPECT_TRUE(std::isinf(result));
    EXPECT_LT(result, 0.0);
}

TEST(QA_GDB202_FloatSpecial, Float64NaN) {
    double nan_val = std::numeric_limits<double>::quiet_NaN();
    auto bin = value_to_pg_binary(Value(nan_val));
    ASSERT_EQ(bin.size(), 8u);
    double result = double_from_be(bin);
    EXPECT_TRUE(std::isnan(result));
}

TEST(QA_GDB202_FloatSpecial, Float64Denormalized) {
    double denorm = std::numeric_limits<double>::denorm_min();
    auto bin = value_to_pg_binary(Value(denorm));
    ASSERT_EQ(bin.size(), 8u);
    double result = double_from_be(bin);
    EXPECT_EQ(result, denorm);
}

TEST(QA_GDB202_FloatSpecial, Float64NegativeZero) {
    double neg_zero = -0.0;
    auto bin = value_to_pg_binary(Value(neg_zero));
    ASSERT_EQ(bin.size(), 8u);
    double result = double_from_be(bin);
    EXPECT_EQ(result, 0.0);
    EXPECT_TRUE(std::signbit(result));
}

TEST(QA_GDB202_FloatSpecial, Float64MaxMin) {
    double d_max = std::numeric_limits<double>::max();
    auto bin_max = value_to_pg_binary(Value(d_max));
    ASSERT_EQ(bin_max.size(), 8u);
    EXPECT_DOUBLE_EQ(double_from_be(bin_max), d_max);

    double d_lowest = std::numeric_limits<double>::lowest();
    auto bin_low = value_to_pg_binary(Value(d_lowest));
    ASSERT_EQ(bin_low.size(), 8u);
    EXPECT_DOUBLE_EQ(double_from_be(bin_low), d_lowest);
}

// =============================================================================
// QA_GDB202_StringBinary — String edge cases in binary mode
// =============================================================================

TEST(QA_GDB202_StringBinary, NullBytesInString) {
    // String containing embedded NUL bytes — binary format must preserve them.
    std::string with_nulls("hello\0world", 11);
    auto bin = value_to_pg_binary(Value(with_nulls));
    ASSERT_EQ(bin.size(), 11u);
    std::string result(bin.begin(), bin.end());
    EXPECT_EQ(result, with_nulls);
    EXPECT_EQ(result[5], '\0');
}

TEST(QA_GDB202_StringBinary, Utf8MultiByte) {
    // Multi-byte UTF-8 characters.
    std::string utf8 = "\xC3\xA9\xC3\xA0"; // "éà" in UTF-8.
    auto bin = value_to_pg_binary(Value(utf8));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(std::string(bin.begin(), bin.end()), utf8);
}

TEST(QA_GDB202_StringBinary, LongString) {
    // Moderately long string (10 KB).
    std::string long_str(10240, 'x');
    auto bin = value_to_pg_binary(Value(long_str));
    ASSERT_EQ(bin.size(), 10240u);
    EXPECT_EQ(std::string(bin.begin(), bin.end()), long_str);
}

TEST(QA_GDB202_StringBinary, SingleChar) {
    auto bin = value_to_pg_binary(Value(std::string("A")));
    ASSERT_EQ(bin.size(), 1u);
    EXPECT_EQ(bin[0], 'A');
}

TEST(QA_GDB202_StringBinary, AllBinaryBytes) {
    // String containing every byte value from 0x00 to 0xFF.
    std::string all_bytes(256, '\0');
    for (int i = 0; i < 256; ++i) {
        all_bytes[static_cast<size_t>(i)] = static_cast<char>(i);
    }
    auto bin = value_to_pg_binary(Value(all_bytes));
    ASSERT_EQ(bin.size(), 256u);
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(bin[static_cast<size_t>(i)], static_cast<uint8_t>(i)) << "byte " << i;
    }
}

// =============================================================================
// QA_GDB202_Fallback — Types that fall back to text representation
// =============================================================================

TEST(QA_GDB202_Fallback, Uint64FallsBackToText) {
    // UINT64 is NOT in the binary switch; it should fall back to text.
    auto bin = value_to_pg_binary(Value(static_cast<uint64_t>(18446744073709551615ULL)));
    // The result should be the text representation.
    std::string text(bin.begin(), bin.end());
    EXPECT_EQ(text, "18446744073709551615");
}

TEST(QA_GDB202_Fallback, TimeFallsBackToText) {
    Time t{43200000000}; // 12:00:00.000000
    auto bin = value_to_pg_binary(Value(t));
    std::string text(bin.begin(), bin.end());
    EXPECT_FALSE(text.empty());
    // Should contain hours:minutes format.
    EXPECT_NE(text.find(':'), std::string::npos);
}

TEST(QA_GDB202_Fallback, TimestampFallsBackToText) {
    Timestamp ts{1705334400000000}; // 2024-01-15T16:00:00 UTC.
    auto bin = value_to_pg_binary(Value(ts));
    std::string text(bin.begin(), bin.end());
    EXPECT_FALSE(text.empty());
}

TEST(QA_GDB202_Fallback, BoolDoesNotFallback) {
    // BOOL should use native binary (1 byte), NOT text fallback.
    auto bin = value_to_pg_binary(Value(true));
    ASSERT_EQ(bin.size(), 1u);
    // Text fallback would produce "true" (4 bytes). Verify it's native binary.
    EXPECT_NE(bin.size(), 4u);
}

// =============================================================================
// QA_GDB202_Null — NULL value handling
// =============================================================================

TEST(QA_GDB202_Null, NullProducesEmptyVector) {
    Value null_val;
    EXPECT_TRUE(null_val.is_null());
    auto bin = value_to_pg_binary(null_val);
    EXPECT_TRUE(bin.empty());
}

TEST(QA_GDB202_Null, NullTextAlsoWorks) {
    Value null_val;
    // value_to_pg_text should handle NULL gracefully too.
    auto text = value_to_pg_text(null_val);
    // The text representation of NULL is typically empty or special.
    // Just verify it doesn't crash.
    (void)text;
}

// =============================================================================
// QA_GDB202_ResolveFormat — resolve_format_code logic
// (Tested indirectly by observing send_data_row behavior)
// =============================================================================

TEST(QA_GDB202_ResolveFormat, TextFormatProducesTextOutput) {
    // With format code 0 (text), value_to_pg_text should be used.
    // We test this indirectly: verify text format for INT32 produces ASCII digits.
    auto text = value_to_pg_text(Value(static_cast<int32_t>(42)));
    EXPECT_EQ(text, "42");
}

TEST(QA_GDB202_ResolveFormat, BinaryFormatProducesBinaryOutput) {
    // With format code 1 (binary), value_to_pg_binary should produce 4 bytes for INT32.
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(42)));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32(bin), 42);
    // Text would produce "42" (2 bytes) — confirm we get binary (4 bytes).
    EXPECT_NE(bin.size(), 2u);
}

// =============================================================================
// QA_GDB202_BinarySize — Verify exact binary sizes for all supported types
// =============================================================================

TEST(QA_GDB202_BinarySize, AllSupportedTypesCorrectSize) {
    // BOOL: 1 byte.
    EXPECT_EQ(value_to_pg_binary(Value(true)).size(), 1u);
    // INT8: 2 bytes (PG int2).
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<int8_t>(1))).size(), 2u);
    // INT16: 2 bytes.
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<int16_t>(1))).size(), 2u);
    // INT32: 4 bytes.
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<int32_t>(1))).size(), 4u);
    // INT64: 8 bytes.
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<int64_t>(1))).size(), 8u);
    // UINT8: 2 bytes (PG int2).
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<uint8_t>(1))).size(), 2u);
    // UINT16: 4 bytes (PG int4).
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<uint16_t>(1))).size(), 4u);
    // UINT32: 8 bytes (PG int8).
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<uint32_t>(1))).size(), 8u);
    // FLOAT32: 4 bytes.
    EXPECT_EQ(value_to_pg_binary(Value(1.0f)).size(), 4u);
    // FLOAT64: 8 bytes.
    EXPECT_EQ(value_to_pg_binary(Value(1.0)).size(), 8u);
    // STRING: length of the string.
    EXPECT_EQ(value_to_pg_binary(Value(std::string("abc"))).size(), 3u);
}

// =============================================================================
// QA_GDB202_Endianness — Verify big-endian byte ordering
// =============================================================================

TEST(QA_GDB202_Endianness, Int32ByteOrder) {
    // 0x01020304 should be stored as [01, 02, 03, 04] in big-endian.
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(0x01020304)));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(bin[0], 0x01);
    EXPECT_EQ(bin[1], 0x02);
    EXPECT_EQ(bin[2], 0x03);
    EXPECT_EQ(bin[3], 0x04);
}

TEST(QA_GDB202_Endianness, Int16ByteOrder) {
    // 0x0102 should be stored as [01, 02] in big-endian.
    auto bin = value_to_pg_binary(Value(static_cast<int16_t>(0x0102)));
    ASSERT_EQ(bin.size(), 2u);
    EXPECT_EQ(bin[0], 0x01);
    EXPECT_EQ(bin[1], 0x02);
}

TEST(QA_GDB202_Endianness, Int64ByteOrder) {
    // 0x0102030405060708 in big-endian.
    auto bin = value_to_pg_binary(Value(static_cast<int64_t>(0x0102030405060708LL)));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(bin[0], 0x01);
    EXPECT_EQ(bin[1], 0x02);
    EXPECT_EQ(bin[2], 0x03);
    EXPECT_EQ(bin[3], 0x04);
    EXPECT_EQ(bin[4], 0x05);
    EXPECT_EQ(bin[5], 0x06);
    EXPECT_EQ(bin[6], 0x07);
    EXPECT_EQ(bin[7], 0x08);
}

TEST(QA_GDB202_Endianness, NegativeInt32BytePattern) {
    // -1 in two's complement is 0xFFFFFFFF.
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(-1)));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(bin[0], 0xFF);
    EXPECT_EQ(bin[1], 0xFF);
    EXPECT_EQ(bin[2], 0xFF);
    EXPECT_EQ(bin[3], 0xFF);
}

// =============================================================================
// QA_GDB202_RoundTrip — Binary encode then decode must match original
// =============================================================================

TEST(QA_GDB202_RoundTrip, Int32ValuesRoundTrip) {
    std::vector<int32_t> test_values = {
        0, 1, -1, 42, -42, 1000000, -1000000,
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(),
        0x7F, 0xFF, 0x7FFF, 0xFFFF,
    };
    for (auto val : test_values) {
        auto bin = value_to_pg_binary(Value(val));
        ASSERT_EQ(bin.size(), 4u) << "Failed for value: " << val;
        EXPECT_EQ(read_be32(bin), val) << "Round-trip failed for: " << val;
    }
}

TEST(QA_GDB202_RoundTrip, Int64ValuesRoundTrip) {
    std::vector<int64_t> test_values = {
        0, 1, -1, 1000000000000LL, -1000000000000LL,
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::max(),
    };
    for (auto val : test_values) {
        auto bin = value_to_pg_binary(Value(val));
        ASSERT_EQ(bin.size(), 8u) << "Failed for value: " << val;
        EXPECT_EQ(read_be64(bin), val) << "Round-trip failed for: " << val;
    }
}

TEST(QA_GDB202_RoundTrip, Float32ValuesRoundTrip) {
    std::vector<float> test_values = {
        0.0f, -0.0f, 1.0f, -1.0f,
        3.14f, -3.14f,
        std::numeric_limits<float>::min(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::epsilon(),
    };
    for (auto val : test_values) {
        auto bin = value_to_pg_binary(Value(val));
        ASSERT_EQ(bin.size(), 4u);
        float result = float_from_be(bin);
        EXPECT_FLOAT_EQ(result, val) << "Round-trip failed for: " << val;
    }
}

TEST(QA_GDB202_RoundTrip, Float64ValuesRoundTrip) {
    std::vector<double> test_values = {
        0.0, -0.0, 1.0, -1.0,
        2.718281828459045, -2.718281828459045,
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::epsilon(),
    };
    for (auto val : test_values) {
        auto bin = value_to_pg_binary(Value(val));
        ASSERT_EQ(bin.size(), 8u);
        double result = double_from_be(bin);
        EXPECT_DOUBLE_EQ(result, val) << "Round-trip failed for: " << val;
    }
}

// =============================================================================
// QA_GDB202_TextFormat — Verify text format still works for all types
// =============================================================================

TEST(QA_GDB202_TextFormat, Int32TextFormat) {
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int32_t>(42))), "42");
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int32_t>(-1))), "-1");
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int32_t>(0))), "0");
}

TEST(QA_GDB202_TextFormat, Int64TextFormat) {
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int64_t>(123456789012345LL))), "123456789012345");
    EXPECT_EQ(value_to_pg_text(Value(static_cast<int64_t>(-100))), "-100");
}

TEST(QA_GDB202_TextFormat, BoolTextFormat) {
    EXPECT_EQ(value_to_pg_text(Value(true)), "t");
    EXPECT_EQ(value_to_pg_text(Value(false)), "f");
}

TEST(QA_GDB202_TextFormat, StringTextFormat) {
    EXPECT_EQ(value_to_pg_text(Value(std::string("hello"))), "hello");
    EXPECT_EQ(value_to_pg_text(Value(std::string(""))), "");
}

TEST(QA_GDB202_TextFormat, Float32TextFormat) {
    auto text = value_to_pg_text(Value(3.14f));
    EXPECT_FALSE(text.empty());
    // Should parse back to approximately the same value.
    EXPECT_NEAR(std::stof(text), 3.14f, 0.001f);
}

TEST(QA_GDB202_TextFormat, Float64TextFormat) {
    auto text = value_to_pg_text(Value(2.718281828459045));
    EXPECT_FALSE(text.empty());
    // Note: text format uses default ostringstream precision (~6 digits).
    // This is a pre-existing limitation, not a GDB-202 issue.
    EXPECT_NEAR(std::stod(text), 2.718281828459045, 1e-4);
}

// =============================================================================
// QA_GDB202_BinaryTextDiff — Binary and text produce different outputs
// =============================================================================

TEST(QA_GDB202_BinaryTextDiff, Int32BinaryDiffersFromText) {
    auto bin = value_to_pg_binary(Value(static_cast<int32_t>(42)));
    auto text = value_to_pg_text(Value(static_cast<int32_t>(42)));
    // Binary: 4 bytes big-endian. Text: "42" (2 bytes).
    EXPECT_NE(bin.size(), text.size());
    // Verify they encode the same logical value.
    EXPECT_EQ(read_be32(bin), 42);
    EXPECT_EQ(text, "42");
}

TEST(QA_GDB202_BinaryTextDiff, BoolBinaryDiffersFromText) {
    auto bin = value_to_pg_binary(Value(true));
    auto text = value_to_pg_text(Value(true));
    // Binary: 1 byte (0x01). Text: "t" (1 byte).
    EXPECT_EQ(bin.size(), 1u);
    EXPECT_EQ(text.size(), 1u);
    // But the content differs: binary=0x01, text='t'=0x74.
    EXPECT_NE(bin[0], static_cast<uint8_t>(text[0]));
}
