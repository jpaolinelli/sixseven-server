#include "sixseven/common/uuid.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- parse_uuid ---------------------------------------------------------------

TEST(ParseUuid, ValidLowercase) {
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(*result, expected);
}

TEST(ParseUuid, ValidUppercase) {
    auto result = parse_uuid("D1458B55-F0BF-44D4-B191-E52F1EF1F60A");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(*result, expected);
}

TEST(ParseUuid, ValidMixedCase) {
    auto result = parse_uuid("d1458B55-F0bf-44D4-b191-E52f1eF1f60A");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(*result, expected);
}

TEST(ParseUuid, AllZeros) {
    auto result = parse_uuid("00000000-0000-0000-0000-000000000000");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(*result, expected);
}

TEST(ParseUuid, AllFs) {
    auto result = parse_uuid("ffffffff-ffff-ffff-ffff-ffffffffffff");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff};
    EXPECT_EQ(*result, expected);
}

TEST(ParseUuid, TooShort) {
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(ParseUuid, TooLong) {
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191-e52f1ef1f60a0");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(ParseUuid, EmptyString) {
    auto result = parse_uuid("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(ParseUuid, MissingHyphens) {
    auto result = parse_uuid("d1458b55f0bf44d4b191e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(ParseUuid, HyphensInWrongPositions) {
    auto result = parse_uuid("d1458b5-5f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(ParseUuid, InvalidHexCharacter) {
    auto result = parse_uuid("g1458b55-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(ParseUuid, SpacesNotAllowed) {
    auto result = parse_uuid("d1458b55 f0bf 44d4 b191 e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- to_hex ---------------------------------------------------------------

TEST(ToHex, EmptyInput) {
    EXPECT_EQ(to_hex(nullptr, 0), "");
}

TEST(ToHex, SingleByteZero) {
    const uint8_t data[] = {0x00};
    EXPECT_EQ(to_hex(data, 1), "00");
}

TEST(ToHex, SingleByteLowNibble) {
    const uint8_t data[] = {0x0f};
    EXPECT_EQ(to_hex(data, 1), "0f");
}

TEST(ToHex, SingleByteHighBitSet) {
    // Regression: static_cast<int>(uint8_t) sign-extension bug would render
    // 0xff/0x80 incorrectly if the byte were treated as signed char.
    const uint8_t data[] = {0xff};
    EXPECT_EQ(to_hex(data, 1), "ff");
}

TEST(ToHex, SingleByte0x80) {
    const uint8_t data[] = {0x80};
    EXPECT_EQ(to_hex(data, 1), "80");
}

TEST(ToHex, MultiByteWithHighBitsLowercaseNoSeparators) {
    const uint8_t data[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x80, 0xff};
    EXPECT_EQ(to_hex(data, sizeof(data)), "deadbeef000180ff");
}

// -- format_uuid ------------------------------------------------------------

TEST(FormatUuid, KnownVectorCanonicalDashedLowercase) {
    Uuid uuid = {0xd1,
                 0x45,
                 0x8b,
                 0x55,
                 0xf0,
                 0xbf,
                 0x44,
                 0xd4,
                 0xb1,
                 0x91,
                 0xe5,
                 0x2f,
                 0x1e,
                 0xf1,
                 0xf6,
                 0x0a};
    EXPECT_EQ(format_uuid(uuid), "d1458b55-f0bf-44d4-b191-e52f1ef1f60a");
}

TEST(FormatUuid, AllZeros) {
    Uuid uuid = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(format_uuid(uuid), "00000000-0000-0000-0000-000000000000");
}

TEST(FormatUuid, AllFsHighBitBytes) {
    Uuid uuid = {0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff,
                 0xff};
    EXPECT_EQ(format_uuid(uuid), "ffffffff-ffff-ffff-ffff-ffffffffffff");
}

TEST(FormatUuid, RoundTripsWithParseUuid) {
    const std::string original = "d1458b55-f0bf-44d4-b191-e52f1ef1f60a";
    auto parsed = parse_uuid(original);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(format_uuid(*parsed), original);
}
