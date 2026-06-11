/// @file test_qa_gdb_712.cpp
/// @brief QA regression tests for GDB-712: Bind parameter format codes parsed
/// then discarded — binary-format parameters are spliced into SQL as raw text.
///
/// Audit finding H18 (severity high). handle_bind read the parameter format
/// codes from the Bind message into a local vector that was never used again:
/// it was not stored in the Portal and not consulted when substituting
/// parameter values. Every parameter was treated as text, so a client sending
/// format code 1 (binary) — pgJDBC, Npgsql, any driver using binary transfer
/// for int/float/bool — had its raw big-endian bytes spliced into the SQL as
/// a text literal. An int4 value 42 arrived as "\x00\x00\x00\x2a" and either
/// failed numeric-literal validation or was quoted as a garbage string.
///
/// The fix stores the format codes in the Portal and decodes binary-format
/// parameters per their OID (bool/int2/int4/int8/float4/float8, plus
/// text-like passthrough) into text before substitution, rejecting format
/// code 1 for OIDs that cannot be decoded.
///
/// These tests drive the real wire protocol: they build Parse/Bind/Execute/
/// Sync frontend messages, push them through PgProtocolHandler over a
/// socketpair, and observe the SQL that reaches the query executor.
///
/// NOTE: unlike older wire-protocol tests, the helpers here use ::send/::recv
/// (not the CRT ::write/::read) and call sixseven::platform_init() so they
/// also run on Windows, where socketpair() is emulated with loopback TCP and
/// returns raw SOCKET handles that are not CRT file descriptors.

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

// platform.h pulls in <winsock2.h>/<windows.h> on Windows; keep it after the
// engine headers so SDK macros (IN/OUT/DELETE) cannot mangle parser/ast.h
// identifiers (same ordering as test_param_substitution.cpp).
#include "sixseven/common/platform.h"

#if !defined(_WIN32)
#include <sys/time.h> // struct timeval for SO_RCVTIMEO
#endif

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

namespace {

// =============================================================================
// Socket helpers (Windows-safe: ::send/::recv on socketpair fds)
// =============================================================================

bool ensure_platform_init() {
    static const bool initialized = sixseven::platform_init();
    return initialized;
}

void set_recv_timeout_ms(int fd, int ms) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(static_cast<SOCKET>(fd),
                 SOL_SOCKET,
                 SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
#else
    struct timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void send_all(int fd, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd,
                        reinterpret_cast<const char*>(data.data() + sent),
                        static_cast<int>(data.size() - sent),
                        0);
        ASSERT_GT(n, 0) << "send() failed";
        sent += static_cast<size_t>(n);
    }
}

// =============================================================================
// Frontend message builders
// =============================================================================

void push_be16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void push_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void push_cstring(std::vector<uint8_t>& buf, std::string_view s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

std::vector<uint8_t> frame_message(uint8_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> msg;
    msg.push_back(type);
    push_be32(msg, static_cast<uint32_t>(4 + body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t> build_startup(const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> body;
    push_be32(body, 196608); // Protocol version 3.0.
    for (const auto& [key, val] : params) {
        push_cstring(body, key);
        push_cstring(body, val);
    }
    body.push_back(0);
    std::vector<uint8_t> msg;
    push_be32(msg, static_cast<uint32_t>(4 + body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

std::vector<uint8_t> build_parse(std::string_view stmt_name,
                                 std::string_view sql,
                                 const std::vector<uint32_t>& param_oids) {
    std::vector<uint8_t> body;
    push_cstring(body, stmt_name);
    push_cstring(body, sql);
    push_be16(body, static_cast<uint16_t>(param_oids.size()));
    for (uint32_t oid : param_oids) {
        push_be32(body, oid);
    }
    return frame_message('P', body);
}

/// Build a Bind message with explicit parameter format codes.
/// Use std::nullopt in param_values to send a NULL parameter (length = -1).
std::vector<uint8_t> build_bind(std::string_view portal_name,
                                std::string_view stmt_name,
                                const std::vector<int16_t>& format_codes,
                                const std::vector<std::optional<std::string>>& param_values) {
    std::vector<uint8_t> body;
    push_cstring(body, portal_name);
    push_cstring(body, stmt_name);
    push_be16(body, static_cast<uint16_t>(format_codes.size()));
    for (int16_t code : format_codes) {
        push_be16(body, static_cast<uint16_t>(code));
    }
    push_be16(body, static_cast<uint16_t>(param_values.size()));
    for (const auto& val : param_values) {
        if (!val.has_value()) {
            push_be32(body, 0xFFFFFFFFu); // NULL: length = -1.
        } else {
            push_be32(body, static_cast<uint32_t>(val->size()));
            body.insert(body.end(), val->begin(), val->end());
        }
    }
    push_be16(body, 0); // Zero result format codes.
    return frame_message('B', body);
}

std::vector<uint8_t> build_execute(std::string_view portal_name) {
    std::vector<uint8_t> body;
    push_cstring(body, portal_name);
    push_be32(body, 0); // max_rows = 0 (no limit).
    return frame_message('E', body);
}

std::vector<uint8_t> build_sync() {
    return {'S', 0, 0, 0, 4};
}

// =============================================================================
// Binary parameter value encoders (network byte order, as a real driver sends)
// =============================================================================

std::string be_bytes16(uint16_t v) {
    std::string s(2, '\0');
    s[0] = static_cast<char>((v >> 8) & 0xFF);
    s[1] = static_cast<char>(v & 0xFF);
    return s;
}

std::string be_bytes32(uint32_t v) {
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) {
        s[static_cast<size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xFF);
    }
    return s;
}

std::string be_bytes64(uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) {
        s[static_cast<size_t>(i)] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
    }
    return s;
}

std::string be_float4(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return be_bytes32(bits);
}

std::string be_float8(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return be_bytes64(bits);
}

// =============================================================================
// Backend message scanning
// =============================================================================

/// Return true if `data` contains a complete message of the given type.
bool has_message(const std::vector<uint8_t>& data, uint8_t type) {
    size_t pos = 0;
    while (pos + 5 <= data.size()) {
        uint8_t msg_type = data[pos];
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size()) {
            return false;
        }
        if (msg_type == type) {
            return true;
        }
        pos += total;
    }
    return false;
}

/// Extract the SQLSTATE ('C' field) of the first ErrorResponse in `data`.
/// Returns an empty string if no ErrorResponse is present.
std::string extract_sqlstate(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    while (pos + 5 <= data.size()) {
        uint8_t msg_type = data[pos];
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size()) {
            return {};
        }
        if (msg_type == 'E') {
            size_t field_pos = pos + 5;
            size_t end = pos + total;
            while (field_pos < end && data[field_pos] != 0) {
                uint8_t field_type = data[field_pos++];
                std::string value;
                while (field_pos < end && data[field_pos] != 0) {
                    value += static_cast<char>(data[field_pos++]);
                }
                ++field_pos; // Skip the value's NUL terminator.
                if (field_type == 'C') {
                    return value;
                }
            }
            return {};
        }
        pos += total;
    }
    return {};
}

// =============================================================================
// Fixture: full extended-query wire flow with a recording executor stub
// =============================================================================

class QA_GDB712 : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(ensure_platform_init());

        int fds[2] = {-1, -1};
        ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
            << "socketpair() failed";
        client_fd_ = fds[0];
        set_recv_timeout_ms(client_fd_, 5000);

        conn_ = std::make_unique<Connection>(fds[1]);
        handler_ = std::make_unique<PgProtocolHandler>(712);
        handler_->set_query_executor(
            [this](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
                ++executor_calls_;
                received_sql_ = sql;
                QueryResult qr;
                qr.affected_rows = 1;
                qr.message = "INSERT";
                return ok(std::move(qr));
            });

        // Startup handshake (trust auth): expect ReadyForQuery.
        auto startup = build_startup({{"user", "qa"}, {"database", "demo"}});
        pump_to_handler(startup);
        ASSERT_EQ(handler_->state(), ProtocolState::READY);
        auto greeting = drain_until_ready();
        ASSERT_TRUE(has_message(greeting, 'Z'));
    }

    void TearDown() override {
        if (conn_) {
            conn_->close();
        }
        if (client_fd_ >= 0) {
            sixseven_platform::socket_close(client_fd_);
            client_fd_ = -1;
        }
    }

    /// Send raw frontend bytes, feed them into the handler, and process them.
    void pump_to_handler(const std::vector<uint8_t>& bytes) {
        send_all(client_fd_, bytes);
        size_t total = 0;
        while (total < bytes.size()) {
            auto r = conn_->read_from_socket();
            ASSERT_TRUE(r.has_value()) << r.error().message;
            ASSERT_TRUE(r->has_value()) << "unexpected EAGAIN on blocking socket";
            ASSERT_GT(**r, 0u) << "peer closed unexpectedly";
            total += **r;
        }
        auto pr = handler_->process(*conn_);
        ASSERT_TRUE(pr.has_value()) << pr.error().message;
    }

    /// Flush the handler's responses and read them back on the client side
    /// until the terminating ReadyForQuery ('Z') message arrives.
    std::vector<uint8_t> drain_until_ready() {
        while (conn_->has_pending_writes()) {
            auto w = conn_->write_to_socket();
            if (!w.has_value()) {
                ADD_FAILURE() << "write_to_socket failed: " << w.error().message;
                break;
            }
        }
        std::vector<uint8_t> data;
        while (!has_message(data, 'Z')) {
            uint8_t buf[4096];
            auto n = ::recv(client_fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) {
                ADD_FAILURE() << "recv() failed or timed out before ReadyForQuery";
                break;
            }
            data.insert(data.end(), buf, buf + static_cast<size_t>(n));
        }
        return data;
    }

    /// Run one Parse/Bind/Execute/Sync batch and return the backend response.
    std::vector<uint8_t> run_extended(const std::string& sql,
                                      const std::vector<uint32_t>& param_oids,
                                      const std::vector<int16_t>& format_codes,
                                      const std::vector<std::optional<std::string>>& params) {
        std::vector<uint8_t> batch;
        auto append = [&batch](const std::vector<uint8_t>& msg) {
            batch.insert(batch.end(), msg.begin(), msg.end());
        };
        append(build_parse("", sql, param_oids));
        append(build_bind("", "", format_codes, params));
        append(build_execute(""));
        append(build_sync());
        pump_to_handler(batch);
        return drain_until_ready();
    }

    int client_fd_ = -1;
    std::unique_ptr<Connection> conn_;
    std::unique_ptr<PgProtocolHandler> handler_;
    std::string received_sql_;
    int executor_calls_ = 0;
};

} // namespace

// =============================================================================
// AC 1: Regression — the exact H18 failure mode. A binary int4 parameter
// (format code 1) must reach the executor as the decoded integer, not as the
// raw big-endian bytes "\x00\x00\x00\x2a" treated as text.
// =============================================================================

TEST_F(QA_GDB712, BinaryInt4Param_DecodedToInteger) {
    auto resp = run_extended(
        "SELECT * FROM t WHERE id = $1", {23 /*int4*/}, {1 /*binary*/}, {be_bytes32(42)});
    EXPECT_EQ(executor_calls_, 1) << "binary int4 parameter was rejected instead of decoded";
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE id = 42");
    EXPECT_FALSE(has_message(resp, 'E'))
        << "unexpected ErrorResponse, SQLSTATE " << extract_sqlstate(resp);
}

TEST_F(QA_GDB712, BinaryInt4NegativeParam_DecodedWithSign) {
    auto resp = run_extended(
        "SELECT * FROM t WHERE delta = $1", {23}, {1}, {be_bytes32(static_cast<uint32_t>(-7))});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE delta = -7");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryInt2Param_Decoded) {
    auto resp =
        run_extended("SELECT * FROM t WHERE qty = $1", {21 /*int2*/}, {1}, {be_bytes16(300)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE qty = 300");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryInt8Param_Decoded) {
    auto resp = run_extended(
        "SELECT * FROM t WHERE big = $1", {20 /*int8*/}, {1}, {be_bytes64(5000000000ULL)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE big = 5000000000");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryFloat4Param_Decoded) {
    auto resp =
        run_extended("SELECT * FROM t WHERE x = $1", {700 /*float4*/}, {1}, {be_float4(1.5F)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE x = 1.5");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryFloat8Param_Decoded) {
    auto resp =
        run_extended("SELECT * FROM t WHERE y = $1", {701 /*float8*/}, {1}, {be_float8(2.5)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE y = 2.5");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryBoolParamTrue_DecodedToTrue) {
    auto resp = run_extended(
        "SELECT * FROM t WHERE active = $1", {16 /*bool*/}, {1}, {std::string("\x01", 1)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE active = TRUE");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryBoolParamFalse_DecodedToFalse) {
    auto resp =
        run_extended("SELECT * FROM t WHERE active = $1", {16}, {1}, {std::string("\x00", 1)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE active = FALSE");
    EXPECT_FALSE(has_message(resp, 'E'));
}

// =============================================================================
// PostgreSQL format-code semantics: one code applies to ALL parameters;
// N codes are per-parameter; zero codes mean all-text (existing behavior).
// =============================================================================

TEST_F(QA_GDB712, SingleFormatCodeAppliesToAllParameters) {
    auto resp = run_extended("SELECT * FROM t WHERE a = $1 AND b = $2",
                             {23, 23},
                             {1}, // ONE format code: applies to both parameters.
                             {be_bytes32(7), be_bytes32(9)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE a = 7 AND b = 9");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, PerParameterFormatCodes_MixedBinaryAndText) {
    auto resp = run_extended("INSERT INTO t (id, name) VALUES ($1, $2)",
                             {23, 25 /*text*/},
                             {1, 0}, // $1 binary, $2 text.
                             {be_bytes32(42), std::string("alice")});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "INSERT INTO t (id, name) VALUES (42, 'alice')");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, ZeroFormatCodes_TextBehaviorUnchanged) {
    auto resp = run_extended("SELECT * FROM t WHERE id = $1", {23}, {}, {std::string("42")});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE id = 42");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, BinaryTextParam_PassthroughStillEscaped) {
    // Binary representation of text equals its text form; escaping must still
    // apply so binary text cannot smuggle quotes into the SQL.
    auto resp =
        run_extended("INSERT INTO t (name) VALUES ($1)", {25}, {1}, {std::string("it's a test")});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "INSERT INTO t (name) VALUES ('it''s a test')");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712, NullBinaryParam_SubstitutedAsNull) {
    auto resp = run_extended("UPDATE t SET name = $1 WHERE id = $2",
                             {25, 23},
                             {1}, // Binary format for all params; $1 is NULL.
                             {std::nullopt, be_bytes32(5)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "UPDATE t SET name = NULL WHERE id = 5");
    EXPECT_FALSE(has_message(resp, 'E'));
}

// =============================================================================
// AC 2: The format codes are no longer discarded — they are stored in the
// Portal created by Bind.
// =============================================================================

TEST_F(QA_GDB712, PortalStoresParamFormatCodes) {
    std::vector<uint8_t> batch;
    auto parse = build_parse("s712", "SELECT $1, $2", {23, 25});
    auto bind = build_bind("p712", "s712", {1, 0}, {be_bytes32(1), std::string("x")});
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    pump_to_handler(batch);

    const auto& portals = handler_->portals();
    auto it = portals.find("p712");
    ASSERT_NE(it, portals.end());
    ASSERT_EQ(it->second.param_format_codes.size(), 2u)
        << "Bind discarded the parameter format codes (GDB-712 regression)";
    EXPECT_EQ(it->second.param_format_codes[0], 1);
    EXPECT_EQ(it->second.param_format_codes[1], 0);
}

// =============================================================================
// Error handling: undecodable binary parameters are rejected with an explicit
// error instead of being spliced into the SQL as garbage text.
// =============================================================================

TEST_F(QA_GDB712, BinaryUnsupportedOid_RejectedWithExplicitError) {
    // TIMESTAMP (oid 1114) binary decoding is not supported: must error, and
    // the executor must never see raw timestamp bytes as a text literal.
    auto resp = run_extended("SELECT * FROM t WHERE ts = $1",
                             {1114 /*timestamp*/},
                             {1},
                             {be_bytes64(0x0002000300040005ULL)});
    EXPECT_EQ(executor_calls_, 0) << "executor received undecoded binary bytes: " << received_sql_;
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023"); // invalid_parameter_value.
    EXPECT_TRUE(has_message(resp, 'Z'));        // Sync still completes the batch.
}

TEST_F(QA_GDB712, BinaryUnspecifiedOid_RejectedWithExplicitError) {
    // OID 0 (unspecified type) cannot be binary-decoded: must error.
    auto resp = run_extended("SELECT * FROM t WHERE v = $1", {0}, {1}, {be_bytes32(1)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

TEST_F(QA_GDB712, BinaryInt4WrongLength_Rejected) {
    auto resp =
        run_extended("SELECT * FROM t WHERE id = $1", {23}, {1}, {std::string("\x00\x00\x2a", 3)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

TEST_F(QA_GDB712, FormatCodeCountMismatch_RejectedAtBind) {
    // Two format codes for one parameter is a protocol violation (08P01).
    auto resp = run_extended("SELECT * FROM t WHERE id = $1", {23}, {1, 1}, {be_bytes32(42)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "08P01");
    EXPECT_TRUE(has_message(resp, 'Z'));
}

TEST_F(QA_GDB712, InvalidFormatCodeValue_Rejected) {
    // Format codes other than 0 (text) and 1 (binary) do not exist.
    auto resp = run_extended("SELECT * FROM t WHERE id = $1", {23}, {2}, {std::string("42")});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}
