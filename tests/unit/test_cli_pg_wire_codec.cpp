#include "sixseven/cli/pg_wire_codec.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace sixseven::cli;

// ---------------------------------------------------------------------------
// Helper: read big-endian uint32
// ---------------------------------------------------------------------------
static uint32_t read_be32(const std::vector<uint8_t>& v, size_t off) {
    return (static_cast<uint32_t>(v[off]) << 24) | (static_cast<uint32_t>(v[off + 1]) << 16) |
           (static_cast<uint32_t>(v[off + 2]) << 8) | static_cast<uint32_t>(v[off + 3]);
}

// ---------------------------------------------------------------------------
// StartupMessage encoding
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, StartupMessageLayout) {
    auto msg = encode_startup_message("alice", "mydb");

    // Minimum size: 4 (len) + 4 (version) + cstrings + terminator.
    ASSERT_GE(msg.size(), 8u);

    // First 4 bytes = total length (including itself).
    uint32_t total_len = read_be32(msg, 0);
    EXPECT_EQ(total_len, static_cast<uint32_t>(msg.size()));

    // Next 4 bytes = protocol version 196608.
    uint32_t version = read_be32(msg, 4);
    EXPECT_EQ(version, 196608u);

    // Body contains "user\0alice\0database\0mydb\0\0".
    const char* expected_body = "user\0alice\0database\0mydb\0";
    size_t expected_body_len = 25; // 5+6+9+5+null_end
    // Count the null-terminated pairs manually.
    // "user" = 4 + 1(null), "alice" = 5 + 1 = 11
    // "database" = 8 + 1, "mydb" = 4 + 1 = 14
    // terminator = 1  => total body = 11 + 14 + 1 = 26
    ASSERT_GE(msg.size(), 8u + 26u);
    // Check each field by scanning for known strings.
    std::string body(reinterpret_cast<const char*>(msg.data() + 8), msg.size() - 8);
    EXPECT_NE(body.find("user"), std::string::npos);
    EXPECT_NE(body.find("alice"), std::string::npos);
    EXPECT_NE(body.find("database"), std::string::npos);
    EXPECT_NE(body.find("mydb"), std::string::npos);

    // Suppress unused variable warning.
    (void)expected_body;
    (void)expected_body_len;
}

TEST(CliPgWireCodec, StartupMessageEmptyUser) {
    auto msg = encode_startup_message("", "");
    ASSERT_GE(msg.size(), 8u);
    uint32_t total_len = read_be32(msg, 0);
    EXPECT_EQ(total_len, static_cast<uint32_t>(msg.size()));
}

// ---------------------------------------------------------------------------
// QueryMessage encoding
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, QueryMessageLayout) {
    auto msg = encode_query_message("SELECT 1");

    // 'Q' + 4-byte len + "SELECT 1\0"
    ASSERT_EQ(msg.size(), 1u + 4u + 8u + 1u); // 14

    EXPECT_EQ(msg[0], static_cast<uint8_t>('Q'));

    uint32_t msg_len = read_be32(msg, 1);
    // Length includes itself (4) + sql bytes (8) + null (1) = 13.
    EXPECT_EQ(msg_len, 13u);

    // SQL bytes.
    std::string sql(reinterpret_cast<const char*>(msg.data() + 5), 8);
    EXPECT_EQ(sql, "SELECT 1");

    // Null terminator.
    EXPECT_EQ(msg.back(), 0u);
}

TEST(CliPgWireCodec, QueryMessageEmptySql) {
    auto msg = encode_query_message("");
    ASSERT_EQ(msg.size(), 1u + 4u + 1u); // 'Q' + len(5) + null
    EXPECT_EQ(msg[0], static_cast<uint8_t>('Q'));
    uint32_t msg_len = read_be32(msg, 1);
    EXPECT_EQ(msg_len, 5u);
    EXPECT_EQ(msg.back(), 0u);
}

// ---------------------------------------------------------------------------
// TerminateMessage encoding
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, TerminateMessageLayout) {
    auto msg = encode_terminate_message();
    ASSERT_EQ(msg.size(), 5u);
    EXPECT_EQ(msg[0], static_cast<uint8_t>('X'));
    uint32_t len = read_be32(msg, 1);
    EXPECT_EQ(len, 4u);
}

// ---------------------------------------------------------------------------
// Decoding: RowDescription + DataRow + CommandComplete + ReadyForQuery
// ---------------------------------------------------------------------------

// Build a hand-crafted server response buffer with two DataRows.
static std::vector<uint8_t> build_select_response() {
    std::vector<uint8_t> buf;

    auto push_be16 = [&](uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push_be32 = [&](uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push_cstr = [&](const char* s) {
        while (*s) {
            buf.push_back(static_cast<uint8_t>(*s++));
        }
        buf.push_back(0);
    };

    // RowDescription: 2 columns: "id" (int4, oid=23), "name" (text, oid=25).
    {
        std::vector<uint8_t> body;
        auto pb16 = [&](uint16_t v) {
            body.push_back(static_cast<uint8_t>(v >> 8));
            body.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        auto pb32 = [&](uint32_t v) {
            body.push_back(static_cast<uint8_t>(v >> 24));
            body.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        auto pcs = [&](const char* s) {
            while (*s) {
                body.push_back(static_cast<uint8_t>(*s++));
            }
            body.push_back(0);
        };
        pb16(2); // num_cols
        // col "id": name + 4(tableOID) + 2(attrNum) + 4(typeOID) + 2(typeSize) + 4(typeMod) +
        // 2(fmt)
        pcs("id");
        pb32(0);
        pb16(0);
        pb32(23);
        pb16(4);
        pb32(static_cast<uint32_t>(-1));
        pb16(0);
        // col "name"
        pcs("name");
        pb32(0);
        pb16(0);
        pb32(25);
        pb16(-1);
        pb32(static_cast<uint32_t>(-1));
        pb16(0);

        buf.push_back('T');
        uint32_t len = 4 + static_cast<uint32_t>(body.size());
        push_be32(len);
        buf.insert(buf.end(), body.begin(), body.end());
    }

    // DataRow 1: id="1", name="alice"
    {
        std::vector<uint8_t> body;
        auto pb16 = [&](uint16_t v) {
            body.push_back(static_cast<uint8_t>(v >> 8));
            body.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        auto pb32 = [&](uint32_t v) {
            body.push_back(static_cast<uint8_t>(v >> 24));
            body.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        pb16(2); // num_fields
        // field "1"
        const char* f1 = "1";
        pb32(static_cast<uint32_t>(strlen(f1)));
        while (*f1) {
            body.push_back(static_cast<uint8_t>(*f1++));
        }
        // field "alice"
        const char* f2 = "alice";
        pb32(static_cast<uint32_t>(strlen(f2)));
        while (*f2) {
            body.push_back(static_cast<uint8_t>(*f2++));
        }

        buf.push_back('D');
        uint32_t len = 4 + static_cast<uint32_t>(body.size());
        push_be32(len);
        buf.insert(buf.end(), body.begin(), body.end());
    }

    // DataRow 2: id="2", name=NULL
    {
        std::vector<uint8_t> body;
        auto pb16 = [&](uint16_t v) {
            body.push_back(static_cast<uint8_t>(v >> 8));
            body.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        auto pb32 = [&](uint32_t v) {
            body.push_back(static_cast<uint8_t>(v >> 24));
            body.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        pb16(2);
        const char* f1 = "2";
        pb32(static_cast<uint32_t>(strlen(f1)));
        while (*f1) {
            body.push_back(static_cast<uint8_t>(*f1++));
        }
        // NULL field: length = -1 (0xFFFFFFFF)
        pb32(0xFFFFFFFFu);

        buf.push_back('D');
        uint32_t len = 4 + static_cast<uint32_t>(body.size());
        push_be32(len);
        buf.insert(buf.end(), body.begin(), body.end());
    }

    // CommandComplete: "SELECT 2\0"
    {
        const char* tag = "SELECT 2";
        size_t tag_len = strlen(tag) + 1;
        buf.push_back('C');
        push_be32(static_cast<uint32_t>(4 + tag_len));
        while (*tag) {
            buf.push_back(static_cast<uint8_t>(*tag++));
        }
        buf.push_back(0);
    }

    // ReadyForQuery: 'Z' + len=5 + 'I'
    buf.push_back('Z');
    push_be32(5);
    buf.push_back(static_cast<uint8_t>('I'));

    // Suppress unreachable warnings about local lambdas.
    (void)push_be16;
    (void)push_cstr;

    return buf;
}

TEST(CliPgWireCodec, DecodeFullSelectResponse) {
    auto buf = build_select_response();

    size_t consumed = 0;
    size_t total_consumed = 0;

    // Message 1: RowDescription
    auto m1 = decode_one_message(buf, consumed);
    ASSERT_TRUE(m1.has_value()) << m1.error().message;
    EXPECT_EQ(m1->tag, ServerMsgTag::RowDescription);
    ASSERT_EQ(m1->row_desc.columns.size(), 2u);
    EXPECT_EQ(m1->row_desc.columns[0].name, "id");
    EXPECT_EQ(m1->row_desc.columns[0].type_oid, 23);
    EXPECT_EQ(m1->row_desc.columns[1].name, "name");
    EXPECT_EQ(m1->row_desc.columns[1].type_oid, 25);
    total_consumed += consumed;

    auto remaining =
        std::vector<uint8_t>(buf.begin() + static_cast<ptrdiff_t>(total_consumed), buf.end());

    // Message 2: DataRow 1 (id=1, name=alice)
    consumed = 0;
    auto m2 = decode_one_message(remaining, consumed);
    ASSERT_TRUE(m2.has_value()) << m2.error().message;
    EXPECT_EQ(m2->tag, ServerMsgTag::DataRow);
    ASSERT_EQ(m2->data_row.fields.size(), 2u);
    ASSERT_TRUE(m2->data_row.fields[0].has_value());
    EXPECT_EQ(*m2->data_row.fields[0], "1");
    ASSERT_TRUE(m2->data_row.fields[1].has_value());
    EXPECT_EQ(*m2->data_row.fields[1], "alice");
    total_consumed += consumed;

    remaining =
        std::vector<uint8_t>(buf.begin() + static_cast<ptrdiff_t>(total_consumed), buf.end());

    // Message 3: DataRow 2 (id=2, name=NULL)
    consumed = 0;
    auto m3 = decode_one_message(remaining, consumed);
    ASSERT_TRUE(m3.has_value()) << m3.error().message;
    EXPECT_EQ(m3->tag, ServerMsgTag::DataRow);
    ASSERT_EQ(m3->data_row.fields.size(), 2u);
    ASSERT_TRUE(m3->data_row.fields[0].has_value());
    EXPECT_EQ(*m3->data_row.fields[0], "2");
    EXPECT_FALSE(m3->data_row.fields[1].has_value()); // NULL
    total_consumed += consumed;

    remaining =
        std::vector<uint8_t>(buf.begin() + static_cast<ptrdiff_t>(total_consumed), buf.end());

    // Message 4: CommandComplete
    consumed = 0;
    auto m4 = decode_one_message(remaining, consumed);
    ASSERT_TRUE(m4.has_value()) << m4.error().message;
    EXPECT_EQ(m4->tag, ServerMsgTag::CommandComplete);
    EXPECT_EQ(m4->cmd_complete.tag, "SELECT 2");
    total_consumed += consumed;

    remaining =
        std::vector<uint8_t>(buf.begin() + static_cast<ptrdiff_t>(total_consumed), buf.end());

    // Message 5: ReadyForQuery
    consumed = 0;
    auto m5 = decode_one_message(remaining, consumed);
    ASSERT_TRUE(m5.has_value()) << m5.error().message;
    EXPECT_EQ(m5->tag, ServerMsgTag::ReadyForQuery);
    EXPECT_EQ(m5->ready.transaction_status, 'I');
}

// ---------------------------------------------------------------------------
// Decoding: ErrorResponse
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, DecodeErrorResponse) {
    // Build an ErrorResponse: 'E' + len + fields + terminator.
    std::vector<uint8_t> buf;
    auto push_be32_b = [&](uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push_field = [&](char code, const char* value) {
        buf.push_back(static_cast<uint8_t>(code));
        const char* p = value;
        while (*p) {
            buf.push_back(static_cast<uint8_t>(*p++));
        }
        buf.push_back(0);
    };

    // Collect body separately to compute length.
    std::vector<uint8_t> body;
    auto push_field_to = [&](std::vector<uint8_t>& target, char code, const char* value) {
        target.push_back(static_cast<uint8_t>(code));
        const char* p = value;
        while (*p) {
            target.push_back(static_cast<uint8_t>(*p++));
        }
        target.push_back(0);
    };

    push_field_to(body, 'S', "ERROR");
    push_field_to(body, 'C', "42601");
    push_field_to(body, 'M', "syntax error at or near \"SELEKT\"");
    push_field_to(body, 'D', "some detail");
    push_field_to(body, 'H', "check spelling");
    body.push_back(0); // Terminator.

    buf.push_back('E');
    push_be32_b(4 + static_cast<uint32_t>(body.size()));
    buf.insert(buf.end(), body.begin(), body.end());

    size_t consumed = 0;
    auto msg = decode_one_message(buf, consumed);
    ASSERT_TRUE(msg.has_value()) << msg.error().message;
    EXPECT_EQ(msg->tag, ServerMsgTag::ErrorResponse);
    EXPECT_EQ(msg->error_resp.severity, "ERROR");
    EXPECT_EQ(msg->error_resp.sql_state, "42601");
    EXPECT_EQ(msg->error_resp.message, "syntax error at or near \"SELEKT\"");
    EXPECT_EQ(msg->error_resp.detail, "some detail");
    EXPECT_EQ(msg->error_resp.hint, "check spelling");
    EXPECT_EQ(consumed, buf.size());

    // Suppress unused lambda warning.
    (void)push_field;
}

// ---------------------------------------------------------------------------
// Decoding: Incomplete message returns NOT_FOUND
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, IncompleteMessageReturnsNotFound) {
    // Only 3 bytes -- too short for any message header.
    std::vector<uint8_t> buf = {'Z', 0, 0};
    size_t consumed = 0;
    auto msg = decode_one_message(buf, consumed);
    EXPECT_FALSE(msg.has_value());
    EXPECT_EQ(msg.error().code, sixseven::StatusCode::NOT_FOUND);
}

TEST(CliPgWireCodec, PartialBodyReturnsNotFound) {
    // ReadyForQuery header says length=5, but body is missing.
    std::vector<uint8_t> buf = {'Z', 0, 0, 0, 5}; // 5 bytes, but body needs 1 more
    size_t consumed = 0;
    auto msg = decode_one_message(buf, consumed);
    EXPECT_FALSE(msg.has_value());
    EXPECT_EQ(msg.error().code, sixseven::StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Decoding: AuthenticationOk
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, DecodeAuthenticationOk) {
    // 'R' + len=8 + auth_type=0
    std::vector<uint8_t> buf = {'R', 0, 0, 0, 8, 0, 0, 0, 0};
    size_t consumed = 0;
    auto msg = decode_one_message(buf, consumed);
    ASSERT_TRUE(msg.has_value()) << msg.error().message;
    EXPECT_EQ(msg->tag, ServerMsgTag::Authentication);
    EXPECT_EQ(msg->auth.auth_type, 0);
    EXPECT_EQ(consumed, 9u);
}

// ---------------------------------------------------------------------------
// Decoding: ReadyForQuery status codes
// ---------------------------------------------------------------------------

TEST(CliPgWireCodec, DecodeReadyForQueryInTransaction) {
    std::vector<uint8_t> buf = {'Z', 0, 0, 0, 5, static_cast<uint8_t>('T')};
    size_t consumed = 0;
    auto msg = decode_one_message(buf, consumed);
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->tag, ServerMsgTag::ReadyForQuery);
    EXPECT_EQ(msg->ready.transaction_status, 'T');
}
