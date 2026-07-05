#include "sixseven/common/byte_io.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Round-trip: write_native / read_native across representative widths.
// ---------------------------------------------------------------------------

TEST(ByteIo, RoundTripUint8) {
    std::vector<uint8_t> buf(1);
    size_t offset = 0;
    write_native<uint8_t>(buf, offset, 0xAB);
    EXPECT_EQ(offset, 1u);

    offset = 0;
    auto value = read_native<uint8_t>(buf, offset);
    EXPECT_EQ(value, 0xAB);
    EXPECT_EQ(offset, 1u);
}

TEST(ByteIo, RoundTripUint16) {
    std::vector<uint8_t> buf(2);
    size_t offset = 0;
    write_native<uint16_t>(buf, offset, 0x1234);
    EXPECT_EQ(offset, 2u);

    offset = 0;
    auto value = read_native<uint16_t>(buf, offset);
    EXPECT_EQ(value, 0x1234);
    EXPECT_EQ(offset, 2u);
}

TEST(ByteIo, RoundTripUint32) {
    std::vector<uint8_t> buf(4);
    size_t offset = 0;
    write_native<uint32_t>(buf, offset, 0xDEADBEEFu);
    EXPECT_EQ(offset, 4u);

    offset = 0;
    auto value = read_native<uint32_t>(buf, offset);
    EXPECT_EQ(value, 0xDEADBEEFu);
    EXPECT_EQ(offset, 4u);
}

TEST(ByteIo, RoundTripUint64) {
    std::vector<uint8_t> buf(8);
    size_t offset = 0;
    write_native<uint64_t>(buf, offset, 0x0123456789ABCDEFULL);
    EXPECT_EQ(offset, 8u);

    offset = 0;
    auto value = read_native<uint64_t>(buf, offset);
    EXPECT_EQ(value, 0x0123456789ABCDEFULL);
    EXPECT_EQ(offset, 8u);
}

TEST(ByteIo, RoundTripInt64Negative) {
    std::vector<uint8_t> buf(8);
    size_t offset = 0;
    int64_t original = -123456789012345LL;
    write_native<int64_t>(buf, offset, original);

    offset = 0;
    auto value = read_native<int64_t>(buf, offset);
    EXPECT_EQ(value, original);
}

TEST(ByteIo, RoundTripFloat64) {
    std::vector<uint8_t> buf(8);
    size_t offset = 0;
    double original = 3.14159265358979;
    write_native<double>(buf, offset, original);

    offset = 0;
    auto value = read_native<double>(buf, offset);
    EXPECT_DOUBLE_EQ(value, original);
}

// ---------------------------------------------------------------------------
// Offset advancement across consecutive writes/reads of mixed widths.
// ---------------------------------------------------------------------------

TEST(ByteIo, OffsetAdvancesAcrossMixedWidthWrites) {
    std::vector<uint8_t> buf(1 + 4 + 8);
    size_t offset = 0;
    write_native<uint8_t>(buf, offset, 0x01);
    EXPECT_EQ(offset, 1u);
    write_native<uint32_t>(buf, offset, 0x02030405u);
    EXPECT_EQ(offset, 5u);
    write_native<uint64_t>(buf, offset, 0x0607080910111213ULL);
    EXPECT_EQ(offset, 13u);

    offset = 0;
    EXPECT_EQ(read_native<uint8_t>(buf, offset), 0x01);
    EXPECT_EQ(offset, 1u);
    EXPECT_EQ(read_native<uint32_t>(buf, offset), 0x02030405u);
    EXPECT_EQ(offset, 5u);
    EXPECT_EQ(read_native<uint64_t>(buf, offset), 0x0607080910111213ULL);
    EXPECT_EQ(offset, 13u);
}

TEST(ByteIo, WriteStartingAtNonZeroOffsetDoesNotDisturbPriorBytes) {
    std::vector<uint8_t> buf(6);
    // Pre-fill a "header" byte before the region under test.
    buf[0] = 0xFF;
    size_t offset = 1;
    write_native<uint16_t>(buf, offset, 0x1122);
    write_native<uint16_t>(buf, offset, 0x3344);
    EXPECT_EQ(offset, 5u);
    EXPECT_EQ(buf[0], 0xFF); // untouched

    offset = 1;
    EXPECT_EQ(read_native<uint16_t>(buf, offset), 0x1122);
    EXPECT_EQ(read_native<uint16_t>(buf, offset), 0x3344);
}

// ---------------------------------------------------------------------------
// Golden byte-layout: locks in native-endian memcpy semantics (no LE/BE
// normalization). This is the exact format both WAL and replication encodings
// depend on.
// ---------------------------------------------------------------------------

TEST(ByteIo, GoldenLayoutUint32MatchesRawMemcpy) {
    std::vector<uint8_t> buf(4);
    size_t offset = 0;
    uint32_t original = 0x11223344u;
    write_native<uint32_t>(buf, offset, original);

    // The golden expectation: write_native must produce exactly what a raw
    // memcpy of the native representation would produce -- no byte-order
    // transformation of any kind.
    uint8_t expected[4];
    std::memcpy(expected, &original, sizeof(original));
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(buf[i], expected[i]) << "byte mismatch at index " << i;
    }
}

TEST(ByteIo, GoldenLayoutUint64MatchesRawMemcpy) {
    std::vector<uint8_t> buf(8);
    size_t offset = 0;
    uint64_t original = 0x0123456789ABCDEFULL;
    write_native<uint64_t>(buf, offset, original);

    uint8_t expected[8];
    std::memcpy(expected, &original, sizeof(original));
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(buf[i], expected[i]) << "byte mismatch at index " << i;
    }
}
