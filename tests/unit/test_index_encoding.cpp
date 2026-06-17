#include "sixseven/common/status.h"
#include "sixseven/index/index_encoding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

using namespace sixseven;
using namespace sixseven::index_encoding;

// ---------------------------------------------------------------------------
// Writer round-trip tests
// ---------------------------------------------------------------------------

TEST(IndexEncodingWriter, WriteU8RoundTrip) {
    std::vector<uint8_t> buf;
    write_u8(buf, 0x00);
    write_u8(buf, 0xAB);
    write_u8(buf, 0xFF);
    ASSERT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 0x00);
    EXPECT_EQ(buf[1], 0xAB);
    EXPECT_EQ(buf[2], 0xFF);
}

TEST(IndexEncodingWriter, WriteU16LittleEndian) {
    std::vector<uint8_t> buf;
    write_u16(buf, 0x1234);
    ASSERT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf[0], 0x34); // low byte first
    EXPECT_EQ(buf[1], 0x12);
}

TEST(IndexEncodingWriter, WriteU32LittleEndian) {
    std::vector<uint8_t> buf;
    write_u32(buf, 0xDEADBEEFu);
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0xEF);
    EXPECT_EQ(buf[1], 0xBE);
    EXPECT_EQ(buf[2], 0xAD);
    EXPECT_EQ(buf[3], 0xDE);
}

TEST(IndexEncodingWriter, WriteU64LittleEndian) {
    std::vector<uint8_t> buf;
    write_u64(buf, 0x0102030405060708ULL);
    ASSERT_EQ(buf.size(), 8u);
    EXPECT_EQ(buf[0], 0x08);
    EXPECT_EQ(buf[1], 0x07);
    EXPECT_EQ(buf[2], 0x06);
    EXPECT_EQ(buf[3], 0x05);
    EXPECT_EQ(buf[4], 0x04);
    EXPECT_EQ(buf[5], 0x03);
    EXPECT_EQ(buf[6], 0x02);
    EXPECT_EQ(buf[7], 0x01);
}

TEST(IndexEncodingWriter, WriteDoubleRoundTrip) {
    std::vector<uint8_t> buf;
    const double val = 3.14159265358979;
    write_double(buf, val);
    ASSERT_EQ(buf.size(), 8u);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_double();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_DOUBLE_EQ(*result, val);
}

// ---------------------------------------------------------------------------
// Reader happy-path round-trip tests
// ---------------------------------------------------------------------------

TEST(IndexEncodingReader, ReadU8RoundTrip) {
    std::vector<uint8_t> buf;
    write_u8(buf, 0x42);
    write_u8(buf, 0x00);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto v1 = r.read_u8();
    ASSERT_TRUE(v1.has_value()) << v1.error().message;
    EXPECT_EQ(*v1, 0x42);

    auto v2 = r.read_u8();
    ASSERT_TRUE(v2.has_value()) << v2.error().message;
    EXPECT_EQ(*v2, 0x00);

    EXPECT_TRUE(r.at_end());
}

TEST(IndexEncodingReader, ReadU16RoundTrip) {
    std::vector<uint8_t> buf;
    write_u16(buf, 0xBEEF);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto v = r.read_u16();
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(*v, 0xBEEF);
    EXPECT_TRUE(r.at_end());
}

TEST(IndexEncodingReader, ReadU32RoundTrip) {
    std::vector<uint8_t> buf;
    write_u32(buf, 0xCAFEBABEu);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto v = r.read_u32();
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(*v, 0xCAFEBABEu);
    EXPECT_TRUE(r.at_end());
}

TEST(IndexEncodingReader, ReadU64RoundTrip) {
    std::vector<uint8_t> buf;
    write_u64(buf, 0xFEDCBA9876543210ULL);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto v = r.read_u64();
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(*v, 0xFEDCBA9876543210ULL);
    EXPECT_TRUE(r.at_end());
}

TEST(IndexEncodingReader, ReadDoubleRoundTrip) {
    std::vector<uint8_t> buf;
    const double pi = 3.14159265358979323846;
    write_double(buf, pi);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto v = r.read_double();
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_DOUBLE_EQ(*v, pi);
    EXPECT_TRUE(r.at_end());
}

TEST(IndexEncodingReader, ReadBytesRoundTrip) {
    std::vector<uint8_t> buf = {0x68, 0x65, 0x6C, 0x6C, 0x6F}; // "hello"

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto v = r.read_bytes(5);
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(*v, "hello");
    EXPECT_TRUE(r.at_end());
}

// ---------------------------------------------------------------------------
// Mixed multi-field round-trip
// ---------------------------------------------------------------------------

TEST(IndexEncodingReader, MultiFieldRoundTrip) {
    std::vector<uint8_t> buf;
    write_u8(buf, 7);
    write_u16(buf, 1000);
    write_u32(buf, 999999);
    write_u64(buf, 123456789012345ULL);
    write_double(buf, -1.5);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));

    auto a = r.read_u8();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 7u);

    auto b = r.read_u16();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, 1000u);

    auto c = r.read_u32();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, 999999u);

    auto d = r.read_u64();
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d, 123456789012345ULL);

    auto e = r.read_double();
    ASSERT_TRUE(e.has_value());
    EXPECT_DOUBLE_EQ(*e, -1.5);

    EXPECT_TRUE(r.at_end());
    EXPECT_EQ(r.remaining(), 0u);
}

// ---------------------------------------------------------------------------
// Bounds-checking / truncation tests — these prove OOB reads are rejected
// ---------------------------------------------------------------------------

TEST(IndexEncodingReader, TruncatedU8ReturnsError) {
    std::vector<uint8_t> buf; // empty
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_u8();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, TruncatedU16ReturnsError) {
    std::vector<uint8_t> buf = {0x01}; // only 1 byte, need 2
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_u16();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, TruncatedU32ReturnsError) {
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03}; // only 3 bytes, need 4
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_u32();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, TruncatedU64ReturnsError) {
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04}; // only 4 bytes, need 8
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_u64();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, TruncatedDoubleReturnsError) {
    std::vector<uint8_t> buf = {0x01, 0x02}; // only 2 bytes, need 8
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_double();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, TruncatedBytesReturnsError) {
    std::vector<uint8_t> buf = {0x68, 0x65}; // only 2 bytes, requesting 5
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.read_bytes(5);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, PartialReadThenTruncation) {
    // Read one u32 successfully, then fail on second read from truncated remainder
    std::vector<uint8_t> buf;
    write_u32(buf, 42u);
    // Append only 2 bytes (not enough for another u32)
    buf.push_back(0xAA);
    buf.push_back(0xBB);

    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto first = r.read_u32();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 42u);

    auto second = r.read_u32();
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::IO_ERROR);
    // Cursor must not have advanced on failure (still at offset 4)
    EXPECT_EQ(r.pos, 4u);
}

TEST(IndexEncodingReader, SkipBoundsCheck) {
    std::vector<uint8_t> buf = {0x01, 0x02};
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto ok_skip = r.skip(1);
    ASSERT_TRUE(ok_skip.has_value());
    auto bad_skip = r.skip(10); // only 1 byte left
    EXPECT_FALSE(bad_skip.has_value());
    EXPECT_EQ(bad_skip.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, PeekU8BoundsCheck) {
    std::vector<uint8_t> buf; // empty
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto result = r.peek_u8();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(IndexEncodingReader, PeekDoesNotAdvance) {
    std::vector<uint8_t> buf = {0x77};
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    auto peeked = r.peek_u8();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(*peeked, 0x77);
    EXPECT_EQ(r.pos, 0u); // peek must not advance

    auto read = r.read_u8();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 0x77);
}

// ---------------------------------------------------------------------------
// Remaining / at_end helpers
// ---------------------------------------------------------------------------

TEST(IndexEncodingReader, RemainingCountsCorrectly) {
    std::vector<uint8_t> buf;
    write_u32(buf, 0u);
    Reader r(std::span<const uint8_t>(buf.data(), buf.size()));
    EXPECT_EQ(r.remaining(), 4u);
    EXPECT_FALSE(r.at_end());
    (void)r.read_u16();
    EXPECT_EQ(r.remaining(), 2u);
    (void)r.read_u16();
    EXPECT_EQ(r.remaining(), 0u);
    EXPECT_TRUE(r.at_end());
}
