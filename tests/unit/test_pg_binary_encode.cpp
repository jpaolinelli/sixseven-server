/// @file test_pg_binary_encode.cpp
/// @brief Unit tests for PostgreSQL binary result encodings (GDB-718).
///
/// value_to_pg_binary used to fall back to the text representation for every
/// type outside BOOL/INT*/UINT8-32/FLOAT*/STRING while send_data_row labeled
/// the bytes as binary (DataRow format code 1). These tests pin the real
/// PostgreSQL binary encodings byte-for-byte for each affected type, and the
/// explicit non-support of DECIMAL (no scale at the Value level).

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

int32_t read_be32_at(const std::vector<uint8_t>& buf, size_t off) {
    return static_cast<int32_t>(
        (static_cast<uint32_t>(buf[off]) << 24) | (static_cast<uint32_t>(buf[off + 1]) << 16) |
        (static_cast<uint32_t>(buf[off + 2]) << 8) | static_cast<uint32_t>(buf[off + 3]));
}

int64_t read_be64_at(const std::vector<uint8_t>& buf, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v = (v << 8) | buf[off + i];
    }
    return static_cast<int64_t>(v);
}

int16_t read_be16_at(const std::vector<uint8_t>& buf, size_t off) {
    return static_cast<int16_t>((static_cast<uint16_t>(buf[off]) << 8) |
                                static_cast<uint16_t>(buf[off + 1]));
}

} // namespace

// =============================================================================
// DATE: int32 days since 2000-01-01 (internal: days since 1970-01-01)
// =============================================================================

TEST(PgBinaryEncode, DatePgEpochIsZero) {
    // 2000-01-01 = 10957 days after the Unix epoch = PG day 0.
    auto bin = value_to_pg_binary(Value(Date{10957}));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32_at(bin, 0), 0);
}

TEST(PgBinaryEncode, DateDayAfterPgEpochIsOne) {
    // 2000-01-02 -> PG day 1.
    auto bin = value_to_pg_binary(Value(Date{10958}));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32_at(bin, 0), 1);
}

TEST(PgBinaryEncode, DateBeforePgEpochIsNegative) {
    // 1999-12-31 -> PG day -1.
    auto bin = value_to_pg_binary(Value(Date{10956}));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32_at(bin, 0), -1);
}

TEST(PgBinaryEncode, DateUnixEpoch) {
    // 1970-01-01 -> PG day -10957.
    auto bin = value_to_pg_binary(Value(Date{0}));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_EQ(read_be32_at(bin, 0), -10957);
}

// =============================================================================
// TIME: int64 microseconds since midnight
// =============================================================================

TEST(PgBinaryEncode, TimeMidnightIsZero) {
    auto bin = value_to_pg_binary(Value(Time{0}));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(read_be64_at(bin, 0), 0);
}

TEST(PgBinaryEncode, TimeNoonExactBytes) {
    // 12:00:00 = 43'200'000'000 us = 0x0000000A0EEBB000.
    auto bin = value_to_pg_binary(Value(Time{43200000000LL}));
    ASSERT_EQ(bin.size(), 8u);
    const std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x0A, 0x0E, 0xEB, 0xB0, 0x00};
    EXPECT_EQ(bin, expected);
}

// =============================================================================
// TIMESTAMP: int64 microseconds since 2000-01-01 (internal: since 1970-01-01)
// =============================================================================

TEST(PgBinaryEncode, TimestampPgEpochIsEightZeroBytes) {
    // 2000-01-01 00:00:00 UTC = 946'684'800'000'000 us after the Unix epoch.
    auto bin = value_to_pg_binary(Value(Timestamp{946684800000000LL}));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(bin, std::vector<uint8_t>(8, 0));
}

TEST(PgBinaryEncode, TimestampOneSecondAfterPgEpoch) {
    auto bin = value_to_pg_binary(Value(Timestamp{946684801000000LL}));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(read_be64_at(bin, 0), 1000000);
}

TEST(PgBinaryEncode, TimestampBeforePgEpochIsNegative) {
    // 1999-12-31 23:59:59 UTC -> -1'000'000 PG microseconds.
    auto bin = value_to_pg_binary(Value(Timestamp{946684799000000LL}));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(read_be64_at(bin, 0), -1000000);
}

TEST(PgBinaryEncode, TimestampIsNotTextFallback) {
    // Regression (GDB-718): the old code emitted the ISO text ("2024-01-15
    // 16:00:00", 19+ bytes). Binary must be exactly 8 bytes.
    auto bin = value_to_pg_binary(Value(Timestamp{1705334400000000LL}));
    ASSERT_EQ(bin.size(), 8u);
    EXPECT_EQ(read_be64_at(bin, 0), 1705334400000000LL - 946684800000000LL);
}

// =============================================================================
// INTERVAL: int64 microseconds + int32 days (always 0) + int32 months
// =============================================================================

TEST(PgBinaryEncode, IntervalLayout) {
    auto bin = value_to_pg_binary(Value(Interval{14, 3600000000LL}));
    ASSERT_EQ(bin.size(), 16u);
    EXPECT_EQ(read_be64_at(bin, 0), 3600000000LL); // Microseconds.
    EXPECT_EQ(read_be32_at(bin, 8), 0);            // Days (no day component).
    EXPECT_EQ(read_be32_at(bin, 12), 14);          // Months.
}

TEST(PgBinaryEncode, IntervalNegativeComponents) {
    auto bin = value_to_pg_binary(Value(Interval{-2, -1000000LL}));
    ASSERT_EQ(bin.size(), 16u);
    EXPECT_EQ(read_be64_at(bin, 0), -1000000);
    EXPECT_EQ(read_be32_at(bin, 8), 0);
    EXPECT_EQ(read_be32_at(bin, 12), -2);
}

// =============================================================================
// UUID: 16 raw bytes
// =============================================================================

TEST(PgBinaryEncode, UuidRawSixteenBytes) {
    Uuid uuid{};
    for (size_t i = 0; i < 16; ++i) {
        uuid[i] = static_cast<uint8_t>(i * 17); // 0x00, 0x11, ..., 0xFF.
    }
    auto bin = value_to_pg_binary(Value(uuid));
    ASSERT_EQ(bin.size(), 16u);
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(bin[i], uuid[i]) << "byte " << i;
    }
}

TEST(PgBinaryEncode, UuidIsNotHyphenatedText) {
    // Regression (GDB-718): the old code emitted the 36-char hyphenated text.
    auto bin = value_to_pg_binary(Value(Uuid{}));
    EXPECT_EQ(bin.size(), 16u);
}

// =============================================================================
// BLOB (bytea): raw bytes
// =============================================================================

TEST(PgBinaryEncode, BlobRawBytes) {
    Blob blob = {0xDE, 0xAD, 0xBE, 0xEF};
    auto bin = value_to_pg_binary(Value(blob));
    EXPECT_EQ(bin, blob);
}

TEST(PgBinaryEncode, BlobIsNotHexText) {
    // Regression (GDB-718): the old code emitted "\xdeadbeef" (10 bytes).
    Blob blob = {0xDE, 0xAD, 0xBE, 0xEF};
    auto bin = value_to_pg_binary(Value(blob));
    ASSERT_EQ(bin.size(), 4u);
    EXPECT_NE(bin[0], '\\');
}

TEST(PgBinaryEncode, EmptyBlobIsEmpty) {
    auto bin = value_to_pg_binary(Value(Blob{}));
    EXPECT_TRUE(bin.empty());
}

// =============================================================================
// POINT: two big-endian float8 (x, y)
// =============================================================================

TEST(PgBinaryEncode, PointTwoFloat8) {
    auto bin = value_to_pg_binary(Value(Point{1.5, -2.5}));
    ASSERT_EQ(bin.size(), 16u);
    EXPECT_EQ(static_cast<uint64_t>(read_be64_at(bin, 0)), 0x3FF8000000000000ULL); // 1.5.
    EXPECT_EQ(static_cast<uint64_t>(read_be64_at(bin, 8)), 0xC004000000000000ULL); // -2.5.
}

// =============================================================================
// JSON (OID 114, not jsonb): binary representation is the text payload
// =============================================================================

TEST(PgBinaryEncode, JsonBinaryIsTextPayload) {
    const std::string payload = R"({"a":1,"b":[true,null]})";
    auto bin = value_to_pg_binary(Value(JsonString{payload}));
    EXPECT_EQ(std::string(bin.begin(), bin.end()), payload);
}

TEST(PgBinaryEncode, JsonHasNoJsonbVersionPrefix) {
    // jsonb binary starts with a version byte (1); json must not.
    auto bin = value_to_pg_binary(Value(JsonString{"{}"}));
    ASSERT_EQ(bin.size(), 2u);
    EXPECT_EQ(bin[0], '{');
}

// =============================================================================
// UINT64 (advertised as numeric): base-10000 digit groups
// =============================================================================

TEST(PgBinaryEncode, NumericUint64Zero) {
    // Zero: ndigits=0, weight=0, sign=0, dscale=0 — exactly 8 zero bytes.
    auto bin = value_to_pg_binary(Value(static_cast<uint64_t>(0)));
    EXPECT_EQ(bin, std::vector<uint8_t>(8, 0));
}

TEST(PgBinaryEncode, NumericUint64One) {
    auto bin = value_to_pg_binary(Value(static_cast<uint64_t>(1)));
    ASSERT_EQ(bin.size(), 10u);
    EXPECT_EQ(read_be16_at(bin, 0), 1); // ndigits.
    EXPECT_EQ(read_be16_at(bin, 2), 0); // weight.
    EXPECT_EQ(read_be16_at(bin, 4), 0); // sign: positive.
    EXPECT_EQ(read_be16_at(bin, 6), 0); // dscale.
    EXPECT_EQ(read_be16_at(bin, 8), 1); // digit.
}

TEST(PgBinaryEncode, NumericUint64MultipleDigitGroups) {
    // 12'345'678 = 1234 * 10000 + 5678 -> digits [1234, 5678], weight 1.
    auto bin = value_to_pg_binary(Value(static_cast<uint64_t>(12345678)));
    ASSERT_EQ(bin.size(), 12u);
    EXPECT_EQ(read_be16_at(bin, 0), 2);    // ndigits.
    EXPECT_EQ(read_be16_at(bin, 2), 1);    // weight.
    EXPECT_EQ(read_be16_at(bin, 4), 0);    // sign.
    EXPECT_EQ(read_be16_at(bin, 6), 0);    // dscale.
    EXPECT_EQ(read_be16_at(bin, 8), 1234); // First (most significant) digit.
    EXPECT_EQ(read_be16_at(bin, 10), 5678);
}

TEST(PgBinaryEncode, NumericUint64TrailingZeroGroupsTrimmed) {
    // 20000 = 2 * 10000 -> single digit 2 with weight 1 (canonical form).
    auto bin = value_to_pg_binary(Value(static_cast<uint64_t>(20000)));
    ASSERT_EQ(bin.size(), 10u);
    EXPECT_EQ(read_be16_at(bin, 0), 1); // ndigits.
    EXPECT_EQ(read_be16_at(bin, 2), 1); // weight.
    EXPECT_EQ(read_be16_at(bin, 8), 2); // digit.
}

TEST(PgBinaryEncode, NumericUint64Max) {
    // 18'446'744'073'709'551'615 -> [1844, 6744, 0737, 0955, 1615], weight 4.
    auto bin = value_to_pg_binary(Value(static_cast<uint64_t>(18446744073709551615ULL)));
    ASSERT_EQ(bin.size(), 18u);
    EXPECT_EQ(read_be16_at(bin, 0), 5); // ndigits.
    EXPECT_EQ(read_be16_at(bin, 2), 4); // weight.
    EXPECT_EQ(read_be16_at(bin, 4), 0); // sign.
    EXPECT_EQ(read_be16_at(bin, 6), 0); // dscale.
    EXPECT_EQ(read_be16_at(bin, 8), 1844);
    EXPECT_EQ(read_be16_at(bin, 10), 6744);
    EXPECT_EQ(read_be16_at(bin, 12), 737);
    EXPECT_EQ(read_be16_at(bin, 14), 955);
    EXPECT_EQ(read_be16_at(bin, 16), 1615);
}

// =============================================================================
// EMBEDDING (custom OID): pgvector wire format
// =============================================================================

TEST(PgBinaryEncode, EmbeddingPgvectorFormat) {
    auto bin = value_to_pg_binary(Value(Embedding{1.0F, -2.0F}));
    ASSERT_EQ(bin.size(), 4u + 2u * 4u);
    EXPECT_EQ(read_be16_at(bin, 0), 2);                                  // Dimension.
    EXPECT_EQ(read_be16_at(bin, 2), 0);                                  // Reserved.
    EXPECT_EQ(static_cast<uint32_t>(read_be32_at(bin, 4)), 0x3F800000u); // 1.0f.
    EXPECT_EQ(static_cast<uint32_t>(read_be32_at(bin, 8)), 0xC0000000u); // -2.0f.
}

// =============================================================================
// PATH (advertised as text): binary representation is the text payload
// =============================================================================

TEST(PgBinaryEncode, PathBinaryEqualsText) {
    Path path;
    path.steps = {{1, 7}, {2, -1}};
    Value v{path};
    auto bin = value_to_pg_binary(v);
    EXPECT_EQ(std::string(bin.begin(), bin.end()), value_to_pg_text(v));
}

// =============================================================================
// DECIMAL: no faithful binary encoding — rejected, guard returns empty
// =============================================================================

TEST(PgBinaryEncode, DecimalNotSupported) {
    EXPECT_FALSE(pg_binary_result_supported(TypeId::DECIMAL));
}

TEST(PgBinaryEncode, DecimalGuardReturnsEmptyNotText) {
    // Defense in depth: must NOT return text bytes labeled as binary.
    auto bin = value_to_pg_binary(Value(Decimal128{12, 34}));
    EXPECT_TRUE(bin.empty());
}

TEST(PgBinaryEncode, AllOtherTypesSupported) {
    for (auto type :
         {TypeId::INT8,      TypeId::INT16,    TypeId::INT32,  TypeId::INT64,   TypeId::UINT8,
          TypeId::UINT16,    TypeId::UINT32,   TypeId::UINT64, TypeId::FLOAT32, TypeId::FLOAT64,
          TypeId::BOOL,      TypeId::STRING,   TypeId::BLOB,   TypeId::DATE,    TypeId::TIME,
          TypeId::TIMESTAMP, TypeId::INTERVAL, TypeId::POINT,  TypeId::JSON,    TypeId::UUID,
          TypeId::EMBEDDING, TypeId::PATH}) {
        EXPECT_TRUE(pg_binary_result_supported(type)) << type_name(type);
    }
}

// =============================================================================
// Pre-existing encodings unchanged (sanity)
// =============================================================================

TEST(PgBinaryEncode, ExistingEncodingsUnchanged) {
    EXPECT_EQ(value_to_pg_binary(Value(static_cast<int32_t>(42))),
              (std::vector<uint8_t>{0x00, 0x00, 0x00, 0x2A}));
    EXPECT_EQ(value_to_pg_binary(Value(true)), std::vector<uint8_t>{0x01});
    auto str = value_to_pg_binary(Value(std::string("abc")));
    EXPECT_EQ(std::string(str.begin(), str.end()), "abc");
}

TEST(PgBinaryEncode, NullReturnsEmpty) {
    EXPECT_TRUE(value_to_pg_binary(Value::make_null()).empty());
}
