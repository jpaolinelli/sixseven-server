/// @file test_qa_gdb_856.cpp
/// @brief Adversarial QA tests for GDB-856: Shared bounds-checked index_encoding helpers.
///
/// Focus areas per handoff:
///   1. read_bytes with hostile length fields (SIZE_MAX, UINT32_MAX, len==remaining,
///      len==remaining+1, len==0)
///   2. Truncation at EVERY field boundary for each primitive type
///   3. Cursor non-advancement on failure (position integrity)
///   4. advance() past end does not crash remaining()/at_end() (they guard gracefully)
///   5. peek_u8 at exactly end-of-buffer; skip past end
///   6. Interleaved peek/read/skip sequences approaching the boundary
///   7. Round-trip for all widths with max/min boundary values
///   8. Empty buffer baseline (all reads return IO_ERROR)
///   9. Single-byte buffer — succeeds for u8, fails for u16/u32/u64/double/bytes(2+)
///  10. Write maximal values and read them back (UINT8_MAX, UINT16_MAX, UINT32_MAX,
///      UINT64_MAX, +/-infinity, NaN, -0.0)

#include "sixseven/common/status.h"
#include "sixseven/index/index_encoding.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace sixseven;
using namespace sixseven::index_encoding;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

Reader make_reader(const std::vector<uint8_t>& buf) {
    return Reader(std::span<const uint8_t>(buf.data(), buf.size()));
}

} // namespace

// ===========================================================================
// Suite: QA_GDB856_HostileLengths
// read_bytes() with extreme and boundary length values
// ===========================================================================

TEST(QA_GDB856_HostileLengths, ReadBytesZeroLengthSucceeds) {
    // Reading 0 bytes from a non-empty buffer must succeed and return empty string.
    std::vector<uint8_t> buf = {0xAA, 0xBB};
    auto r = make_reader(buf);
    auto res = r.read_bytes(0);
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(res->size(), 0u);
    EXPECT_EQ(r.pos, 0u); // cursor must not advance
}

TEST(QA_GDB856_HostileLengths, ReadBytesZeroLengthOnEmptyBuffer) {
    // Reading 0 bytes from an empty buffer must also succeed.
    std::vector<uint8_t> buf;
    auto r = make_reader(buf);
    auto res = r.read_bytes(0);
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(res->size(), 0u);
}

TEST(QA_GDB856_HostileLengths, ReadBytesExactlyRemaining) {
    // len == remaining: must succeed, cursor at end.
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
    auto r = make_reader(buf);
    auto res = r.read_bytes(3);
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(res->size(), 3u);
    EXPECT_TRUE(r.at_end());
}

TEST(QA_GDB856_HostileLengths, ReadBytesOneMoreThanRemaining) {
    // len == remaining + 1: must reject with IO_ERROR, cursor must not advance.
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
    auto r = make_reader(buf);
    auto res = r.read_bytes(4); // only 3 available
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u); // cursor not advanced on failure
}

TEST(QA_GDB856_HostileLengths, ReadBytesMaxSizeT) {
    // SIZE_MAX length on a tiny buffer must reject without allocating gigabytes.
    std::vector<uint8_t> buf = {0xDE, 0xAD};
    auto r = make_reader(buf);
    auto res = r.read_bytes(std::numeric_limits<size_t>::max());
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_HostileLengths, ReadBytesUint32Max) {
    // 0xFFFFFFFF length on a short buffer must reject cleanly.
    std::vector<uint8_t> buf = {0x01};
    auto r = make_reader(buf);
    auto res = r.read_bytes(0xFFFFFFFFu);
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_HostileLengths, ReadBytesAfterPartialAdvance) {
    // Reproducer for GDB-857: integer overflow in read_bytes() bounds check.
    //
    // When pos=2 and n=SIZE_MAX-1, the expression `pos + n` wraps to 0
    // (unsigned overflow on 64-bit), making `0 > data.size()` FALSE — the
    // hostile length bypasses the guard and the std::string constructor
    // throws std::length_error instead of returning IO_ERROR.
    //
    // The correct check is `n > remaining()` to avoid the pos+n wrap.
    //
    // EXPECTED: EXPECT_FALSE(res.has_value()) with StatusCode::IO_ERROR
    // ACTUAL:   throws std::length_error (C++ exception escapes the Result boundary)
    //
    // This test is left FAILING as the reproducer for GDB-857.
    std::vector<uint8_t> buf = {0xAA, 0xBB, 0xCC};
    auto r = make_reader(buf);
    ASSERT_TRUE(r.read_u16().has_value());
    EXPECT_EQ(r.pos, 2u);
    auto res = r.read_bytes(std::numeric_limits<size_t>::max() - 1);
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 2u);
}

// ===========================================================================
// Suite: QA_GDB856_TruncationAtEveryByte
// For each primitive, truncate 1 byte before the required size and confirm
// IO_ERROR. Also verify the position is NOT advanced on failure.
// ===========================================================================

TEST(QA_GDB856_TruncationAtEveryByte, U8EmptyBuffer) {
    std::vector<uint8_t> buf;
    auto r = make_reader(buf);
    auto res = r.read_u8();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_TruncationAtEveryByte, U16OneByte) {
    std::vector<uint8_t> buf = {0xFF};
    auto r = make_reader(buf);
    auto res = r.read_u16();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_TruncationAtEveryByte, U32ThreeBytes) {
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
    auto r = make_reader(buf);
    auto res = r.read_u32();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_TruncationAtEveryByte, U64SevenBytes) {
    std::vector<uint8_t> buf = {1, 2, 3, 4, 5, 6, 7}; // need 8
    auto r = make_reader(buf);
    auto res = r.read_u64();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_TruncationAtEveryByte, DoubleSevenBytes) {
    std::vector<uint8_t> buf = {1, 2, 3, 4, 5, 6, 7};
    auto r = make_reader(buf);
    auto res = r.read_double();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

// Truncation at boundaries mid-stream (after a successful read)
TEST(QA_GDB856_TruncationAtEveryByte, MidStreamTruncationU32) {
    // Write one valid u32, then truncate to only 2 bytes of a second u32.
    std::vector<uint8_t> buf;
    write_u32(buf, 0xDEADBEEFu);
    buf.push_back(0xAA);
    buf.push_back(0xBB); // only 2 of 4 bytes for second u32

    auto r = make_reader(buf);
    auto first = r.read_u32();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0xDEADBEEFu);
    EXPECT_EQ(r.pos, 4u);

    auto second = r.read_u32();
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 4u); // must not advance on failure
}

TEST(QA_GDB856_TruncationAtEveryByte, MidStreamTruncationU64) {
    std::vector<uint8_t> buf;
    write_u64(buf, 0x0102030405060708ULL);
    // Add 4 bytes (not enough for another u64)
    buf.push_back(0xAA);
    buf.push_back(0xBB);
    buf.push_back(0xCC);
    buf.push_back(0xDD);

    auto r = make_reader(buf);
    auto first = r.read_u64();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(r.pos, 8u);

    auto second = r.read_u64();
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 8u);
}

TEST(QA_GDB856_TruncationAtEveryByte, MidStreamTruncationDouble) {
    std::vector<uint8_t> buf;
    write_double(buf, 1.5);
    buf.push_back(0xFF); // only 1 byte for second double (need 8)

    auto r = make_reader(buf);
    ASSERT_TRUE(r.read_double().has_value());
    EXPECT_EQ(r.pos, 8u);

    auto second = r.read_double();
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 8u);
}

// ===========================================================================
// Suite: QA_GDB856_BoundaryValues
// Max/min/special values round-trip correctly.
// ===========================================================================

TEST(QA_GDB856_BoundaryValues, U8MaxValue) {
    std::vector<uint8_t> buf;
    write_u8(buf, 0xFF);
    auto r = make_reader(buf);
    auto v = r.read_u8();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0xFFu);
}

TEST(QA_GDB856_BoundaryValues, U8Zero) {
    std::vector<uint8_t> buf;
    write_u8(buf, 0x00);
    auto r = make_reader(buf);
    auto v = r.read_u8();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0x00u);
}

TEST(QA_GDB856_BoundaryValues, U16MaxValue) {
    std::vector<uint8_t> buf;
    write_u16(buf, 0xFFFF);
    auto r = make_reader(buf);
    auto v = r.read_u16();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0xFFFFu);
}

TEST(QA_GDB856_BoundaryValues, U32MaxValue) {
    std::vector<uint8_t> buf;
    write_u32(buf, 0xFFFFFFFFu);
    auto r = make_reader(buf);
    auto v = r.read_u32();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0xFFFFFFFFu);
}

TEST(QA_GDB856_BoundaryValues, U64MaxValue) {
    std::vector<uint8_t> buf;
    write_u64(buf, std::numeric_limits<uint64_t>::max());
    auto r = make_reader(buf);
    auto v = r.read_u64();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::numeric_limits<uint64_t>::max());
}

TEST(QA_GDB856_BoundaryValues, DoublePositiveInfinity) {
    std::vector<uint8_t> buf;
    const double inf = std::numeric_limits<double>::infinity();
    write_double(buf, inf);
    auto r = make_reader(buf);
    auto v = r.read_double();
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(std::isinf(*v));
    EXPECT_GT(*v, 0.0);
}

TEST(QA_GDB856_BoundaryValues, DoubleNegativeInfinity) {
    std::vector<uint8_t> buf;
    const double neg_inf = -std::numeric_limits<double>::infinity();
    write_double(buf, neg_inf);
    auto r = make_reader(buf);
    auto v = r.read_double();
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(std::isinf(*v));
    EXPECT_LT(*v, 0.0);
}

TEST(QA_GDB856_BoundaryValues, DoubleNaN) {
    std::vector<uint8_t> buf;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    write_double(buf, nan);
    auto r = make_reader(buf);
    auto v = r.read_double();
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(std::isnan(*v));
}

TEST(QA_GDB856_BoundaryValues, DoubleNegativeZero) {
    std::vector<uint8_t> buf;
    const double neg_zero = -0.0;
    write_double(buf, neg_zero);
    auto r = make_reader(buf);
    auto v = r.read_double();
    ASSERT_TRUE(v.has_value());
    // Verify bit-identical round-trip: -0.0 and +0.0 compare equal but differ in sign bit.
    uint64_t bits_in = 0;
    uint64_t bits_out = 0;
    std::memcpy(&bits_in, &neg_zero, 8);
    std::memcpy(&bits_out, &*v, 8);
    EXPECT_EQ(bits_in, bits_out);
}

TEST(QA_GDB856_BoundaryValues, DoubleSmallestDenormal) {
    std::vector<uint8_t> buf;
    const double denormal = std::numeric_limits<double>::denorm_min();
    write_double(buf, denormal);
    auto r = make_reader(buf);
    auto v = r.read_double();
    ASSERT_TRUE(v.has_value());
    uint64_t bits_in = 0;
    uint64_t bits_out = 0;
    std::memcpy(&bits_in, &denormal, 8);
    std::memcpy(&bits_out, &*v, 8);
    EXPECT_EQ(bits_in, bits_out);
}

// ===========================================================================
// Suite: QA_GDB856_PeekAndSkip
// peek_u8/skip boundary and interleaving
// ===========================================================================

TEST(QA_GDB856_PeekAndSkip, PeekAtExactlyEnd) {
    std::vector<uint8_t> buf = {0x42};
    auto r = make_reader(buf);
    // Consume the single byte
    (void)r.read_u8();
    ASSERT_TRUE(r.at_end());
    // peek at end must fail
    auto res = r.peek_u8();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 1u); // peek doesn't advance
}

TEST(QA_GDB856_PeekAndSkip, PeekDoesNotAdvanceCursor) {
    std::vector<uint8_t> buf = {0xAB, 0xCD};
    auto r = make_reader(buf);
    for (int i = 0; i < 5; ++i) {
        auto p = r.peek_u8();
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(*p, 0xABu);
        EXPECT_EQ(r.pos, 0u); // cursor stays at 0 after every peek
    }
}

TEST(QA_GDB856_PeekAndSkip, SkipExactlyRemaining) {
    std::vector<uint8_t> buf = {1, 2, 3, 4};
    auto r = make_reader(buf);
    auto res = r.skip(4);
    ASSERT_TRUE(res.has_value());
    EXPECT_TRUE(r.at_end());
}

TEST(QA_GDB856_PeekAndSkip, SkipOneMoreThanRemaining) {
    std::vector<uint8_t> buf = {1, 2, 3};
    auto r = make_reader(buf);
    auto res = r.skip(4);
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u); // not advanced
}

TEST(QA_GDB856_PeekAndSkip, SkipMaxSizeT) {
    std::vector<uint8_t> buf = {0x01};
    auto r = make_reader(buf);
    auto res = r.skip(std::numeric_limits<size_t>::max());
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_PeekAndSkip, SkipZeroBytes) {
    // Skipping 0 bytes is always valid — even on an empty buffer.
    std::vector<uint8_t> buf;
    auto r = make_reader(buf);
    auto res = r.skip(0);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_PeekAndSkip, InterleavedPeekReadSkip) {
    // Build: [u8=0xAA] [u16=0xBEEF] [u8=0xCC]
    std::vector<uint8_t> buf;
    write_u8(buf, 0xAA);
    write_u16(buf, 0xBEEF);
    write_u8(buf, 0xCC);
    ASSERT_EQ(buf.size(), 4u);

    auto r = make_reader(buf);
    EXPECT_EQ(r.remaining(), 4u);

    // Peek first byte — should be 0xAA, cursor stays
    auto pk = r.peek_u8();
    ASSERT_TRUE(pk.has_value());
    EXPECT_EQ(*pk, 0xAAu);
    EXPECT_EQ(r.pos, 0u);

    // Read the u8
    auto v1 = r.read_u8();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 0xAAu);
    EXPECT_EQ(r.remaining(), 3u);

    // Skip 2 bytes (over the u16)
    ASSERT_TRUE(r.skip(2).has_value());
    EXPECT_EQ(r.remaining(), 1u);

    // Read final byte
    auto v4 = r.read_u8();
    ASSERT_TRUE(v4.has_value());
    EXPECT_EQ(*v4, 0xCCu);
    EXPECT_TRUE(r.at_end());

    // One more read must fail
    auto eof = r.read_u8();
    EXPECT_FALSE(eof.has_value());
    EXPECT_EQ(eof.error().code, StatusCode::IO_ERROR);
}

// ===========================================================================
// Suite: QA_GDB856_AdvanceSafety
// advance() is an unchecked internal helper used alongside ptr()/end_ptr().
// Even if advance() is called past end, remaining() and at_end() must not crash
// (they have a guard: pos < data.size() ? ... : 0).
// ===========================================================================

TEST(QA_GDB856_AdvanceSafety, AdvancePastEndRemainingReturnsZero) {
    std::vector<uint8_t> buf = {0x01, 0x02};
    auto r = make_reader(buf);
    // Unchecked advance past end — this is intentional internal use
    r.advance(100);
    EXPECT_EQ(r.remaining(), 0u); // must not return a huge wrapped value
    EXPECT_TRUE(r.at_end());
}

TEST(QA_GDB856_AdvanceSafety, AdvancePastEndThenReadReturnsError) {
    std::vector<uint8_t> buf = {0x01};
    auto r = make_reader(buf);
    r.advance(50); // unchecked
    auto res = r.read_u8();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, StatusCode::IO_ERROR);
}

// ===========================================================================
// Suite: QA_GDB856_SingleByteBuffer
// A 1-byte buffer — u8 succeeds; wider reads fail.
// ===========================================================================

TEST(QA_GDB856_SingleByteBuffer, U8Succeeds) {
    std::vector<uint8_t> buf = {0x5A};
    auto r = make_reader(buf);
    auto v = r.read_u8();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0x5Au);
    EXPECT_TRUE(r.at_end());
}

TEST(QA_GDB856_SingleByteBuffer, U16Fails) {
    std::vector<uint8_t> buf = {0x5A};
    auto r = make_reader(buf);
    EXPECT_FALSE(r.read_u16().has_value());
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_SingleByteBuffer, U32Fails) {
    std::vector<uint8_t> buf = {0x5A};
    auto r = make_reader(buf);
    EXPECT_FALSE(r.read_u32().has_value());
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_SingleByteBuffer, U64Fails) {
    std::vector<uint8_t> buf = {0x5A};
    auto r = make_reader(buf);
    EXPECT_FALSE(r.read_u64().has_value());
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_SingleByteBuffer, DoubleFails) {
    std::vector<uint8_t> buf = {0x5A};
    auto r = make_reader(buf);
    EXPECT_FALSE(r.read_double().has_value());
    EXPECT_EQ(r.pos, 0u);
}

TEST(QA_GDB856_SingleByteBuffer, ReadBytesOneFails) {
    // Only 1 byte in buffer; reading 2 must fail.
    std::vector<uint8_t> buf = {0x5A};
    auto r = make_reader(buf);
    EXPECT_FALSE(r.read_bytes(2).has_value());
    EXPECT_EQ(r.pos, 0u);
}

// ===========================================================================
// Suite: QA_GDB856_LittleEndian
// Confirm byte ordering is truly little-endian (LSB first).
// ===========================================================================

TEST(QA_GDB856_LittleEndian, U16Order) {
    // 0x0102 in LE: byte[0]=0x02, byte[1]=0x01
    std::vector<uint8_t> buf;
    write_u16(buf, 0x0102u);
    ASSERT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf[0], 0x02u);
    EXPECT_EQ(buf[1], 0x01u);
}

TEST(QA_GDB856_LittleEndian, U32Order) {
    // 0x01020304 in LE: byte[0]=0x04, byte[1]=0x03, byte[2]=0x02, byte[3]=0x01
    std::vector<uint8_t> buf;
    write_u32(buf, 0x01020304u);
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0x04u);
    EXPECT_EQ(buf[1], 0x03u);
    EXPECT_EQ(buf[2], 0x02u);
    EXPECT_EQ(buf[3], 0x01u);
}

TEST(QA_GDB856_LittleEndian, U64Order) {
    // 0x0102030405060708 in LE: byte[0]=0x08, ..., byte[7]=0x01
    std::vector<uint8_t> buf;
    write_u64(buf, 0x0102030405060708ULL);
    ASSERT_EQ(buf.size(), 8u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(buf[static_cast<size_t>(i)], static_cast<uint8_t>(8 - i))
            << "byte[" << i << "] mismatch";
    }
}

// ===========================================================================
// Suite: QA_GDB856_MultiFieldSequences
// Multi-field writes followed by sequential reads — verify accumulation and
// sequential cursor advancement produce the correct result.
// ===========================================================================

TEST(QA_GDB856_MultiFieldSequences, AllWidthsSequential) {
    std::vector<uint8_t> buf;
    write_u8(buf, 0x11u);
    write_u16(buf, 0x2222u);
    write_u32(buf, 0x33333333u);
    write_u64(buf, 0x4444444444444444ULL);

    auto r = make_reader(buf);
    EXPECT_EQ(r.remaining(), 15u);

    auto v1 = r.read_u8();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 0x11u);
    EXPECT_EQ(r.remaining(), 14u);

    auto v2 = r.read_u16();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 0x2222u);
    EXPECT_EQ(r.remaining(), 12u);

    auto v3 = r.read_u32();
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v3, 0x33333333u);
    EXPECT_EQ(r.remaining(), 8u);

    auto v4 = r.read_u64();
    ASSERT_TRUE(v4.has_value());
    EXPECT_EQ(*v4, 0x4444444444444444ULL);
    EXPECT_EQ(r.remaining(), 0u);
    EXPECT_TRUE(r.at_end());
}

TEST(QA_GDB856_MultiFieldSequences, SequenceWithBytes) {
    std::vector<uint8_t> buf;
    write_u32(buf, 5u); // length prefix
    // manually append 5 bytes
    buf.push_back('h');
    buf.push_back('e');
    buf.push_back('l');
    buf.push_back('l');
    buf.push_back('o');
    write_u8(buf, 0xFF); // sentinel after

    auto r = make_reader(buf);
    auto len_r = r.read_u32();
    ASSERT_TRUE(len_r.has_value());
    EXPECT_EQ(*len_r, 5u);

    auto str_r = r.read_bytes(*len_r);
    ASSERT_TRUE(str_r.has_value());
    EXPECT_EQ(*str_r, "hello");

    auto sentinel = r.read_u8();
    ASSERT_TRUE(sentinel.has_value());
    EXPECT_EQ(*sentinel, 0xFFu);
    EXPECT_TRUE(r.at_end());
}

TEST(QA_GDB856_MultiFieldSequences, PtrEndPtrConsistency) {
    // Verify ptr()/end_ptr() reflect the current cursor position.
    std::vector<uint8_t> buf;
    write_u32(buf, 0xAABBCCDDu);
    write_u32(buf, 0x11223344u);

    auto r = make_reader(buf);
    EXPECT_EQ(r.ptr(), buf.data());
    EXPECT_EQ(r.end_ptr(), buf.data() + buf.size());

    (void)r.read_u32(); // consume 4 bytes
    EXPECT_EQ(r.ptr(), buf.data() + 4);
    EXPECT_EQ(r.end_ptr(), buf.data() + 8);
    EXPECT_EQ(static_cast<size_t>(r.end_ptr() - r.ptr()), r.remaining());
}

// ===========================================================================
// Suite: QA_GDB856_EmptyBuffer
// All operations on a zero-length buffer return IO_ERROR.
// ===========================================================================

TEST(QA_GDB856_EmptyBuffer, AllReadsFailOnEmpty) {
    std::vector<uint8_t> buf;
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.read_u8().has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.read_u16().has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.read_u32().has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.read_u64().has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.read_double().has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.read_bytes(1).has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.peek_u8().has_value());
    }
    {
        auto r = make_reader(buf);
        EXPECT_FALSE(r.skip(1).has_value());
    }
}

TEST(QA_GDB856_EmptyBuffer, AtEndAndRemaining) {
    std::vector<uint8_t> buf;
    auto r = make_reader(buf);
    EXPECT_TRUE(r.at_end());
    EXPECT_EQ(r.remaining(), 0u);
}

// ===========================================================================
// Suite: QA_GDB856_ErrorMessageNonEmpty
// Error messages must be informative (non-empty) and mention the offset.
// ===========================================================================

TEST(QA_GDB856_ErrorMessageNonEmpty, U8ErrorHasMessage) {
    std::vector<uint8_t> buf;
    auto r = make_reader(buf);
    auto res = r.read_u8();
    ASSERT_FALSE(res.has_value());
    EXPECT_FALSE(res.error().message.empty());
    // Should contain "u8" and "offset"
    EXPECT_NE(res.error().message.find("u8"), std::string::npos);
    EXPECT_NE(res.error().message.find("0"), std::string::npos); // offset 0
}

TEST(QA_GDB856_ErrorMessageNonEmpty, BytesErrorIncludesCount) {
    // Error message for read_bytes should include the requested byte count.
    std::vector<uint8_t> buf = {0x01};
    auto r = make_reader(buf);
    auto res = r.read_bytes(42);
    ASSERT_FALSE(res.has_value());
    EXPECT_FALSE(res.error().message.empty());
    EXPECT_NE(res.error().message.find("42"), std::string::npos);
}

TEST(QA_GDB856_ErrorMessageNonEmpty, SkipErrorIncludesCount) {
    std::vector<uint8_t> buf = {0x01};
    auto r = make_reader(buf);
    auto res = r.skip(99);
    ASSERT_FALSE(res.has_value());
    EXPECT_FALSE(res.error().message.empty());
    EXPECT_NE(res.error().message.find("99"), std::string::npos);
}

// ===========================================================================
// Suite: QA_GDB856_WriterAppends
// Writer functions append to existing buffer content (do not overwrite).
// ===========================================================================

TEST(QA_GDB856_WriterAppends, AppendToNonEmpty) {
    std::vector<uint8_t> buf = {0xAA, 0xBB};
    write_u8(buf, 0xCC);
    ASSERT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 0xAAu);
    EXPECT_EQ(buf[1], 0xBBu);
    EXPECT_EQ(buf[2], 0xCCu);
}

TEST(QA_GDB856_WriterAppends, U32ThenU64Sequential) {
    std::vector<uint8_t> buf;
    write_u32(buf, 0x12345678u);
    write_u64(buf, 0xFEDCBA9876543210ULL);
    EXPECT_EQ(buf.size(), 12u);

    auto r = make_reader(buf);
    auto v1 = r.read_u32();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 0x12345678u);

    auto v2 = r.read_u64();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 0xFEDCBA9876543210ULL);
    EXPECT_TRUE(r.at_end());
}

// ===========================================================================
// Suite: QA_GDB856_LengthPrefixedKeySimulation
// Simulate the pattern used by btree/hash: a u32 length prefix followed by
// the key bytes. A hostile length prefix must be caught before allocation.
// ===========================================================================

TEST(QA_GDB856_LengthPrefixedKeySimulation, LargeCountButSmallBuffer) {
    // Simulate a corrupt btree leaf: key_count claims 65535 entries but
    // only a few bytes remain. The u16 read will succeed (reading the count),
    // then the first attempt to read a key will fail cleanly.
    std::vector<uint8_t> buf;
    write_u16(buf, 0xFFFF); // hostile key_count
    buf.push_back(0x01);    // only 1 stray byte follows (no valid key data)

    auto r = make_reader(buf);
    auto count_r = r.read_u16();
    ASSERT_TRUE(count_r.has_value());
    EXPECT_EQ(*count_r, 0xFFFFu);

    // Attempting to read a u32 (page_id in btree leaf) with only 1 byte left
    auto rid_r = r.read_u32();
    EXPECT_FALSE(rid_r.has_value());
    EXPECT_EQ(rid_r.error().code, StatusCode::IO_ERROR);
}

TEST(QA_GDB856_LengthPrefixedKeySimulation, ZeroKeyCount) {
    // key_count == 0: valid empty leaf, no key data consumed.
    std::vector<uint8_t> buf;
    write_u16(buf, 0u); // key_count = 0

    auto r = make_reader(buf);
    auto count_r = r.read_u16();
    ASSERT_TRUE(count_r.has_value());
    EXPECT_EQ(*count_r, 0u);
    EXPECT_TRUE(r.at_end());
}
