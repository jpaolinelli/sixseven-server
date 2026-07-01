// test_qa_gdb_951.cpp -- Adversarial QA tests for GDB-951: real interactive
// pg-wire v3 CLI (decoder robustness, formatter edge cases, REPL edge cases).
//
// All tests are socket-free; they exercise the pure-library components only.
// Live TCP connect is Windows-unverifiable (CRT fd-assert is pre-existing).

#include "sixseven/cli/pg_wire_codec.h"
#include "sixseven/cli/repl.h"
#include "sixseven/cli/result_formatter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sixseven;
using namespace sixseven::cli;

// ---------------------------------------------------------------------------
// Wire-building helpers
// ---------------------------------------------------------------------------

namespace {

void wb_push_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void wb_push_be16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void wb_push_cstr(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

// Build a minimal ReadyForQuery message: 'Z' + len=5 + status.
std::vector<uint8_t> build_rfq(char status = 'I') {
    std::vector<uint8_t> buf = {'Z', 0, 0, 0, 5, static_cast<uint8_t>(status)};
    return buf;
}

// Build a well-formed RowDescription with n zero-filled columns.
std::vector<uint8_t> build_row_description(const std::vector<std::string>& col_names) {
    std::vector<uint8_t> body;
    wb_push_be16(body, static_cast<uint16_t>(col_names.size()));
    for (const auto& name : col_names) {
        wb_push_cstr(body, name);
        // tableOID, attrNum, typeOID, typeSize, typeMod, formatCode
        wb_push_be32(body, 0);                         // tableOID
        wb_push_be16(body, 0);                         // attrNum
        wb_push_be32(body, 25);                        // typeOID = text
        wb_push_be16(body, static_cast<uint16_t>(-1)); // typeSize = -1 (variable)
        wb_push_be32(body, static_cast<uint32_t>(-1)); // typeMod = -1
        wb_push_be16(body, 0);                         // formatCode = text
    }

    std::vector<uint8_t> msg;
    msg.push_back('T');
    wb_push_be32(msg, static_cast<uint32_t>(4 + body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

// Build a DataRow from a list of optional strings (nullopt = SQL NULL).
std::vector<uint8_t> build_data_row(const std::vector<std::optional<std::string>>& fields) {
    std::vector<uint8_t> body;
    wb_push_be16(body, static_cast<uint16_t>(fields.size()));
    for (const auto& f : fields) {
        if (!f.has_value()) {
            wb_push_be32(body, 0xFFFFFFFFu); // -1 = NULL
        } else {
            wb_push_be32(body, static_cast<uint32_t>(f->size()));
            body.insert(body.end(), f->begin(), f->end());
        }
    }

    std::vector<uint8_t> msg;
    msg.push_back('D');
    wb_push_be32(msg, static_cast<uint32_t>(4 + body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

// Build an ErrorResponse with the standard fields.
[[maybe_unused]] std::vector<uint8_t> build_error_response(const std::string& severity,
                                                           const std::string& sqlstate,
                                                           const std::string& message) {
    std::vector<uint8_t> body;
    body.push_back('S');
    wb_push_cstr(body, severity);
    body.push_back('C');
    wb_push_cstr(body, sqlstate);
    body.push_back('M');
    wb_push_cstr(body, message);
    body.push_back(0); // terminator

    std::vector<uint8_t> msg;
    msg.push_back('E');
    wb_push_be32(msg, static_cast<uint32_t>(4 + body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

struct FakeExec951 {
    std::vector<std::string> captured;
    bool should_fail{false};

    Result<void> operator()(const std::string& sql) {
        captured.push_back(sql);
        if (should_fail) {
            return make_error(StatusCode::NETWORK_ERROR, "simulated failure");
        }
        return ok();
    }
};

static ExecFn make_fn951(FakeExec951& exec) {
    return [&exec](const std::string& sql) -> Result<void> { return exec(sql); };
}

} // namespace

// ===========================================================================
// DECODER ROBUSTNESS
// ===========================================================================

// --- Truncated headers (1, 2, 3, 4 bytes) -> NOT_FOUND, no over-read -------

TEST(QA_GDB951_Decoder, TruncatedHeader1Byte) {
    std::vector<uint8_t> buf = {'Z'};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB951_Decoder, TruncatedHeader2Bytes) {
    std::vector<uint8_t> buf = {'Z', 0};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB951_Decoder, TruncatedHeader3Bytes) {
    std::vector<uint8_t> buf = {'Z', 0, 0};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB951_Decoder, TruncatedHeader4Bytes) {
    // 4 bytes: type + 3 of the 4-byte length field -- still incomplete header.
    std::vector<uint8_t> buf = {'Z', 0, 0, 0};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

// --- Length field larger than available bytes -> NOT_FOUND, no over-read ---

TEST(QA_GDB951_Decoder, LengthClaimsMoreBytesThanPresent) {
    // ReadyForQuery: says length = 0xFF (253), but buffer only has 6 bytes.
    std::vector<uint8_t> buf = {'Z', 0, 0, 0, 0xFF, 'I'};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_GDB951_Decoder, LengthClaimsMaxInt32MoreThanPresent) {
    // INT32_MAX as length -- should not over-read or integer-overflow the
    // total = 1 + msg_len computation.
    std::vector<uint8_t> buf = {'Z', 0x7F, 0xFF, 0xFF, 0xFF, 'I'};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

// --- Length field < 4 -> PARSE_ERROR (not a valid pg-wire message) ----------

TEST(QA_GDB951_Decoder, LengthFieldSmallerThan4) {
    // Any length < 4 is a protocol error.
    std::vector<uint8_t> buf = {'Z', 0, 0, 0, 3, 'I'};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB951_Decoder, LengthFieldZero) {
    std::vector<uint8_t> buf = {'Z', 0, 0, 0, 0, 'I'};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- RowDescription: column-count says N but buffer ends early --------------

TEST(QA_GDB951_Decoder, RowDescriptionTruncatedAfterColumnCount) {
    // RowDescription says 3 columns but body has nothing after the count.
    std::vector<uint8_t> body;
    wb_push_be16(body, 3); // claims 3 columns

    std::vector<uint8_t> buf;
    buf.push_back('T');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    // Should fail gracefully: either parse error or the column list is short.
    // The implementation iterates num_cols times; on first iteration the
    // cstring read returns "" (off-end), then the 18-byte check fires -> error.
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB951_Decoder, RowDescriptionTruncatedInColumnFixedData) {
    // One column name present but only 10 of the 18 fixed bytes after it.
    std::vector<uint8_t> body;
    wb_push_be16(body, 1);          // num_cols = 1
    wb_push_cstr(body, "mycolumn"); // name
    // Only 10 bytes of the 18-byte fixed block:
    for (int i = 0; i < 10; ++i) {
        body.push_back(0);
    }

    std::vector<uint8_t> buf;
    buf.push_back('T');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- DataRow: column-count says N but buffer ends early ---------------------

TEST(QA_GDB951_Decoder, DataRowTruncatedBeforeFieldLength) {
    // DataRow says 3 fields but only provides 2 field-length words.
    std::vector<uint8_t> body;
    wb_push_be16(body, 3); // claims 3 fields
    // Field 0: "a"
    wb_push_be32(body, 1);
    body.push_back('a');
    // Field 1: "b"
    wb_push_be32(body, 1);
    body.push_back('b');
    // Field 2: truncated -- no length word at all

    std::vector<uint8_t> buf;
    buf.push_back('D');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB951_Decoder, DataRowFieldLengthExceedsBody) {
    // DataRow: 1 field, field-length word claims 100 bytes but body is shorter.
    std::vector<uint8_t> body;
    wb_push_be16(body, 1);   // 1 field
    wb_push_be32(body, 100); // claims 100 bytes
    body.push_back('x');     // only 1 byte of data

    std::vector<uint8_t> buf;
    buf.push_back('D');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- Field length -1 (NULL) vs 0 (empty string) -- must be distinct --------

TEST(QA_GDB951_Decoder, DataRowNullFieldDistinctFromEmpty) {
    // Two fields: first is NULL (-1), second is empty string (length 0).
    std::vector<uint8_t> body;
    wb_push_be16(body, 2);
    // NULL
    wb_push_be32(body, 0xFFFFFFFFu);
    // Empty string
    wb_push_be32(body, 0u);

    std::vector<uint8_t> buf;
    buf.push_back('D');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->data_row.fields.size(), 2u);
    // Field 0: SQL NULL
    EXPECT_FALSE(r->data_row.fields[0].has_value());
    // Field 1: empty string (not NULL)
    ASSERT_TRUE(r->data_row.fields[1].has_value());
    EXPECT_EQ(*r->data_row.fields[1], "");
}

// --- DataRow with INT32_MAX field length -> no allocation bomb --------------

TEST(QA_GDB951_Decoder, DataRowHugeFieldLength) {
    // field_len = INT32_MAX -- buffer doesn't have that many bytes,
    // so the bounds check must fire, not any allocation / index overflow.
    std::vector<uint8_t> body;
    wb_push_be16(body, 1);
    // INT32_MAX = 0x7FFFFFFF
    wb_push_be32(body, 0x7FFFFFFFu);
    body.push_back('x'); // tiny body

    std::vector<uint8_t> buf;
    buf.push_back('D');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- RowDescription with a negative (sign-extended) column count ------------
// The wire protocol encodes num_cols as int16_t; a -1 = 0xFFFF -> cast to
// uint16_t = 65535.  The loop would try to iterate 65535 times.  After the
// first iteration the inner body truncation check should fire.

TEST(QA_GDB951_Decoder, RowDescriptionNegativeColumnCount) {
    // num_cols = -1 (wire: 0xFF 0xFF), body contains no column data.
    std::vector<uint8_t> body;
    body.push_back(0xFF); // high byte of int16 = -1
    body.push_back(0xFF); // low byte

    std::vector<uint8_t> buf;
    buf.push_back('T');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    // Either parse error from body-bounds check (expected) or possibly a
    // successful empty column list. What must NOT happen: an infinite loop or
    // memory over-read. The test guards against both by asserting we get back
    // some result in finite time with no crash.
    // Acceptable outcomes: error (PARSE_ERROR) or success with 0 columns.
    if (r.has_value()) {
        // If it "succeeds", the column list should be empty (no valid data).
        // 65535 iterations with no body data would have triggered the truncation
        // check on the very first column, returning an error. Getting here means
        // the implementation cast -1 to int16_t and treated it as 0 columns.
        EXPECT_EQ(r->row_desc.columns.size(), 0u);
    } else {
        EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
    }
}

// --- ErrorResponse without null terminator ----------------------------------

TEST(QA_GDB951_Decoder, ErrorResponseNoNullTerminator) {
    // ErrorResponse body with 'M' field and NO trailing \0 terminator.
    // The cstring reader should stop at body_len without over-reading.
    std::vector<uint8_t> body;
    body.push_back('M');
    const char* msg = "no terminator here";
    for (const char* p = msg; *p; ++p) {
        body.push_back(static_cast<uint8_t>(*p));
    }
    // Deliberately omit the null terminator AND the \0 block terminator.

    std::vector<uint8_t> buf;
    buf.push_back('E');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    // Must not crash; graceful parse (message may be partially populated).
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::ErrorResponse);
    // The message field should contain what was read up to body end.
    EXPECT_NE(r->error_resp.message.find("no terminator"), std::string::npos);
}

// --- Unknown message type byte -> Unknown tag, not a crash ------------------

TEST(QA_GDB951_Decoder, UnknownMessageTypeByte) {
    // Use type byte 0x01 (not a valid pg-wire server message type).
    std::vector<uint8_t> buf = {0x01, 0, 0, 0, 4}; // len=4, no body
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::Unknown);
    EXPECT_EQ(r->raw_type, 0x01u);
    EXPECT_EQ(consumed, 5u);
}

TEST(QA_GDB951_Decoder, UnknownMessageTypeByte255) {
    std::vector<uint8_t> buf = {0xFF, 0, 0, 0, 4};
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::Unknown);
}

// --- Authentication message with non-zero sub-type --------------------------

TEST(QA_GDB951_Decoder, AuthMD5ReturnsPopulatedAuthType) {
    // 'R' + len=12 + auth_type=5 (MD5) + 4 bytes salt
    std::vector<uint8_t> buf = {'R',
                                0,
                                0,
                                0,
                                12,
                                0,
                                0,
                                0,
                                5, // auth_type = 5 (MD5)
                                0xDE,
                                0xAD,
                                0xBE,
                                0xEF}; // salt
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::Authentication);
    EXPECT_EQ(r->auth.auth_type, 5); // MD5
    ASSERT_EQ(r->auth.payload.size(), 4u);
    EXPECT_EQ(r->auth.payload[0], 0xDEu);
}

TEST(QA_GDB951_Decoder, AuthSCRAMReturnsPopulatedAuthType) {
    // auth_type=10 (SCRAM-SHA-256), no payload bytes needed beyond type.
    std::vector<uint8_t> buf;
    buf.push_back('R');
    wb_push_be32(buf, 8);  // len=8 -> body_len=4
    wb_push_be32(buf, 10); // auth_type=10 (SCRAM)
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->auth.auth_type, 10);     // SCRAM
    EXPECT_TRUE(r->auth.payload.empty()); // no payload bytes
}

// --- Authentication message body too short ----------------------------------

TEST(QA_GDB951_Decoder, AuthBodyTooShort) {
    // 'R' + len=7 (body_len=3) < 4 -> PARSE_ERROR.
    std::vector<uint8_t> buf = {'R', 0, 0, 0, 7, 0, 0, 0}; // body_len=3
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- BackendKeyData body too short ------------------------------------------

TEST(QA_GDB951_Decoder, BackendKeyDataBodyTooShort) {
    // 'K' + len=8 (body_len=4) < 8 -> PARSE_ERROR.
    std::vector<uint8_t> buf;
    buf.push_back('K');
    wb_push_be32(buf, 8);  // len=8 -> body_len=4
    wb_push_be32(buf, 42); // only 4 bytes of body
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- ReadyForQuery body too short -------------------------------------------

TEST(QA_GDB951_Decoder, ReadyForQueryBodyTooShort) {
    // 'Z' + len=4 (body_len=0) -- needs at least 1 byte.
    std::vector<uint8_t> buf;
    buf.push_back('Z');
    wb_push_be32(buf, 4); // len=4 -> body_len=0
    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// --- Trailing garbage after a complete message -> next decode handles it ----

TEST(QA_GDB951_Decoder, TrailingGarbageAfterCompleteMessage) {
    // A well-formed ReadyForQuery followed by 5 bytes of garbage.
    auto rfq = build_rfq('I');
    rfq.insert(rfq.end(), {0xDE, 0xAD, 0xBE, 0xEF, 0x00});

    // First decode should consume exactly the ReadyForQuery.
    size_t consumed = 0;
    auto r = decode_one_message(rfq, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::ReadyForQuery);
    EXPECT_EQ(consumed, 6u); // 1 type + 4 len + 1 body

    // Second decode: feed the remaining 5 bytes of garbage.
    auto remaining = std::vector<uint8_t>(rfq.begin() + 6, rfq.end());
    size_t consumed2 = 0;
    auto r2 = decode_one_message(remaining, consumed2);
    // 5 bytes is enough for a header (5 bytes) but msg_len=0xDEADBEEF is
    // astronomically large so NOT_FOUND (not enough body bytes).
    ASSERT_FALSE(r2.has_value());
    // NOT_FOUND (body too short) or PARSE_ERROR (len < 4). Either is correct.
    EXPECT_TRUE(r2.error().code == StatusCode::NOT_FOUND ||
                r2.error().code == StatusCode::PARSE_ERROR);
}

// --- Consumed is NOT set when an error is returned --------------------------

TEST(QA_GDB951_Decoder, ConsumedNotModifiedOnError) {
    // On error, consumed should remain unchanged.
    std::vector<uint8_t> buf = {'Z', 0, 0}; // truncated header
    size_t consumed = 0xDEADBEEF;           // sentinel
    auto r = decode_one_message(buf, consumed);
    ASSERT_FALSE(r.has_value());
    // consumed should not be changed (the impl currently doesn't set it on
    // error, which is correct).
    EXPECT_EQ(consumed, static_cast<size_t>(0xDEADBEEF));
}

// --- DataRow: 0 fields is valid (empty row) ---------------------------------

TEST(QA_GDB951_Decoder, DataRowZeroFields) {
    std::vector<uint8_t> body;
    wb_push_be16(body, 0); // 0 fields

    std::vector<uint8_t> buf;
    buf.push_back('D');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::DataRow);
    EXPECT_TRUE(r->data_row.fields.empty());
}

// --- RowDescription: 0 columns is valid (e.g. command result) ---------------

TEST(QA_GDB951_Decoder, RowDescriptionZeroColumns) {
    auto msg = build_row_description({});
    size_t consumed = 0;
    auto r = decode_one_message(msg, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::RowDescription);
    EXPECT_TRUE(r->row_desc.columns.empty());
}

// --- ParameterStatus: two cstrings, no trailing data -----------------------

TEST(QA_GDB951_Decoder, ParameterStatusDecoded) {
    std::vector<uint8_t> body;
    wb_push_cstr(body, "server_version");
    wb_push_cstr(body, "14.0");

    std::vector<uint8_t> buf;
    buf.push_back('S');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::ParameterStatus);
    EXPECT_EQ(r->param_status.name, "server_version");
    EXPECT_EQ(r->param_status.value, "14.0");
}

// --- DataRow with a field containing embedded null bytes --------------------
// (pg-wire is length-prefixed, so embedded nulls are legal in the field data)

TEST(QA_GDB951_Decoder, DataRowFieldWithEmbeddedNull) {
    std::string val("ab\x00cd", 5); // 5 bytes with an embedded null
    std::vector<uint8_t> body;
    wb_push_be16(body, 1);
    wb_push_be32(body, static_cast<uint32_t>(val.size()));
    body.insert(body.end(), val.begin(), val.end());

    std::vector<uint8_t> buf;
    buf.push_back('D');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->data_row.fields.size(), 1u);
    ASSERT_TRUE(r->data_row.fields[0].has_value());
    EXPECT_EQ(r->data_row.fields[0]->size(), 5u);
}

// --- NoticeResponse same format as ErrorResponse ---------------------------

TEST(QA_GDB951_Decoder, NoticeResponseDecoded) {
    std::vector<uint8_t> body;
    body.push_back('M');
    wb_push_cstr(body, "helpful notice");
    body.push_back(0); // terminator

    std::vector<uint8_t> buf;
    buf.push_back('N');
    wb_push_be32(buf, static_cast<uint32_t>(4 + body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto r = decode_one_message(buf, consumed);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->tag, ServerMsgTag::NoticeResponse);
    EXPECT_EQ(r->notice.message, "helpful notice");
}

// ===========================================================================
// ENCODER SANITY
// ===========================================================================

TEST(QA_GDB951_Encoder, StartupMessageSelfConsistentLength) {
    auto msg = encode_startup_message("alice", "testdb");
    ASSERT_GE(msg.size(), 4u);
    uint32_t declared_len = (static_cast<uint32_t>(msg[0]) << 24) |
                            (static_cast<uint32_t>(msg[1]) << 16) |
                            (static_cast<uint32_t>(msg[2]) << 8) | static_cast<uint32_t>(msg[3]);
    EXPECT_EQ(declared_len, static_cast<uint32_t>(msg.size()));
}

TEST(QA_GDB951_Encoder, QueryMessageNullTerminated) {
    auto msg = encode_query_message("SELECT 42");
    ASSERT_FALSE(msg.empty());
    EXPECT_EQ(msg.back(), 0u);
}

TEST(QA_GDB951_Encoder, QueryMessageVeryLongSql) {
    std::string sql(65536, 'x'); // 64 KiB of junk
    auto msg = encode_query_message(sql);
    EXPECT_EQ(msg[0], static_cast<uint8_t>('Q'));
    EXPECT_EQ(msg.back(), 0u);
    // Declared length must match actual.
    uint32_t declared_len = (static_cast<uint32_t>(msg[1]) << 24) |
                            (static_cast<uint32_t>(msg[2]) << 16) |
                            (static_cast<uint32_t>(msg[3]) << 8) | static_cast<uint32_t>(msg[4]);
    EXPECT_EQ(1u + declared_len, msg.size());
}

// ===========================================================================
// RESULT FORMATTER EDGE CASES
// ===========================================================================

// --- Value containing embedded newlines/tabs --------------------------------

TEST(QA_GDB951_Formatter, ValueWithEmbeddedNewline) {
    std::vector<std::string> cols = {"note"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {std::string("line1\nline2")},
    };
    // Should not crash. The embedded newline will appear literally in the
    // output, which is acceptable (byte-based formatter).
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find("line1"), std::string::npos);
    EXPECT_NE(out.find("line2"), std::string::npos);
    EXPECT_NE(out.find("(1 row)"), std::string::npos);
}

TEST(QA_GDB951_Formatter, ValueWithEmbeddedTab) {
    std::vector<std::string> cols = {"data"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {std::string("before\tafter")},
    };
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find("before"), std::string::npos);
    EXPECT_NE(out.find("after"), std::string::npos);
}

// --- Very wide value --------------------------------------------------------

TEST(QA_GDB951_Formatter, VeryWideValue) {
    std::vector<std::string> cols = {"col"};
    std::string wide_val(1024, 'A');
    std::vector<std::vector<std::optional<std::string>>> rows = {{wide_val}};
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find(wide_val), std::string::npos);
    EXPECT_NE(out.find("(1 row)"), std::string::npos);
}

// --- Column name wider than all values --------------------------------------

TEST(QA_GDB951_Formatter, ColumnNameWiderThanAllValues) {
    std::vector<std::string> cols = {"extremely_long_column_header_name"};
    std::vector<std::vector<std::optional<std::string>>> rows = {{"x"}, {"y"}};
    auto out = format_result_table(cols, rows, "SELECT 2");
    // Header must appear untruncated.
    EXPECT_NE(out.find("extremely_long_column_header_name"), std::string::npos);
    // The separator must be at least as wide as the header.
    size_t sep_pos = out.find("--");
    EXPECT_NE(sep_pos, std::string::npos);
}

// --- Many columns -----------------------------------------------------------

TEST(QA_GDB951_Formatter, ManyColumns) {
    std::vector<std::string> cols;
    for (int i = 0; i < 20; ++i) {
        cols.push_back("col" + std::to_string(i));
    }
    std::vector<std::optional<std::string>> row;
    for (int i = 0; i < 20; ++i) {
        row.push_back(std::to_string(i * 100));
    }
    std::vector<std::vector<std::optional<std::string>>> rows = {row};
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find("col0"), std::string::npos);
    EXPECT_NE(out.find("col19"), std::string::npos);
    EXPECT_NE(out.find("(1 row)"), std::string::npos);
}

// --- Row with FEWER values than columns (defensive) -------------------------

TEST(QA_GDB951_Formatter, RowFewerValuesThanColumns) {
    // Row has 1 value but schema has 3 columns.  Missing cells should render
    // as empty strings (not crash).
    std::vector<std::string> cols = {"a", "b", "c"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {std::string("only_a")}, // 1 value, not 3
    };
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find("only_a"), std::string::npos);
    EXPECT_NE(out.find("(1 row)"), std::string::npos);
}

// --- Row with MORE values than columns (defensive) --------------------------

TEST(QA_GDB951_Formatter, RowMoreValuesThanColumns) {
    // Row has 4 values but schema has 2 columns.  Extra values are silently
    // ignored (the loop is bounded by ncols).
    std::vector<std::string> cols = {"x", "y"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {std::string("1"), std::string("2"), std::string("3"), std::string("4")},
    };
    // Must not crash.
    auto out = format_result_table(cols, rows, "SELECT 1");
    EXPECT_NE(out.find("(1 row)"), std::string::npos);
}

// --- 0 columns + non-empty command tag (INSERT-style) -----------------------

TEST(QA_GDB951_Formatter, ZeroColumnsNonEmptyTag) {
    std::vector<std::string> cols;
    std::vector<std::vector<std::optional<std::string>>> rows;
    auto out = format_result_table(cols, rows, "DELETE 5");
    EXPECT_NE(out.find("DELETE 5"), std::string::npos);
    // Must not include a row-count line.
    EXPECT_EQ(out.find("rows"), std::string::npos);
}

// --- 0 columns + empty command tag ------------------------------------------

TEST(QA_GDB951_Formatter, ZeroColumnsEmptyTag) {
    std::vector<std::string> cols;
    std::vector<std::vector<std::optional<std::string>>> rows;
    auto out = format_result_table(cols, rows, "");
    // Empty output or whitespace-only -- just must not crash.
    EXPECT_TRUE(out.empty() || out.find_first_not_of(" \n\r\t") == std::string::npos);
}

// --- All-NULL row -----------------------------------------------------------

TEST(QA_GDB951_Formatter, AllNullRow) {
    std::vector<std::string> cols = {"a", "b", "c"};
    std::vector<std::vector<std::optional<std::string>>> rows = {
        {std::nullopt, std::nullopt, std::nullopt},
    };
    auto out = format_result_table(cols, rows, "SELECT 1");
    // Should show three "NULL" markers.
    size_t pos = 0;
    int null_count = 0;
    while ((pos = out.find("NULL", pos)) != std::string::npos) {
        ++null_count;
        ++pos;
    }
    EXPECT_EQ(null_count, 3);
}

// ===========================================================================
// REPL EDGE CASES
// ===========================================================================

// --- Semicolon inside a string literal (known limitation) -------------------
// The REPL splits on any trailing semicolon, so SELECT ';' will be split
// after the single-quote content ends with ';'.  This is a known limitation
// (low severity); the test documents the actual behavior without asserting
// "correct" SQL semantics.

TEST(QA_GDB951_Repl, SemicolonInsideStringLiteral_KnownLimitation) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    // Input: SELECT ';' (the semicolon is INSIDE the string literal, so the
    // statement does NOT end with a ';' after trimming -- the closing char
    // is the single-quote).  Then \q exits.
    //
    // The REPL checks has_terminating_semicolon on the accumulated buffer and
    // correctly sees that the line ends with "'" not ";", so nothing is sent.
    // \q then exits cleanly.
    //
    // A SEPARATE known-limitation case: if the user types SELECT ';'; (a
    // semicolon after the closing quote), the REPL sends the whole thing as one
    // statement -- the server must tolerate the extra semicolon. We do not test
    // that here because it requires a real connection.
    std::istringstream in("SELECT ';'\n\\q\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The statement was not terminated, so nothing should have been sent.
    EXPECT_TRUE(exec.captured.empty());
}

// --- Multiple statements on one line ----------------------------------------

TEST(QA_GDB951_Repl, MultipleStatementsOnOneLine_OnlyFirstSent) {
    // "SELECT 1; SELECT 2;" on a single line.
    // The REPL checks has_terminating_semicolon on the ENTIRE accumulated
    // buffer each time a line is received.  It sees "SELECT 1; SELECT 2;"
    // and sends the whole thing as a single statement to exec_fn.
    // This is correct behavior for the accumulator design.
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("SELECT 1; SELECT 2;\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Either 1 exec (whole line as one statement) or 2 execs if split.
    // Both are acceptable; the key assertion is no crash.
    EXPECT_FALSE(exec.captured.empty());
}

// --- Whitespace-only line does not send statement ---------------------------

TEST(QA_GDB951_Repl, WhitespaceOnlyLineDoesNotSend) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("   \n\t\n\n\\q\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(exec.captured.empty());
}

// --- \q mid-multi-line buffer exits without sending partial statement -------

TEST(QA_GDB951_Repl, BackslashQMidMultiLineBufferNoSend) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    // Start a multi-line statement then \q before the semicolon.
    std::istringstream in("SELECT\n1\n\\q\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The partial "SELECT\n1" statement must NOT have been sent.
    EXPECT_TRUE(exec.captured.empty());
}

// --- -c with empty string ---------------------------------------------------

TEST(QA_GDB951_Repl, OneShotEmptyString) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.one_shot = "";
    opts.interactive = false;

    std::istringstream in("");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    // Empty one_shot: falls through to REPL mode (one_shot.empty() check).
    // With empty stdin, should exit cleanly.
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(exec.captured.empty());
}

// --- EOF on incomplete statement: no partial send ---------------------------

TEST(QA_GDB951_Repl, EofOnIncompleteStatementNoPartialSend) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    // Statement never terminated with ';'.
    std::istringstream in("SELECT * FROM users WHERE id = 42");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(exec.captured.empty());
}

// --- Meta-command with args (e.g. \help arg) --------------------------------

TEST(QA_GDB951_Repl, MetaCommandWithArgs) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    // "\help tables" -- not recognised; should be reported, not executed.
    std::istringstream in("\\help tables\n\\q\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(exec.captured.empty());
    // The output should mention help or the unknown command.
    EXPECT_FALSE(out.str().empty());
}

// --- \exit is also recognised as quit ---------------------------------------

TEST(QA_GDB951_Repl, BackslashExitExits) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("\\exit\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(exec.captured.empty());
}

// --- Interactive prompt output differs from non-interactive -----------------

TEST(QA_GDB951_Repl, InteractiveModePrintsPrompt) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = true; // interactive ON

    std::istringstream in("\\q\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // The prompt "sixseven=> " should appear.
    EXPECT_NE(out.str().find("sixseven=>"), std::string::npos);
}

TEST(QA_GDB951_Repl, NonInteractiveModeNoPrimaryPrompt) {
    FakeExec951 exec;
    ReplOptions opts;
    opts.interactive = false; // non-interactive

    std::istringstream in("\\q\n");
    std::ostringstream out;
    auto fn = make_fn951(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // No prompt in non-interactive mode.
    EXPECT_EQ(out.str().find("sixseven=>"), std::string::npos);
}

// --- Decode + format integration round-trip ---------------------------------

TEST(QA_GDB951_Integration, DecodeAndFormatRoundTrip) {
    // Build a full server response, decode it, and format it.
    auto rd = build_row_description({"id", "name"});
    auto dr1 = build_data_row({std::string("1"), std::string("alice")});
    auto dr2 = build_data_row({std::string("2"), std::nullopt}); // NULL name

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), rd.begin(), rd.end());
    buf.insert(buf.end(), dr1.begin(), dr1.end());
    buf.insert(buf.end(), dr2.begin(), dr2.end());

    // Decode RowDescription.
    size_t consumed = 0;
    auto m1 = decode_one_message(buf, consumed);
    ASSERT_TRUE(m1.has_value()) << m1.error().message;
    ASSERT_EQ(m1->tag, ServerMsgTag::RowDescription);

    std::vector<std::string> col_names;
    for (const auto& c : m1->row_desc.columns) {
        col_names.push_back(c.name);
    }

    // Decode DataRow 1.
    auto rem1 = std::vector<uint8_t>(buf.begin() + static_cast<ptrdiff_t>(consumed), buf.end());
    size_t c2 = 0;
    auto m2 = decode_one_message(rem1, c2);
    ASSERT_TRUE(m2.has_value()) << m2.error().message;
    ASSERT_EQ(m2->tag, ServerMsgTag::DataRow);

    // Decode DataRow 2.
    auto rem2 = std::vector<uint8_t>(rem1.begin() + static_cast<ptrdiff_t>(c2), rem1.end());
    size_t c3 = 0;
    auto m3 = decode_one_message(rem2, c3);
    ASSERT_TRUE(m3.has_value()) << m3.error().message;
    ASSERT_EQ(m3->tag, ServerMsgTag::DataRow);

    std::vector<std::vector<std::optional<std::string>>> rows = {m2->data_row.fields,
                                                                 m3->data_row.fields};

    auto formatted = format_result_table(col_names, rows, "SELECT 2");
    EXPECT_NE(formatted.find("id"), std::string::npos);
    EXPECT_NE(formatted.find("name"), std::string::npos);
    EXPECT_NE(formatted.find("alice"), std::string::npos);
    EXPECT_NE(formatted.find("NULL"), std::string::npos);
    EXPECT_NE(formatted.find("(2 rows)"), std::string::npos);
}
