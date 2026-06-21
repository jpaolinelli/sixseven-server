// QA tests for GDB-867: PG v3 wire-protocol test helpers extracted to
// tests/unit/pg_wire_test_helpers.h.
//
// These are MUTATION-GRADE exact-byte assertions hand-derived from the
// PostgreSQL v3 frontend/backend protocol specification.  Every expected byte
// vector is constructed independently of the implementation — calling the
// function to compute the expected value would be a tautology.
//
// Reference: https://www.postgresql.org/docs/current/protocol-message-formats.html

#include <gtest/gtest.h>

#include "pg_wire_test_helpers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Helpers for nicer EXPECT output
// ---------------------------------------------------------------------------
static std::string hex_dump(const std::vector<uint8_t>& v) {
    std::string out;
    out.reserve(v.size() * 3);
    for (auto b : v) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x ", b);
        out += buf;
    }
    return out;
}

#define ASSERT_BYTES_EQ(actual, expected)                                                          \
    ASSERT_EQ((actual), (expected)) << "actual : " << hex_dump(actual)                            \
                                    << "\nexpected: " << hex_dump(expected)

// ---------------------------------------------------------------------------
// Suite: QA_GDB867_PgWireHelpers — exact-byte assertions
// ---------------------------------------------------------------------------

// build_startup_message
// -----------------------------------------------------------------------
// PG v3 StartupMessage:
//   [int32 total_length] [int32 protocol_version] [key\0 val\0 ...] [0x00]
//
// Hand-derivation for params = {{"user","test"}}:
//   version   = 0x00030000 (196608)
//   payload   = [0x00,0x03,0x00,0x00, 'u','s','e','r',0,'t','e','s','t',0, 0]
//             =  4 bytes version + 4+1 key + 4+1 val + 1 term = 15 bytes
//   total_len = 4 (length field) + 15 = 19 = 0x00000013
//   msg       = [0x00,0x00,0x00,0x13] ++ payload  (19 bytes total)

TEST(QA_GDB867_PgWireHelpers, StartupMessageExactBytes) {
    auto msg = pg_wire_test::build_startup_message({{"user", "test"}});

    // clang-format off
    const std::vector<uint8_t> expected = {
        // length = 19 (includes itself)
        0x00, 0x00, 0x00, 0x13,
        // protocol 3.0 = 0x00030000
        0x00, 0x03, 0x00, 0x00,
        // "user\0"
        'u', 's', 'e', 'r', 0x00,
        // "test\0"
        't', 'e', 's', 't', 0x00,
        // terminator
        0x00
    };
    // clang-format on

    ASSERT_BYTES_EQ(msg, expected);
}

// Mutation guard: if the length field were computed excluding itself the value
// would be 15 (0x0F), not 19 (0x13).  This test catches that regression.
TEST(QA_GDB867_PgWireHelpers, StartupMessageLengthIncludesItself) {
    auto msg = pg_wire_test::build_startup_message({{"user", "test"}});
    ASSERT_GE(msg.size(), 4u);
    uint32_t encoded_len = (static_cast<uint32_t>(msg[0]) << 24) |
                           (static_cast<uint32_t>(msg[1]) << 16) |
                           (static_cast<uint32_t>(msg[2]) << 8) |
                           static_cast<uint32_t>(msg[3]);
    // Length field must equal total message size (i.e. it includes itself).
    EXPECT_EQ(encoded_len, static_cast<uint32_t>(msg.size()));
}

// Empty params: just version + double-null terminator.
// payload = [0x00,0x03,0x00,0x00, 0x00]  (5 bytes)
// total_len = 4 + 5 = 9 = 0x09
TEST(QA_GDB867_PgWireHelpers, StartupMessageEmptyParams) {
    auto msg = pg_wire_test::build_startup_message({});
    const std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x09,
                                           0x00, 0x03, 0x00, 0x00,
                                           0x00};
    ASSERT_BYTES_EQ(msg, expected);
}

// build_parse_message
// -----------------------------------------------------------------------
// PG v3 Parse ('P'):
//   'P' [int32 msg_len] [stmt_name\0] [query\0] [int16 num_params]
//   msg_len includes itself.
//
// Hand-derivation: stmt="", sql="SELECT 1", 0 params:
//   body = [0x00, 'S','E','L','E','C','T',' ','1',0x00, 0x00,0x00]
//          = 1 (stmt null) + 9 (sql+null) + 2 (param count) = 12 bytes
//   body_len = 4 + 12 = 16 = 0x10
//   msg = ['P', 0x00,0x00,0x00,0x10, 0x00, 'S','E','L','E','C','T',' ','1',0x00, 0x00,0x00]

TEST(QA_GDB867_PgWireHelpers, ParseMessageExactBytes) {
    auto msg = pg_wire_test::build_parse_message("", "SELECT 1");

    // clang-format off
    const std::vector<uint8_t> expected = {
        // type tag
        'P',
        // msg_len = 16 (includes itself)
        0x00, 0x00, 0x00, 0x10,
        // stmt_name = "" + null
        0x00,
        // sql = "SELECT 1" + null
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0x00,
        // num_params = 0
        0x00, 0x00
    };
    // clang-format on

    ASSERT_BYTES_EQ(msg, expected);
}

// Mutation guard: if body_len excluded itself it would be 12 (0x0C).
TEST(QA_GDB867_PgWireHelpers, ParseMessageLengthIncludesItself) {
    auto msg = pg_wire_test::build_parse_message("stmt1", "SELECT $1");
    ASSERT_GE(msg.size(), 5u);
    EXPECT_EQ(msg[0], static_cast<uint8_t>('P'));
    uint32_t encoded_len = (static_cast<uint32_t>(msg[1]) << 24) |
                           (static_cast<uint32_t>(msg[2]) << 16) |
                           (static_cast<uint32_t>(msg[3]) << 8) |
                           static_cast<uint32_t>(msg[4]);
    // body_len = 4 + body_size; total msg = 1 + body_len = 1 + encoded_len
    EXPECT_EQ(encoded_len, static_cast<uint32_t>(msg.size() - 1));
}

// build_bind_message (NON-NULL)
// -----------------------------------------------------------------------
// PG v3 Bind ('B'):
//   'B' [int32 msg_len]
//   [portal_name\0] [stmt_name\0]
//   [int16 num_format_codes]  (0 = all text)
//   [int16 num_params]
//   for each param: [int32 value_len] [value_bytes]   (NO null terminator)
//   [int16 num_result_format_codes]  (0 = all text)
//
// Hand-derivation: portal="", stmt="", params={"42"}:
//   body = [0x00, 0x00,         // portal\0 stmt\0
//            0x00,0x00,         // 0 format codes
//            0x00,0x01,         // 1 param
//            0x00,0x00,0x00,0x02, '4','2',  // len=2, value bytes (NO \0!)
//            0x00,0x00]         // 0 result format codes
//          = 2+2+2+6+2 = 14 bytes
//   body_len = 4+14 = 18 = 0x12
//   msg = ['B', 0x00,0x00,0x00,0x12, ...14 body bytes...]

TEST(QA_GDB867_PgWireHelpers, BindMessageNonNullExactBytes) {
    auto msg = pg_wire_test::build_bind_message("", "", {std::string("42")});

    // clang-format off
    const std::vector<uint8_t> expected = {
        // type tag
        'B',
        // msg_len = 18
        0x00, 0x00, 0x00, 0x12,
        // portal_name = "" + null
        0x00,
        // stmt_name = "" + null
        0x00,
        // 0 parameter format codes
        0x00, 0x00,
        // 1 parameter
        0x00, 0x01,
        // param: len=2 then bytes '4','2'  (NO null terminator)
        0x00, 0x00, 0x00, 0x02,
        '4', '2',
        // 0 result format codes
        0x00, 0x00
    };
    // clang-format on

    ASSERT_BYTES_EQ(msg, expected);
}

// THE CRITICAL NULL SENTINEL TEST
// -----------------------------------------------------------------------
// When a param is std::nullopt, the wire format must be 0xFFFFFFFF (-1 as
// int32) and NO value bytes follow.  A stray null terminator or wrong sentinel
// (e.g. 0x00000000) must fail this test.

TEST(QA_GDB867_PgWireHelpers, BindMessageNullParamExactBytes) {
    auto msg = pg_wire_test::build_bind_message("", "", {std::nullopt});

    // Hand-derivation: portal="", stmt="", params={nullopt}:
    //   body = [0x00, 0x00,          // portal\0 stmt\0
    //            0x00,0x00,          // 0 format codes
    //            0x00,0x01,          // 1 param
    //            0xFF,0xFF,0xFF,0xFF, // NULL sentinel (-1), NO value bytes
    //            0x00,0x00]          // 0 result format codes
    //          = 2+2+2+4+2 = 12 bytes
    //   body_len = 4+12 = 16 = 0x10

    // clang-format off
    const std::vector<uint8_t> expected = {
        'B',
        0x00, 0x00, 0x00, 0x10,
        0x00,        // portal ""
        0x00,        // stmt ""
        0x00, 0x00,  // 0 format codes
        0x00, 0x01,  // 1 param
        0xFF, 0xFF, 0xFF, 0xFF,  // NULL sentinel
        0x00, 0x00   // 0 result format codes
    };
    // clang-format on

    ASSERT_BYTES_EQ(msg, expected);

    // Extra guard: null sentinel must be EXACTLY 4 bytes with no trailing value.
    // Find the sentinel position (after 'B' + 4-byte len + portal\0 + stmt\0 +
    // format_codes(2) + param_count(2) = 1+4+1+1+2+2 = 11).
    ASSERT_GE(msg.size(), 15u);
    EXPECT_EQ(msg[11], 0xFF);
    EXPECT_EQ(msg[12], 0xFF);
    EXPECT_EQ(msg[13], 0xFF);
    EXPECT_EQ(msg[14], 0xFF);
    // No extra bytes between sentinel and the result-format-code field.
    ASSERT_EQ(msg.size(), 17u) << "NULL param must not append any value bytes";
}

// Mutation guard: non-null "42" must NOT use 0xFFFFFFFF sentinel.
TEST(QA_GDB867_PgWireHelpers, BindMessageNonNullDoesNotUseSentinel) {
    auto msg = pg_wire_test::build_bind_message("", "", {std::string("42")});
    // len field for "42" is at offset 11
    ASSERT_GE(msg.size(), 15u);
    EXPECT_NE(msg[11], 0xFF) << "non-null param must not use 0xFF sentinel";
    EXPECT_EQ(msg[11], 0x00);
    EXPECT_EQ(msg[12], 0x00);
    EXPECT_EQ(msg[13], 0x00);
    EXPECT_EQ(msg[14], 0x02);
}

// Non-null value must have no null terminator after the value bytes.
TEST(QA_GDB867_PgWireHelpers, BindMessageNonNullNoNullTerminator) {
    auto msg = pg_wire_test::build_bind_message("", "", {std::string("42")});
    // Offset of result-format-codes = 1+4+1+1+2+2+4+2 = 17
    ASSERT_EQ(msg.size(), 19u);
    // bytes 15-16 = '4','2';  bytes 17-18 = [0x00,0x00] result format codes.
    EXPECT_EQ(msg[15], static_cast<uint8_t>('4'));
    EXPECT_EQ(msg[16], static_cast<uint8_t>('2'));
    // If there were a stray null terminator it would push result codes to 19-20.
    EXPECT_EQ(msg[17], 0x00);
    EXPECT_EQ(msg[18], 0x00);
}

// Mixed: first param non-null, second param null.
TEST(QA_GDB867_PgWireHelpers, BindMessageMixedNullAndNonNull) {
    auto msg = pg_wire_test::build_bind_message("", "", {std::string("X"), std::nullopt});

    // body:
    //  portal\0 stmt\0 = 2
    //  format codes (0) = 2
    //  num_params (2) = 2
    //  param0: len=1, 'X' = 5
    //  param1: 0xFFFFFFFF = 4
    //  result format codes (0) = 2
    //  total body = 17
    //  body_len = 21 = 0x15
    // clang-format off
    const std::vector<uint8_t> expected = {
        'B',
        0x00, 0x00, 0x00, 0x15,
        0x00, 0x00,        // portal, stmt
        0x00, 0x00,        // 0 format codes
        0x00, 0x02,        // 2 params
        0x00, 0x00, 0x00, 0x01, 'X',         // "X"
        0xFF, 0xFF, 0xFF, 0xFF,              // NULL
        0x00, 0x00                           // result format codes
    };
    // clang-format on

    ASSERT_BYTES_EQ(msg, expected);
}

// build_execute_message
// -----------------------------------------------------------------------
// PG v3 Execute ('E'):
//   'E' [int32 msg_len] [portal_name\0] [int32 max_rows]
//   msg_len includes itself.
//
// Hand-derivation: portal="", max_rows=0:
//   body = [0x00, 0x00,0x00,0x00,0x00]  = 1+4 = 5 bytes
//   body_len = 4+5 = 9 = 0x09
//   msg = ['E', 0x00,0x00,0x00,0x09, 0x00, 0x00,0x00,0x00,0x00]

TEST(QA_GDB867_PgWireHelpers, ExecuteMessageExactBytes) {
    auto msg = pg_wire_test::build_execute_message("", 0);

    const std::vector<uint8_t> expected = {
        'E',
        0x00, 0x00, 0x00, 0x09,
        0x00,                         // portal "" + null
        0x00, 0x00, 0x00, 0x00        // max_rows = 0
    };

    ASSERT_BYTES_EQ(msg, expected);
}

// Non-zero max_rows.
TEST(QA_GDB867_PgWireHelpers, ExecuteMessageNonZeroMaxRows) {
    auto msg = pg_wire_test::build_execute_message("", 100);
    ASSERT_EQ(msg.size(), 10u);
    // max_rows at offset 6-9 = 0x00000064
    EXPECT_EQ(msg[6], 0x00);
    EXPECT_EQ(msg[7], 0x00);
    EXPECT_EQ(msg[8], 0x00);
    EXPECT_EQ(msg[9], 0x64);
}

// build_sync_message
// -----------------------------------------------------------------------
// PG v3 Sync ('S'):
//   'S' [int32 msg_len]  where msg_len = 4 (length field only, no body)
//
// Expected exactly: ['S', 0x00, 0x00, 0x00, 0x04]

TEST(QA_GDB867_PgWireHelpers, SyncMessageExactBytes) {
    auto msg = pg_wire_test::build_sync_message();
    const std::vector<uint8_t> expected = {'S', 0x00, 0x00, 0x00, 0x04};
    ASSERT_BYTES_EQ(msg, expected);
}

// Mutation guard: length must be 4, not 5 or 0.
TEST(QA_GDB867_PgWireHelpers, SyncMessageLength) {
    auto msg = pg_wire_test::build_sync_message();
    ASSERT_EQ(msg.size(), 5u);
    EXPECT_EQ(msg[0], static_cast<uint8_t>('S'));
    uint32_t len = (static_cast<uint32_t>(msg[1]) << 24) |
                   (static_cast<uint32_t>(msg[2]) << 16) |
                   (static_cast<uint32_t>(msg[3]) << 8) |
                   static_cast<uint32_t>(msg[4]);
    EXPECT_EQ(len, 4u);
}

// build_query_message
// -----------------------------------------------------------------------
// PG v3 Query ('Q'):
//   'Q' [int32 msg_len] [sql\0]
//   msg_len includes itself.
//
// Hand-derivation: sql="SELECT 1":
//   body = ['S','E','L','E','C','T',' ','1',0x00]  = 9 bytes
//   body_len = 4+9 = 13 = 0x0D
//   msg = ['Q', 0x00,0x00,0x00,0x0D, 'S','E','L','E','C','T',' ','1',0x00]

TEST(QA_GDB867_PgWireHelpers, QueryMessageExactBytes) {
    auto msg = pg_wire_test::build_query_message("SELECT 1");

    // clang-format off
    const std::vector<uint8_t> expected = {
        'Q',
        0x00, 0x00, 0x00, 0x0D,
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '1',
        0x00   // null terminator
    };
    // clang-format on

    ASSERT_BYTES_EQ(msg, expected);
}

// Mutation guard: verify sql has null terminator (not missing, not doubled).
TEST(QA_GDB867_PgWireHelpers, QueryMessageHasNullTerminator) {
    auto msg = pg_wire_test::build_query_message("X");
    // Expected: ['Q', 0,0,0,6, 'X', 0]
    ASSERT_EQ(msg.size(), 7u);
    EXPECT_EQ(msg[5], static_cast<uint8_t>('X'));
    EXPECT_EQ(msg[6], 0x00) << "query must be null-terminated";
}

// Empty SQL.
TEST(QA_GDB867_PgWireHelpers, QueryMessageEmptySql) {
    auto msg = pg_wire_test::build_query_message("");
    // body = [0x00], body_len = 4+1 = 5
    const std::vector<uint8_t> expected = {'Q', 0x00, 0x00, 0x00, 0x05, 0x00};
    ASSERT_BYTES_EQ(msg, expected);
}

// ---------------------------------------------------------------------------
// Endianness mutation guards — big-endian wire protocol
// ---------------------------------------------------------------------------

// If length were written little-endian, msg[1] would be non-zero for small
// messages.  E.g. sync: length=4 little-endian = [4,0,0,0]; big-endian = [0,0,0,4].
TEST(QA_GDB867_PgWireHelpers, SyncMessageIsBigEndian) {
    auto msg = pg_wire_test::build_sync_message();
    EXPECT_EQ(msg[1], 0x00) << "length field must be big-endian";
    EXPECT_EQ(msg[4], 0x04) << "length field must be big-endian";
}

TEST(QA_GDB867_PgWireHelpers, QueryMessageIsBigEndian) {
    // "SELECT 1" -> body_len 13 = 0x0000000D; in little-endian: [13,0,0,0].
    auto msg = pg_wire_test::build_query_message("SELECT 1");
    EXPECT_EQ(msg[1], 0x00) << "most-significant byte must be 0 (big-endian)";
    EXPECT_EQ(msg[4], 0x0D) << "least-significant byte must be 13 (big-endian)";
}

// ---------------------------------------------------------------------------
// Additional edge-case adversarial tests
// ---------------------------------------------------------------------------

// build_bind_message with zero params: body = portal\0 stmt\0 + format codes
// + 0 params + result codes.
// body = [0x00, 0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00] = 8 bytes
// body_len = 4+8 = 12 = 0x0C

TEST(QA_GDB867_PgWireHelpers, BindMessageZeroParams) {
    auto msg = pg_wire_test::build_bind_message("", "");
    const std::vector<uint8_t> expected = {
        'B',
        0x00, 0x00, 0x00, 0x0C,
        0x00,        // portal ""
        0x00,        // stmt ""
        0x00, 0x00,  // 0 format codes
        0x00, 0x00,  // 0 params
        0x00, 0x00   // 0 result format codes
    };
    ASSERT_BYTES_EQ(msg, expected);
}

// Empty string param is NOT the same as NULL (len=0, no value bytes, not sentinel).
TEST(QA_GDB867_PgWireHelpers, BindMessageEmptyStringNotNull) {
    auto msg = pg_wire_test::build_bind_message("", "", {std::string("")});
    // body: portal\0 stmt\0 = 2; format=2; count=2; param len=0 (4 bytes, no value); result=2 = 12
    // body_len = 16 = 0x10
    // clang-format off
    const std::vector<uint8_t> expected = {
        'B',
        0x00, 0x00, 0x00, 0x10,
        0x00, 0x00,        // portal, stmt
        0x00, 0x00,        // format codes
        0x00, 0x01,        // 1 param
        0x00, 0x00, 0x00, 0x00,  // len = 0 (NOT null sentinel)
        0x00, 0x00          // result format codes
    };
    // clang-format on
    ASSERT_BYTES_EQ(msg, expected);
}

// build_parse_message with one OID param.
// stmt="s", sql="SELECT $1", oids=[23]:
//   body = ['s',0, 'S','E','L','E','C','T',' ','$','1',0, 0x00,0x01, 0x00,0x00,0x00,0x17]
//        = 2 + 10 + 2 + 4 = 18 bytes
//   body_len = 22 = 0x16
TEST(QA_GDB867_PgWireHelpers, ParseMessageWithOneOid) {
    auto msg = pg_wire_test::build_parse_message("s", "SELECT $1", {23u});
    // clang-format off
    const std::vector<uint8_t> expected = {
        'P',
        0x00, 0x00, 0x00, 0x16,
        's', 0x00,
        'S','E','L','E','C','T',' ','$','1', 0x00,
        0x00, 0x01,          // 1 param OID
        0x00, 0x00, 0x00, 0x17  // OID 23 = INT4
    };
    // clang-format on
    ASSERT_BYTES_EQ(msg, expected);
}

// Startup message with multiple params and correct termination.
TEST(QA_GDB867_PgWireHelpers, StartupMessageMultipleParams) {
    auto msg = pg_wire_test::build_startup_message({{"user", "alice"}, {"database", "mydb"}});
    // payload:
    //   version(4) + "user\0alice\0" (11) + "database\0mydb\0" (14) + term(1) = 30
    //   total_len = 4 + 30 = 34 = 0x22
    ASSERT_GE(msg.size(), 4u);
    uint32_t encoded_len = (static_cast<uint32_t>(msg[0]) << 24) |
                           (static_cast<uint32_t>(msg[1]) << 16) |
                           (static_cast<uint32_t>(msg[2]) << 8) |
                           static_cast<uint32_t>(msg[3]);
    EXPECT_EQ(encoded_len, 34u);
    EXPECT_EQ(encoded_len, static_cast<uint32_t>(msg.size()));
    // Protocol version bytes at offset 4-7.
    EXPECT_EQ(msg[4], 0x00);
    EXPECT_EQ(msg[5], 0x03);
    EXPECT_EQ(msg[6], 0x00);
    EXPECT_EQ(msg[7], 0x00);
    // Last byte must be the terminator.
    EXPECT_EQ(msg.back(), 0x00);
}

} // namespace
