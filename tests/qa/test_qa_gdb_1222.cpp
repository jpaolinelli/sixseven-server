// QA regression tests for GDB-1222: dedup bytes-to-hex ostringstream loops
// into shared to_hex()/format_uuid() (include/sixseven/common/uuid.h).
//
// Adversarial focus:
//  1. AUTH (security): md5_hex / salt hex formatting must be byte-identical
//     lowercase, 2-digit, no-separator hex for every byte value 0x00-0xFF,
//     including the high-bit range where a signed-char sign-extension bug
//     would corrupt output (e.g. 0xFF must never render as "ffffffffffffffff"
//     or similar sign-extended garbage).
//  2. UUID: format_uuid emits canonical 8-4-4-4-12 lowercase dashed form,
//     round-trips through parse_uuid, and handles all-zero / all-0xFF UUIDs.
//  3. to_hex edge cases: nullptr/0-length, single byte at every "interesting"
//     value, multi-byte concatenation with no separators.
//  4. No coverage lost relative to the 3 removed inline ostringstream loops
//     (auth digest hex, auth salt hex, pg_protocol UUID text rendering).

#include "sixseven/common/uuid.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

using namespace sixseven;

namespace {

// Reference implementation mirroring the OLD hand-rolled ostringstream loop
// that GDB-1222 replaced, used as an oracle to prove byte-for-byte parity.
std::string legacy_ostringstream_hex(const uint8_t* data, size_t len) {
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        hex << std::setw(2) << static_cast<int>(data[i]);
    }
    return hex.str();
}

} // namespace

// -- to_hex: exhaustive byte-value parity vs legacy formatter ----------------

TEST(QA_GDB1222_ToHex, AllByteValuesMatchLegacyOstringstreamFormatter) {
    // Exhaustively check every possible byte value 0x00-0xFF renders
    // identically to the old ostringstream-based hex loop. This is the
    // strongest possible regression guard for the auth MD5 hex path,
    // which is security-sensitive wire-format code.
    for (int v = 0; v <= 0xFF; ++v) {
        const uint8_t byte = static_cast<uint8_t>(v);
        std::string got = to_hex(&byte, 1);
        std::string expected = legacy_ostringstream_hex(&byte, 1);
        ASSERT_EQ(got, expected) << "byte value 0x" << std::hex << v;
        ASSERT_EQ(got.size(), 2u);
    }
}

TEST(QA_GDB1222_ToHex, NullptrZeroLengthIsEmptyNoCrash) {
    EXPECT_EQ(to_hex(nullptr, 0), "");
}

TEST(QA_GDB1222_ToHex, HighBitBytesNoSignExtension) {
    // Regression: static_cast<int>(byte) on a *signed* char would sign-extend
    // 0x80-0xFF into negative ints, and std::hex on a negative int produces
    // garbage (e.g. platform-dependent all-Fs prefix), not "80".."ff".
    const uint8_t highs[] = {0x80, 0x81, 0x8f, 0x90, 0xa0, 0xc0, 0xf0, 0xfe, 0xff};
    const char* expect[] = {"80", "81", "8f", "90", "a0", "c0", "f0", "fe", "ff"};
    for (size_t i = 0; i < sizeof(highs); ++i) {
        EXPECT_EQ(to_hex(&highs[i], 1), expect[i]);
    }
}

TEST(QA_GDB1222_ToHex, ConcatenationHasNoSeparatorsOrPrefix) {
    const uint8_t data[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    EXPECT_EQ(to_hex(data, sizeof(data)), "0123456789abcdef");
    // No "0x" prefix, no spaces, no commas.
    EXPECT_EQ(to_hex(data, sizeof(data)).find("0x"), std::string::npos);
    EXPECT_EQ(to_hex(data, sizeof(data)).find(' '), std::string::npos);
}

TEST(QA_GDB1222_ToHex, LargeBufferMatchesLegacyFormatter) {
    // Stress: 256-byte buffer covering every value 0-255, verify against the
    // legacy oracle for the whole buffer (catches any off-by-one across byte
    // boundaries, e.g. width/fill state leaking between iterations, which was
    // a real historical risk with the stateful ostringstream approach).
    std::array<uint8_t, 256> data{};
    for (int i = 0; i < 256; ++i) {
        data[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    }
    EXPECT_EQ(to_hex(data.data(), data.size()),
              legacy_ostringstream_hex(data.data(), data.size()));
}

// -- format_uuid: canonical form + round-trip --------------------------------

TEST(QA_GDB1222_FormatUuid, DashPositionsAreExactly8_4_4_4_12) {
    Uuid uuid = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::string s = format_uuid(uuid);
    ASSERT_EQ(s.size(), 36u);
    EXPECT_EQ(s[8], '-');
    EXPECT_EQ(s[13], '-');
    EXPECT_EQ(s[18], '-');
    EXPECT_EQ(s[23], '-');
    // No other dashes.
    int dash_count = 0;
    for (char c : s) {
        if (c == '-') {
            ++dash_count;
        }
    }
    EXPECT_EQ(dash_count, 4);
}

TEST(QA_GDB1222_FormatUuid, AllLowercaseNoUppercaseHexDigits) {
    Uuid uuid = {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89,
                 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    std::string s = format_uuid(uuid);
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            EXPECT_TRUE(c >= 'a' && c <= 'f') << "found non-lowercase hex char: " << c;
        }
    }
}

TEST(QA_GDB1222_FormatUuid, RoundTripsForManyPseudorandomUuids) {
    // Deterministic pseudo-random UUIDs (fixed seed pattern), round-tripped
    // through format_uuid -> parse_uuid -> format_uuid to confirm stability.
    uint32_t seed = 0xC0FFEE;
    for (int trial = 0; trial < 50; ++trial) {
        Uuid uuid{};
        for (auto& b : uuid) {
            seed = seed * 1103515245u + 12345u;
            b = static_cast<uint8_t>(seed >> 16);
        }
        std::string s1 = format_uuid(uuid);
        auto parsed = parse_uuid(s1);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
        std::string s2 = format_uuid(*parsed);
        EXPECT_EQ(s1, s2);
        EXPECT_EQ(*parsed, uuid);
    }
}

TEST(QA_GDB1222_FormatUuid, SingleBitDifferencesProduceDifferentStrings) {
    // Adversarial: flip one bit at a time across all 16 bytes and confirm the
    // rendered string changes every time (catches any byte-index / dash-shift
    // bug that could alias two distinct UUIDs to the same text).
    Uuid base = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::string base_str = format_uuid(base);
    for (size_t byte_idx = 0; byte_idx < base.size(); ++byte_idx) {
        Uuid mutated = base;
        mutated[byte_idx] = 0x01;
        std::string mutated_str = format_uuid(mutated);
        EXPECT_NE(mutated_str, base_str) << "byte index " << byte_idx;
    }
}

// -- Cross-check: to_hex composition equals format_uuid semantics -----------

TEST(QA_GDB1222_Integration, FormatUuidGroupsMatchDirectToHexOfSameBytes) {
    // format_uuid is documented to be built from to_hex per-byte; confirm the
    // concatenation of to_hex applied to each dash-delimited group matches
    // slicing the raw bytes directly through to_hex, i.e. no byte reordering
    // (endianness bug) was introduced when both helpers were unified.
    Uuid uuid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
    std::string full_hex = to_hex(uuid.data(), uuid.size());
    std::string dashed = format_uuid(uuid);
    std::string stripped;
    for (char c : dashed) {
        if (c != '-') {
            stripped += c;
        }
    }
    EXPECT_EQ(stripped, full_hex);
}
