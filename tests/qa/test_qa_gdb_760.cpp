#include "sixseven/storage/serialization.h"

#include <gtest/gtest.h>

// GDB-760: Integer overflow in STRING/BLOB/JSON deserialize length check
// allows out-of-bounds read.
//
// Pre-fix: check_size(4 + len) where both 4 and len are uint32_t.  When
// len >= 0xFFFFFFFC the addition wraps to a small number, the check passes,
// and the subsequent string/blob construction reads far past the buffer.
//
// Fix: promote the addition to size_t via size_t{4} + len so wrap is
// impossible on any 64-bit platform.
//
// These tests craft malicious buffers and assert that deserialize() returns a
// clean INVALID_ARGUMENT error in every overflow scenario.

using namespace sixseven;

namespace {

// Build a raw deserialize input: null-flag 0x01, 4-byte LE length prefix,
// then 'payload_bytes' of filler bytes.
std::vector<uint8_t> make_var_buf(uint32_t len_prefix, size_t payload_bytes) {
    std::vector<uint8_t> buf;
    buf.push_back(0x01);
    buf.push_back(static_cast<uint8_t>(len_prefix & 0xFFU));
    buf.push_back(static_cast<uint8_t>((len_prefix >> 8U) & 0xFFU));
    buf.push_back(static_cast<uint8_t>((len_prefix >> 16U) & 0xFFU));
    buf.push_back(static_cast<uint8_t>((len_prefix >> 24U) & 0xFFU));
    for (size_t i = 0; i < payload_bytes; ++i) {
        buf.push_back(static_cast<uint8_t>(i & 0xFFU));
    }
    return buf;
}

} // namespace

// ---- STRING overflow cases --------------------------------------------------

// len = 0xFFFFFFFD: 4 + 0xFFFFFFFD wraps to 1 in uint32.
// A buffer with only 4 real bytes would pass the (broken) check but the
// fixed code must reject it with INVALID_ARGUMENT.
TEST(GDB760_Serialization, StringLen_0xFFFFFFFD_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFDU, 4);
    auto result = deserialize(buf, TypeId::STRING);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// len = 0xFFFFFFFC: 4 + 0xFFFFFFFC wraps to 0 in uint32 (worst case — would
// always pass check_size(0)).
TEST(GDB760_Serialization, StringLen_0xFFFFFFFC_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFCU, 4);
    auto result = deserialize(buf, TypeId::STRING);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// len = 0xFFFFFFFE: 4 + 0xFFFFFFFE wraps to 2 in uint32.
TEST(GDB760_Serialization, StringLen_0xFFFFFFFE_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFEU, 4);
    auto result = deserialize(buf, TypeId::STRING);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---- BLOB overflow cases ----------------------------------------------------

TEST(GDB760_Serialization, BlobLen_0xFFFFFFFD_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFDU, 4);
    auto result = deserialize(buf, TypeId::BLOB);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GDB760_Serialization, BlobLen_0xFFFFFFFC_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFCU, 4);
    auto result = deserialize(buf, TypeId::BLOB);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GDB760_Serialization, BlobLen_0xFFFFFFFE_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFEU, 4);
    auto result = deserialize(buf, TypeId::BLOB);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---- JSON overflow cases ----------------------------------------------------

TEST(GDB760_Serialization, JsonLen_0xFFFFFFFD_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFDU, 4);
    auto result = deserialize(buf, TypeId::JSON);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GDB760_Serialization, JsonLen_0xFFFFFFFC_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFCU, 4);
    auto result = deserialize(buf, TypeId::JSON);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GDB760_Serialization, JsonLen_0xFFFFFFFE_ReturnsError) {
    auto buf = make_var_buf(0xFFFFFFFEU, 4);
    auto result = deserialize(buf, TypeId::JSON);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---- Plain too-large (no overflow) ------------------------------------------
// len = buffer_remaining + 1: straightforward truncation, should also error.

TEST(GDB760_Serialization, StringPlainTooLarge_ReturnsError) {
    auto buf = make_var_buf(20U, 5); // claims 20 bytes, only 5 present
    auto result = deserialize(buf, TypeId::STRING);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GDB760_Serialization, BlobPlainTooLarge_ReturnsError) {
    auto buf = make_var_buf(20U, 5);
    auto result = deserialize(buf, TypeId::BLOB);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GDB760_Serialization, JsonPlainTooLarge_ReturnsError) {
    auto buf = make_var_buf(20U, 5);
    auto result = deserialize(buf, TypeId::JSON);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---- Exactly-fitting buffers still succeed ----------------------------------
// Regression: the fix must not break valid deserialization.

TEST(GDB760_Serialization, StringExactFit_Succeeds) {
    auto buf = make_var_buf(6U, 6); // "ABCDEF"
    buf[5] = 'A';
    buf[6] = 'B';
    buf[7] = 'C';
    buf[8] = 'D';
    buf[9] = 'E';
    buf[10] = 'F';
    auto result = deserialize(buf, TypeId::STRING);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string().size(), 6u);
    EXPECT_EQ(result->as_string(), "ABCDEF");
}

TEST(GDB760_Serialization, BlobExactFit_Succeeds) {
    auto buf = make_var_buf(3U, 3);
    buf[5] = 0xAA;
    buf[6] = 0xBB;
    buf[7] = 0xCC;
    auto result = deserialize(buf, TypeId::BLOB);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->as_blob().size(), 3u);
    EXPECT_EQ(result->as_blob()[0], 0xAAU);
    EXPECT_EQ(result->as_blob()[1], 0xBBU);
    EXPECT_EQ(result->as_blob()[2], 0xCCU);
}

TEST(GDB760_Serialization, JsonExactFit_Succeeds) {
    // Minimal JSON content: "{}" (2 bytes)
    auto buf = make_var_buf(2U, 2);
    buf[5] = '{';
    buf[6] = '}';
    auto result = deserialize(buf, TypeId::JSON);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "{}");
}

// ---- Zero-length buffers still succeed (empty string/blob/json) -------------

TEST(GDB760_Serialization, StringZeroLen_Succeeds) {
    auto buf = make_var_buf(0U, 0);
    auto result = deserialize(buf, TypeId::STRING);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "");
}

TEST(GDB760_Serialization, BlobZeroLen_Succeeds) {
    auto buf = make_var_buf(0U, 0);
    auto result = deserialize(buf, TypeId::BLOB);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_blob().empty());
}

TEST(GDB760_Serialization, JsonZeroLen_Succeeds) {
    auto buf = make_var_buf(0U, 0);
    auto result = deserialize(buf, TypeId::JSON);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_json().data, "");
}
