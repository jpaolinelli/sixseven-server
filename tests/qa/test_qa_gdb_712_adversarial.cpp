/// @file test_qa_gdb_712_adversarial.cpp
/// @brief Adversarial QA tests for GDB-712: Bind parameter format-code decoding.
///
/// The companion file test_qa_gdb_712.cpp proves the happy path and the basic
/// error paths. This file attacks the extended-protocol Bind/Execute path the
/// way a hostile or buggy client would: truncated and over-long Bind frames,
/// out-of-range / negative format codes, zero-parameter Bind messages carrying
/// format codes, binary parameters whose declared type is missing or beyond the
/// OID list, protocol-state recovery after every flavour of error, repeated and
/// interleaved portals, oversized (1 MB) binary text values, and the full set of
/// numeric boundary / NaN / Inf / -0.0 edge cases at the decode layer.
///
/// Wire tests follow the GDB-712 socketpair + ::send/::recv + platform_init()
/// pattern (see test_qa_gdb_712.cpp header) which is REQUIRED on Windows, where
/// socketpair() is emulated with loopback TCP returning raw SOCKET handles.

#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

// platform.h pulls in <winsock2.h>/<windows.h> on Windows; keep it after the
// engine headers so SDK macros (IN/OUT/DELETE) cannot mangle parser/ast.h.
#include "sixseven/common/platform.h"

#if !defined(_WIN32)
#include <sys/time.h> // struct timeval for SO_RCVTIMEO
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

namespace {

// =============================================================================
// Socket helpers (Windows-safe)
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

/// Frame a message body with a 1-byte type tag and the standard 4-byte length
/// (length counts itself + body, never the type tag), matching PostgreSQL v3.
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

/// Well-formed Bind builder: every count field is consistent with the data.
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
    push_be32(body, 0); // max_rows = 0.
    return frame_message('E', body);
}

std::vector<uint8_t> build_sync() {
    return {'S', 0, 0, 0, 4};
}

// =============================================================================
// Binary value encoders (network byte order)
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

int count_messages(const std::vector<uint8_t>& data, uint8_t type) {
    int n = 0;
    size_t pos = 0;
    while (pos + 5 <= data.size()) {
        uint8_t msg_type = data[pos];
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size()) {
            break;
        }
        if (msg_type == type) {
            ++n;
        }
        pos += total;
    }
    return n;
}

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
                ++field_pos;
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
// Fixture: extended-query wire flow with a recording executor stub.
// =============================================================================

class QA_GDB712Adv : public ::testing::Test {
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
            [this](const std::string& sql, const std::string& /*db*/) -> Result<QueryResult> {
                ++executor_calls_;
                received_sql_ = sql;
                received_sqls_.push_back(sql);
                QueryResult qr;
                qr.affected_rows = 1;
                qr.message = "OK";
                return ok(std::move(qr));
            });

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
            uint8_t buf[8192];
            auto n = ::recv(client_fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) {
                ADD_FAILURE() << "recv() failed or timed out before ReadyForQuery";
                break;
            }
            data.insert(data.end(), buf, buf + static_cast<size_t>(n));
        }
        return data;
    }

    /// Parse/Bind/Execute/Sync for a single statement and portal "".
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
    std::vector<std::string> received_sqls_;
    int executor_calls_ = 0;
};

} // namespace

// =============================================================================
// Format-code count semantics under attack
// =============================================================================

// A negative num_format_codes (0xFFFF = -1) must not be treated as a huge
// unsigned reserve count. GDB-712 guards the read with `> 0`, so the field is
// ignored and parameters fall back to text — no crash, no allocation blowup.
TEST_F(QA_GDB712Adv, NegativeNumFormatCodes_IgnoredAndTreatedAsText) {
    std::vector<uint8_t> body;
    push_cstring(body, "");  // portal
    push_cstring(body, "");  // statement
    push_be16(body, 0xFFFF); // num_format_codes = -1 (no codes follow).
    push_be16(body, 1);      // one parameter
    push_be32(body, 2);      // value length 2
    body.push_back('4');
    body.push_back('2');
    push_be16(body, 0); // zero result format codes
    auto bind = frame_message('B', body);

    std::vector<uint8_t> batch;
    auto parse = build_parse("", "SELECT * FROM t WHERE id = $1", {23});
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    auto exec = build_execute("");
    auto sync = build_sync();
    batch.insert(batch.end(), exec.begin(), exec.end());
    batch.insert(batch.end(), sync.begin(), sync.end());
    pump_to_handler(batch);
    auto resp = drain_until_ready();

    EXPECT_EQ(executor_calls_, 1) << "negative format-code count was not handled gracefully";
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE id = 42");
    EXPECT_FALSE(has_message(resp, 'E')) << "SQLSTATE " << extract_sqlstate(resp);
}

// num_format_codes claims many codes but the body is short. read_int16() yields
// 0 for the missing codes, leaving a code count (3) that does not match the
// single parameter, which is rejected at Bind with 08P01. No crash.
TEST_F(QA_GDB712Adv, OverclaimedFormatCodeCount_RejectedAtBindNoCrash) {
    std::vector<uint8_t> body;
    push_cstring(body, "");
    push_cstring(body, "");
    push_be16(body, 3); // claim 3 format codes but provide only 1 below.
    push_be16(body, 1); // single (real) code
    push_be16(body, 1); // num_params = 1 (the reader will mis-split, then fail).
    push_be32(body, 4);
    body.insert(body.end(), {0, 0, 0, 42});
    push_be16(body, 0);
    auto bind = frame_message('B', body);

    std::vector<uint8_t> batch;
    auto parse = build_parse("", "SELECT $1", {23});
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    auto exec = build_execute("");
    auto sync = build_sync();
    batch.insert(batch.end(), exec.begin(), exec.end());
    batch.insert(batch.end(), sync.begin(), sync.end());
    pump_to_handler(batch);
    auto resp = drain_until_ready();

    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "08P01");
    EXPECT_TRUE(has_message(resp, 'Z')); // Sync still completes.
}

// Zero parameters but one format code: PostgreSQL accepts (the lone code simply
// applies to nothing). Must not error, must execute the parameterless SQL.
TEST_F(QA_GDB712Adv, ZeroParamsOneFormatCode_Accepted) {
    auto resp = run_extended("SELECT 1", {}, {1}, {});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT 1");
    EXPECT_FALSE(has_message(resp, 'E')) << extract_sqlstate(resp);
}

// Zero parameters but two format codes: count > 1 and != 0 params → 08P01.
TEST_F(QA_GDB712Adv, ZeroParamsTwoFormatCodes_RejectedAtBind) {
    auto resp = run_extended("SELECT 1", {}, {1, 0}, {});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "08P01");
}

// Format code 1 (binary) for $1 but the per-parameter list has an out-of-range
// value (5) for $2: count matches param count so Bind accepts, but Execute
// rejects the bogus code at decode time with 22023.
TEST_F(QA_GDB712Adv, PerParamOutOfRangeFormatCode_RejectedAtExecute) {
    auto resp = run_extended("SELECT $1, $2",
                             {23, 23},
                             {1, 5}, // $2 has an undefined format code.
                             {be_bytes32(1), be_bytes32(2)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

// A negative single format code (0xFFFF = -1) applies to all parameters and is
// neither text(0) nor binary(1): rejected at Execute with 22023.
TEST_F(QA_GDB712Adv, NegativeFormatCodeValue_Rejected) {
    auto resp = run_extended("SELECT $1", {23}, {-1}, {be_bytes32(1)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

// =============================================================================
// Truncated / malformed parameter values inside a well-framed Bind
// =============================================================================

// A parameter whose declared length exceeds the entire remaining Bind body:
// MessageReader::read_bytes() refuses the out-of-bounds read and returns
// nullptr, so handle_bind records the value as SQL NULL. The decode step passes
// NULL through. The point of this test is that an over-long length triggers NO
// out-of-bounds read and NO crash — it degrades safely to NULL.
TEST_F(QA_GDB712Adv, OverlongBinaryValueLength_DegradesToNullNotOob) {
    std::vector<uint8_t> body;
    push_cstring(body, "");
    push_cstring(body, "");
    push_be16(body, 1);          // one format code
    push_be16(body, 1);          // binary
    push_be16(body, 1);          // one parameter
    push_be32(body, 0x00100000); // claim 1 MiB; no value bytes actually follow.
    auto bind = frame_message('B', body);

    std::vector<uint8_t> batch;
    auto parse = build_parse("", "UPDATE t SET id = $1", {23});
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind.begin(), bind.end());
    auto exec = build_execute("");
    auto sync = build_sync();
    batch.insert(batch.end(), exec.begin(), exec.end());
    batch.insert(batch.end(), sync.begin(), sync.end());
    pump_to_handler(batch);
    auto resp = drain_until_ready();

    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "UPDATE t SET id = NULL")
        << "over-long value length should degrade to NULL via the read_bytes guard";
    EXPECT_FALSE(has_message(resp, 'E'));
    EXPECT_TRUE(has_message(resp, 'Z'));
}

// =============================================================================
// OID-vs-parameter index mismatches
// =============================================================================

// Two binary parameters but Parse declared only one OID. The missing $2 OID
// defaults to 0 (unspecified) which cannot be binary-decoded → 22023.
TEST_F(QA_GDB712Adv, BinaryParamBeyondDeclaredOids_Rejected) {
    auto resp = run_extended(
        "SELECT $1, $2", {23 /*only one OID*/}, {1, 1}, {be_bytes32(1), be_bytes32(2)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

// =============================================================================
// Binary bool: any nonzero byte is true; multi-byte is a length error
// =============================================================================

TEST_F(QA_GDB712Adv, BinaryBoolByte0x02_DecodesTrue) {
    auto resp = run_extended("SELECT * FROM t WHERE a = $1", {16}, {1}, {std::string("\x02", 1)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE a = TRUE");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712Adv, BinaryBoolByte0xFF_DecodesTrue) {
    auto resp = run_extended("SELECT * FROM t WHERE a = $1", {16}, {1}, {std::string("\xff", 1)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT * FROM t WHERE a = TRUE");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712Adv, BinaryBoolTwoBytes_RejectedAsLengthError) {
    auto resp = run_extended("SELECT $1", {16}, {1}, {std::string("\x00\x01", 2)});
    EXPECT_EQ(executor_calls_, 0);
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

// =============================================================================
// Integer boundary values over the wire
// =============================================================================

TEST_F(QA_GDB712Adv, BinaryInt8Min_DecodedExactly) {
    auto resp = run_extended("SELECT $1", {20}, {1}, {be_bytes64(0x8000000000000000ULL)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT -9223372036854775808");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712Adv, BinaryInt8Max_DecodedExactly) {
    auto resp = run_extended("SELECT $1", {20}, {1}, {be_bytes64(0x7FFFFFFFFFFFFFFFULL)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT 9223372036854775807");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712Adv, BinaryInt2Min_DecodedExactly) {
    auto resp = run_extended("SELECT $1", {21}, {1}, {be_bytes16(0x8000u)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT -32768");
    EXPECT_FALSE(has_message(resp, 'E'));
}

// =============================================================================
// float edge cases: -0.0 round-trips; NaN/Inf are decoded then rejected at
// substitution (documented pre-existing literal limitation, see review notes).
// =============================================================================

TEST_F(QA_GDB712Adv, BinaryFloat8NegativeZero_DecodedAsNegZero) {
    auto resp = run_extended("SELECT $1", {701}, {1}, {be_float8(-0.0)});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT -0");
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712Adv, BinaryFloat4Denormal_RoundTripsThroughDecode) {
    // Smallest positive subnormal float: must decode to a parseable literal and
    // reach the executor (no rejection, no crash).
    float denorm = std::numeric_limits<float>::denorm_min();
    auto resp = run_extended("SELECT $1", {700}, {1}, {be_float4(denorm)});
    EXPECT_EQ(executor_calls_, 1) << "denormal float4 was rejected";
    EXPECT_FALSE(has_message(resp, 'E'));
}

TEST_F(QA_GDB712Adv, BinaryFloat8NaN_RejectedAtSubstitution) {
    auto resp = run_extended(
        "SELECT $1", {701}, {1}, {be_float8(std::numeric_limits<double>::quiet_NaN())});
    EXPECT_EQ(executor_calls_, 0) << "NaN literal reached the executor: " << received_sql_;
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

TEST_F(QA_GDB712Adv, BinaryFloat8Inf_RejectedAtSubstitution) {
    auto resp =
        run_extended("SELECT $1", {701}, {1}, {be_float8(std::numeric_limits<double>::infinity())});
    EXPECT_EQ(executor_calls_, 0) << "Inf literal reached the executor: " << received_sql_;
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "22023");
}

// =============================================================================
// Text/JSON binary passthrough: injection safety must survive the binary path
// =============================================================================

// Binary text containing single quotes AND a NUL byte: quotes must be doubled
// by the SQL escaper; the value must not be able to break out of the literal.
TEST_F(QA_GDB712Adv, BinaryTextWithQuotesAndNul_EscapedNoInjection) {
    std::string payload = std::string("a'b", 3) + std::string("\x00", 1) + "'; DROP TABLE t;--";
    auto resp = run_extended("INSERT INTO t (v) VALUES ($1)", {25}, {1}, {payload});
    EXPECT_EQ(executor_calls_, 1);
    // Every single quote in the payload must appear doubled in the SQL, and the
    // statement must remain a single INSERT (no second statement smuggled in).
    EXPECT_NE(received_sql_.find("''"), std::string::npos)
        << "single quotes were not escaped: " << received_sql_;
    EXPECT_EQ(received_sql_.rfind("INSERT INTO t (v) VALUES ('", 0), 0u);
    EXPECT_FALSE(has_message(resp, 'E'));
}

// Binary JSON with embedded double quotes and an embedded single quote: the
// JSON OID takes the text-like passthrough but the outer SQL literal is still
// single-quote-escaped.
TEST_F(QA_GDB712Adv, BinaryJsonWithEmbeddedQuotes_EscapedAndQuoted) {
    auto resp = run_extended(
        "INSERT INTO t (j) VALUES ($1)", {114}, {1}, {std::string(R"({"k":"o'brien"})")});
    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, R"(INSERT INTO t (j) VALUES ('{"k":"o''brien"}'))");
    EXPECT_FALSE(has_message(resp, 'E'));
}

// A 1 MB binary text value must pass through intact (length-prefixed, not a
// C-string) and reach the executor without truncation or crash.
TEST_F(QA_GDB712Adv, LargeBinaryTextValue_OneMegabyte_Passthrough) {
    std::string big(1024u * 1024u, 'x');
    auto resp = run_extended("INSERT INTO t (v) VALUES ($1)", {25}, {1}, {big});
    EXPECT_EQ(executor_calls_, 1);
    // 1 MB of 'x', wrapped in quotes, none of which need escaping.
    ASSERT_FALSE(received_sql_.empty());
    EXPECT_EQ(received_sql_.size(),
              std::string("INSERT INTO t (v) VALUES ('").size() + big.size() +
                  std::string("')").size());
    EXPECT_FALSE(has_message(resp, 'E'));
}

// =============================================================================
// Protocol state machine: recovery after every error flavour
// =============================================================================

// Bind references a prepared statement that does not exist: 26000, and the
// follow-up Execute against the (never-created) portal must be suppressed
// (error_in_extended_), then Sync recovers the connection to ReadyForQuery.
TEST_F(QA_GDB712Adv, BindToNonexistentStatement_ExecuteSuppressed_SyncRecovers) {
    std::vector<uint8_t> batch;
    auto bind = build_bind("p", "no_such_stmt", {1}, {be_bytes32(1)});
    auto exec = build_execute("p");
    auto sync = build_sync();
    batch.insert(batch.end(), bind.begin(), bind.end());
    batch.insert(batch.end(), exec.begin(), exec.end());
    batch.insert(batch.end(), sync.begin(), sync.end());
    pump_to_handler(batch);
    auto resp = drain_until_ready();

    EXPECT_EQ(executor_calls_, 0) << "Execute ran after a failed Bind";
    EXPECT_TRUE(has_message(resp, 'E'));
    EXPECT_EQ(extract_sqlstate(resp), "26000");
    EXPECT_TRUE(has_message(resp, 'Z'));
}

// After a decode error (unsupported binary OID) and Sync, a fresh, valid
// extended batch on the same connection must succeed — the error state does not
// poison subsequent batches.
TEST_F(QA_GDB712Adv, RecoversAfterDecodeError_NextBatchSucceeds) {
    auto bad = run_extended("SELECT $1", {1114 /*timestamp*/}, {1}, {be_bytes64(1)});
    ASSERT_TRUE(has_message(bad, 'E'));
    ASSERT_EQ(extract_sqlstate(bad), "22023");
    ASSERT_EQ(executor_calls_, 0);

    auto good = run_extended("SELECT $1", {23}, {1}, {be_bytes32(99)});
    EXPECT_EQ(executor_calls_, 1) << "connection did not recover after a decode error";
    EXPECT_EQ(received_sql_, "SELECT 99");
    EXPECT_FALSE(has_message(good, 'E'));
}

// =============================================================================
// Repeated and interleaved portals
// =============================================================================

// Binding the same portal name twice replaces the first portal; Execute must
// see the second Bind's decoded value.
TEST_F(QA_GDB712Adv, RepeatedBindSamePortal_SecondBindWins) {
    std::vector<uint8_t> batch;
    auto parse = build_parse("s", "SELECT $1", {23});
    auto bind1 = build_bind("p", "s", {1}, {be_bytes32(111)});
    auto bind2 = build_bind("p", "s", {1}, {be_bytes32(222)});
    auto exec = build_execute("p");
    auto sync = build_sync();
    batch.insert(batch.end(), parse.begin(), parse.end());
    batch.insert(batch.end(), bind1.begin(), bind1.end());
    batch.insert(batch.end(), bind2.begin(), bind2.end());
    batch.insert(batch.end(), exec.begin(), exec.end());
    batch.insert(batch.end(), sync.begin(), sync.end());
    pump_to_handler(batch);
    auto resp = drain_until_ready();

    EXPECT_EQ(executor_calls_, 1);
    EXPECT_EQ(received_sql_, "SELECT 222") << "second Bind did not overwrite the portal";
    EXPECT_FALSE(has_message(resp, 'E'));
}

// Two prepared statements, two portals, both binary-bound, executed in one
// batch. Each portal must decode independently and both must execute.
TEST_F(QA_GDB712Adv, InterleavedTwoPortals_BothDecodeIndependently) {
    std::vector<uint8_t> batch;
    auto p1 = build_parse("s1", "SELECT $1", {23});
    auto p2 = build_parse("s2", "SELECT $1", {20});
    auto b1 = build_bind("port1", "s1", {1}, {be_bytes32(7)});
    auto b2 = build_bind("port2", "s2", {1}, {be_bytes64(8000000000ULL)});
    auto e1 = build_execute("port1");
    auto e2 = build_execute("port2");
    auto sync = build_sync();
    for (const auto* m : {&p1, &p2, &b1, &b2, &e1, &e2, &sync}) {
        batch.insert(batch.end(), m->begin(), m->end());
    }
    pump_to_handler(batch);
    auto resp = drain_until_ready();

    EXPECT_EQ(executor_calls_, 2);
    ASSERT_EQ(received_sqls_.size(), 2u);
    EXPECT_EQ(received_sqls_[0], "SELECT 7");
    EXPECT_EQ(received_sqls_[1], "SELECT 8000000000");
    EXPECT_EQ(count_messages(resp, 'C'), 2); // two CommandComplete.
    EXPECT_FALSE(has_message(resp, 'E'));
}

// =============================================================================
// Decode-layer boundary tests (no wire; exercise the pure functions directly)
// =============================================================================

TEST(QA_GDB712Decode, Float8NaNDecodesToNonNumericThenSubstitutionRejects) {
    auto bits = be_float8(std::numeric_limits<double>::quiet_NaN());
    auto decoded = decode_binary_parameter(bits, 701);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    // decode itself does not validate; substitution must reject the non-numeric.
    auto sql = substitute_parameters("SELECT $1", {*decoded}, {701});
    ASSERT_FALSE(sql.has_value());
    EXPECT_EQ(sql.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB712Decode, Float4NegativeZeroDecodesToNegZero) {
    auto r = decode_binary_parameter(be_float4(-0.0F), 700);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "-0");
}

TEST(QA_GDB712Decode, TextPassthroughPreservesEmbeddedNulAndLength) {
    std::string raw = std::string("a\0b", 3);
    auto r = decode_binary_parameter(raw, 25);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 3u) << "embedded NUL truncated the text passthrough";
    EXPECT_EQ(*r, raw);
}

TEST(QA_GDB712Decode, EmptyTextPassthroughIsEmptyNotError) {
    auto r = decode_binary_parameter(std::string(), 25);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

TEST(QA_GDB712Decode, Int4ExtraByteRejected) {
    auto r = decode_binary_parameter(std::string("\x00\x00\x00\x00\x2a", 5), 23);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB712Decode, NumericOidBinaryRejectedNotSpliced) {
    // NUMERIC (1700) has a complex binary wire format we cannot decode; it must
    // be rejected rather than splicing raw bytes as a numeric literal.
    auto r = decode_binary_parameter(be_bytes64(1), 1700);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}
