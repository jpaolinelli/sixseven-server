/// QA tests for GDB-959: tests/e2e CTest tier wire-up.
///
/// These tests verify the verifiable aspects of the e2e tier on Windows:
///   - The pg-wire framing logic (decode_one_message) used by
///     wait_ready_for_query correctly identifies ReadyForQuery frames and
///     is not fooled by 'Z' (0x5A) appearing as a data byte inside other
///     message payloads.
///   - Partial buffer handling returns NOT_FOUND (need-more-data), not a
///     hard error or crash.
///   - Empty buffer returns NOT_FOUND without undefined behaviour.
///   - The encode helpers produce correctly structured messages.
///
/// Live server scenarios (kill -9 durability, two-process replication) are
/// POSIX/CI only and are covered by the e2e test binaries which GTEST_SKIP
/// on _WIN32.

#include "sixseven/cli/pg_wire_codec.h"
#include "sixseven/common/status.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace sixseven::cli;
using sixseven::StatusCode;

namespace {

// ---------------------------------------------------------------------------
// Helper: build a well-formed pg-wire server message byte sequence.
// Format: 1 type byte + 4-byte big-endian length (includes the 4 bytes,
//         NOT the type byte) + payload.
// ---------------------------------------------------------------------------
std::vector<uint8_t> make_pg_msg(uint8_t type, const std::vector<uint8_t>& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size() + 4);
    std::vector<uint8_t> msg;
    msg.push_back(type);
    msg.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(len & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

// Build a ReadyForQuery message with transaction_status 'I'.
// Payload: 1 byte transaction status.
std::vector<uint8_t> make_ready_for_query(char tx_status = 'I') {
    return make_pg_msg('Z', {static_cast<uint8_t>(tx_status)});
}

// Build a DataRow with a single text field whose content is arbitrary bytes.
// Format: int16 field count + per-field int32 length + bytes.
std::vector<uint8_t> make_data_row(const std::vector<uint8_t>& field_data) {
    std::vector<uint8_t> payload;
    // field count = 1 (big-endian int16)
    payload.push_back(0x00);
    payload.push_back(0x01);
    // field length (big-endian int32)
    uint32_t flen = static_cast<uint32_t>(field_data.size());
    payload.push_back(static_cast<uint8_t>((flen >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((flen >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((flen >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(flen & 0xFF));
    payload.insert(payload.end(), field_data.begin(), field_data.end());
    return make_pg_msg('D', payload);
}

// Build a CommandComplete with a given tag string (null-terminated in payload).
std::vector<uint8_t> make_command_complete(const std::string& tag) {
    std::vector<uint8_t> payload(tag.begin(), tag.end());
    payload.push_back(0x00); // null terminator
    return make_pg_msg('C', payload);
}

} // namespace

// ===========================================================================
// Suite: QA_GDB959_FramingLogic
// Tests the pg-wire framing used by wait_ready_for_query.
// ===========================================================================

TEST(QA_GDB959_FramingLogic, EmptyBufferReturnsNotFound) {
    // An empty buffer is always a partial message (NOT_FOUND), not a crash.
    std::vector<uint8_t> empty;
    size_t consumed = 0;
    auto result = decode_one_message(empty, consumed);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_EQ(consumed, 0u) << "consumed must be 0 on NOT_FOUND";
}

TEST(QA_GDB959_FramingLogic, TypeByteOnlyIsPartial) {
    // 1 byte (type only) -- no length present yet, must be NOT_FOUND.
    std::vector<uint8_t> buf = {'Z'};
    size_t consumed = 0;
    auto result = decode_one_message(buf, consumed);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_EQ(consumed, 0u);
}

TEST(QA_GDB959_FramingLogic, PartialHeaderIsNotFound) {
    // 3 bytes: type + 2 of 4 length bytes -- NOT_FOUND, not crash.
    std::vector<uint8_t> buf = {'Z', 0x00, 0x00};
    size_t consumed = 0;
    auto result = decode_one_message(buf, consumed);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_EQ(consumed, 0u);
}

TEST(QA_GDB959_FramingLogic, CompleteHeaderMissingPayloadIsNotFound) {
    // ReadyForQuery: type 'Z', length=5 (4 hdr + 1 payload byte), but no payload.
    // Buffer has type + 4-byte length only (5 bytes total), missing the 1 payload byte.
    std::vector<uint8_t> buf = {'Z', 0x00, 0x00, 0x00, 0x05};
    size_t consumed = 0;
    auto result = decode_one_message(buf, consumed);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_EQ(consumed, 0u);
}

TEST(QA_GDB959_FramingLogic, WellFormedReadyForQueryIsDecoded) {
    // A complete, valid ReadyForQuery frame.
    auto buf = make_ready_for_query('I');
    size_t consumed = 0;
    auto result = decode_one_message(buf, consumed);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->tag, ServerMsgTag::ReadyForQuery);
    EXPECT_EQ(result->ready.transaction_status, 'I');
    EXPECT_EQ(consumed, buf.size());
}

TEST(QA_GDB959_FramingLogic, ZByteInsideDataRowPayloadNotMistakenForRFQ) {
    // KEY FRAMING FIX VERIFICATION (v1 bug):
    // A DataRow whose field content contains 0x5A ('Z') must NOT cause
    // wait_ready_for_query to false-positive return true.  The framing walk
    // must use the length prefix, not a raw byte scan for 'Z'.
    //
    // We build: DataRow containing the byte 0x5A followed by ReadyForQuery.
    // Decoding the DataRow first must yield DataRow (not ReadyForQuery),
    // and the second message must yield ReadyForQuery.

    std::vector<uint8_t> z_payload = {0x5A, 0x5A, 0x5A}; // three 'Z' bytes in field data
    auto data_row_msg = make_data_row(z_payload);
    auto rfq_msg = make_ready_for_query('I');

    // Concatenate both messages.
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), data_row_msg.begin(), data_row_msg.end());
    buf.insert(buf.end(), rfq_msg.begin(), rfq_msg.end());

    // First decode: must be DataRow, NOT ReadyForQuery.
    size_t consumed1 = 0;
    auto msg1 = decode_one_message(buf, consumed1);
    ASSERT_TRUE(msg1.has_value()) << msg1.error().message;
    EXPECT_EQ(msg1->tag, ServerMsgTag::DataRow)
        << "'Z' inside DataRow payload must not be decoded as ReadyForQuery";
    EXPECT_GT(consumed1, 0u);

    // Second decode: must be ReadyForQuery.
    std::vector<uint8_t> remaining(buf.begin() + static_cast<ptrdiff_t>(consumed1), buf.end());
    size_t consumed2 = 0;
    auto msg2 = decode_one_message(remaining, consumed2);
    ASSERT_TRUE(msg2.has_value()) << msg2.error().message;
    EXPECT_EQ(msg2->tag, ServerMsgTag::ReadyForQuery);
    EXPECT_EQ(consumed2, rfq_msg.size());
}

TEST(QA_GDB959_FramingLogic, ZByteInsideCommandCompleteNotMistakenForRFQ) {
    // CommandComplete tag "SELECT 0 5A" contains 'Z' chars in the text.
    // Must not false-positive as ReadyForQuery.
    auto cc_msg = make_command_complete("SELECT\x5A\x5A");
    auto rfq_msg = make_ready_for_query('I');

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), cc_msg.begin(), cc_msg.end());
    buf.insert(buf.end(), rfq_msg.begin(), rfq_msg.end());

    size_t consumed1 = 0;
    auto msg1 = decode_one_message(buf, consumed1);
    ASSERT_TRUE(msg1.has_value()) << msg1.error().message;
    EXPECT_EQ(msg1->tag, ServerMsgTag::CommandComplete)
        << "'Z' in CommandComplete payload must not decode as ReadyForQuery";

    std::vector<uint8_t> remaining(buf.begin() + static_cast<ptrdiff_t>(consumed1), buf.end());
    size_t consumed2 = 0;
    auto msg2 = decode_one_message(remaining, consumed2);
    ASSERT_TRUE(msg2.has_value()) << msg2.error().message;
    EXPECT_EQ(msg2->tag, ServerMsgTag::ReadyForQuery);
}

TEST(QA_GDB959_FramingLogic, MultipleMessagesConsumedSequentially) {
    // Simulate a realistic server response: RowDescription + DataRow + CommandComplete + RFQ.
    // Verify that sequential decode_one_message calls walk the buffer correctly
    // and that the final message is ReadyForQuery.
    auto rfq_msg = make_ready_for_query('I');
    auto cc_msg = make_command_complete("SELECT 1");
    auto dr_msg = make_data_row({'h', 'e', 'l', 'l', 'o'});

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), dr_msg.begin(), dr_msg.end());
    buf.insert(buf.end(), cc_msg.begin(), cc_msg.end());
    buf.insert(buf.end(), rfq_msg.begin(), rfq_msg.end());

    std::vector<ServerMsgTag> tags;
    size_t offset = 0;
    for (int i = 0; i < 10; ++i) { // bounded iteration, must not spin
        std::vector<uint8_t> slice(buf.begin() + static_cast<ptrdiff_t>(offset), buf.end());
        size_t consumed = 0;
        auto msg = decode_one_message(slice, consumed);
        if (!msg.has_value())
            break; // NOT_FOUND = no more complete messages
        tags.push_back(msg->tag);
        offset += consumed;
        if (msg->tag == ServerMsgTag::ReadyForQuery)
            break;
    }

    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0], ServerMsgTag::DataRow);
    EXPECT_EQ(tags[1], ServerMsgTag::CommandComplete);
    EXPECT_EQ(tags[2], ServerMsgTag::ReadyForQuery);
    EXPECT_EQ(offset, buf.size()) << "all bytes consumed";
}

// ===========================================================================
// Suite: QA_GDB959_EncodeHelpers
// Tests that client-side encode functions produce correctly structured output.
// ===========================================================================

TEST(QA_GDB959_EncodeHelpers, EncodeQueryMessageFormat) {
    // Query message: 'Q' + 4-byte big-endian length (4 + len(sql) + 1 nul) + sql + nul
    auto msg = encode_query_message("SELECT 1");
    ASSERT_GE(msg.size(), 6u);
    EXPECT_EQ(msg[0], 'Q');
    // Length field: big-endian uint32 at bytes [1..4].
    uint32_t len = (static_cast<uint32_t>(msg[1]) << 24) | (static_cast<uint32_t>(msg[2]) << 16) |
                   (static_cast<uint32_t>(msg[3]) << 8) | static_cast<uint32_t>(msg[4]);
    // Should be 4 (hdr) + 8 (sql) + 1 (nul) = 13
    EXPECT_EQ(len, 13u);
    EXPECT_EQ(msg.size(), static_cast<size_t>(1 + len));
    // Last byte must be null terminator.
    EXPECT_EQ(msg.back(), 0x00);
}

TEST(QA_GDB959_EncodeHelpers, EncodeQueryMessageEmptySql) {
    // Empty SQL is an edge case -- should still produce a valid 'Q' frame.
    auto msg = encode_query_message("");
    ASSERT_GE(msg.size(), 6u);
    EXPECT_EQ(msg[0], 'Q');
    EXPECT_EQ(msg.back(), 0x00); // null terminator present
}

TEST(QA_GDB959_EncodeHelpers, EncodeStartupMessageContainsUserAndDb) {
    // StartupMessage: no type byte; starts with 4-byte total length.
    auto msg = encode_startup_message("testuser", "testdb");
    ASSERT_GE(msg.size(), 8u);
    // First 4 bytes are total length (big-endian).
    uint32_t total_len = (static_cast<uint32_t>(msg[0]) << 24) |
                         (static_cast<uint32_t>(msg[1]) << 16) |
                         (static_cast<uint32_t>(msg[2]) << 8) | static_cast<uint32_t>(msg[3]);
    EXPECT_EQ(total_len, msg.size()) << "startup total_len must equal actual byte count";
    // Protocol version 3.0 = 196608 = 0x00030000 at bytes [4..7].
    EXPECT_EQ(msg[4], 0x00);
    EXPECT_EQ(msg[5], 0x03);
    EXPECT_EQ(msg[6], 0x00);
    EXPECT_EQ(msg[7], 0x00);
    // Somewhere in the payload the user and database strings must appear.
    std::string content(msg.begin() + 8, msg.end());
    EXPECT_NE(content.find("testuser"), std::string::npos) << "user not found in startup";
    EXPECT_NE(content.find("testdb"), std::string::npos) << "database not found in startup";
}

TEST(QA_GDB959_EncodeHelpers, EncodeTerminateMessage) {
    // Terminate: 'X' + int32 len=4 (no payload).
    auto msg = encode_terminate_message();
    ASSERT_EQ(msg.size(), 5u);
    EXPECT_EQ(msg[0], 'X');
    uint32_t len = (static_cast<uint32_t>(msg[1]) << 24) | (static_cast<uint32_t>(msg[2]) << 16) |
                   (static_cast<uint32_t>(msg[3]) << 8) | static_cast<uint32_t>(msg[4]);
    EXPECT_EQ(len, 4u);
}

// ===========================================================================
// Suite: QA_GDB959_BuildSystem
// Compile-time / structural assertions about the e2e tier wiring.
// ===========================================================================

TEST(QA_GDB959_BuildSystem, SIXSEVENServerBinaryMacroIsDefined) {
    // The CMakeLists.txt must inject SIXSEVEN_SERVER_BINARY.
    // If it is missing, the e2e tests would fail to compile.
    // This test simply verifies the macro is non-empty from this binary's POV;
    // the qa binary does not define it, so we check it is empty here (i.e.
    // the macro is test-binary-scoped, not leaked into qa tests).
    //
    // We cannot directly check the e2e binary's compile definitions from here,
    // but we can assert our (qa) binary does NOT accidentally define it,
    // confirming the definition is scoped to sixseven_e2e_tests only.
#ifdef SIXSEVEN_SERVER_BINARY
    // If we reach here in the QA binary, the macro leaked -- that is a CMake bug.
    FAIL() << "SIXSEVEN_SERVER_BINARY leaked into sixseven_qa_tests; "
              "it should be private to sixseven_e2e_tests";
#else
    SUCCEED();
#endif
}
