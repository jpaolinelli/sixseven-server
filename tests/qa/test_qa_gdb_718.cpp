/// @file test_qa_gdb_718.cpp
/// @brief QA regression tests for GDB-718: value_to_pg_binary emits text
/// bytes for most types while DataRow declares binary format.
///
/// Audit finding H19 (severity high). value_to_pg_binary handled BOOL,
/// INT8-INT64, UINT8-UINT32, FLOAT32/64 and STRING, then fell back to
/// value_to_pg_text for everything else — and send_data_row shipped those
/// text bytes whenever the client requested result format code 1, with no
/// error. A binary client reading a TIMESTAMP/UUID/NUMERIC column got text
/// bytes labeled as binary: a decode error at best, a silently wrong value
/// at worst.
///
/// The fix implements the real PostgreSQL binary encodings for every
/// advertised OID (date/time/timestamp as PG-epoch integers, uuid/bytea as
/// raw bytes, uint64 as numeric digit groups, interval, point, json/path as
/// text payload per their OIDs, embedding in pgvector wire format) and
/// rejects result format code 1 with an ErrorResponse (SQLSTATE 0A000) for
/// DECIMAL, whose Value carries no scale and therefore has no faithful
/// numeric encoding.
///
/// These tests drive the real wire protocol: Parse/Bind(result format)/
/// Execute/Sync through PgProtocolHandler over a socketpair, then parse the
/// DataRow fields the client receives.
///
/// NOTE: helpers use ::send/::recv and sixseven::platform_init() so they
/// also run on Windows (socketpair() is emulated with loopback TCP), same
/// as test_qa_gdb_712.cpp.

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

// platform.h pulls in <winsock2.h>/<windows.h> on Windows; keep it after the
// engine headers so SDK macros (IN/OUT/DELETE) cannot mangle parser/ast.h
// identifiers (same ordering as test_qa_gdb_712.cpp).
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

std::vector<uint8_t> build_parse(std::string_view stmt_name, std::string_view sql) {
    std::vector<uint8_t> body;
    push_cstring(body, stmt_name);
    push_cstring(body, sql);
    push_be16(body, 0); // No parameters.
    return frame_message('P', body);
}

/// Build a Bind message with no parameters and the given RESULT format codes.
std::vector<uint8_t> build_bind_with_result_formats(std::string_view portal_name,
                                                    std::string_view stmt_name,
                                                    const std::vector<int16_t>& result_formats) {
    std::vector<uint8_t> body;
    push_cstring(body, portal_name);
    push_cstring(body, stmt_name);
    push_be16(body, 0); // Zero parameter format codes.
    push_be16(body, 0); // Zero parameters.
    push_be16(body, static_cast<uint16_t>(result_formats.size()));
    for (int16_t code : result_formats) {
        push_be16(body, static_cast<uint16_t>(code));
    }
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

/// Parse the fields of the first DataRow ('D') message in `data`.
/// Each field is nullopt for NULL (-1 length) or the raw field bytes.
std::vector<std::optional<std::vector<uint8_t>>>
extract_first_data_row(const std::vector<uint8_t>& data) {
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
        if (msg_type == 'D') {
            std::vector<std::optional<std::vector<uint8_t>>> fields;
            size_t p = pos + 5;
            uint16_t ncols =
                static_cast<uint16_t>((static_cast<uint16_t>(data[p]) << 8) | data[p + 1]);
            p += 2;
            for (uint16_t c = 0; c < ncols; ++c) {
                auto field_len = static_cast<int32_t>((static_cast<uint32_t>(data[p]) << 24) |
                                                      (static_cast<uint32_t>(data[p + 1]) << 16) |
                                                      (static_cast<uint32_t>(data[p + 2]) << 8) |
                                                      static_cast<uint32_t>(data[p + 3]));
                p += 4;
                if (field_len < 0) {
                    fields.emplace_back(std::nullopt);
                } else {
                    fields.emplace_back(
                        std::vector<uint8_t>(data.begin() + static_cast<ptrdiff_t>(p),
                                             data.begin() + static_cast<ptrdiff_t>(p) + field_len));
                    p += static_cast<size_t>(field_len);
                }
            }
            return fields;
        }
        pos += total;
    }
    return {};
}

int64_t read_be64_field(const std::vector<uint8_t>& field) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && i < field.size(); ++i) {
        v = (v << 8) | field[i];
    }
    return static_cast<int64_t>(v);
}

int32_t read_be32_field(const std::vector<uint8_t>& field) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4 && i < field.size(); ++i) {
        v = (v << 8) | field[i];
    }
    return static_cast<int32_t>(v);
}

// =============================================================================
// Fixture: extended-query wire flow with a stub executor returning one row
// =============================================================================

class QA_GDB718 : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(ensure_platform_init());

        int fds[2] = {-1, -1};
        ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
            << "socketpair() failed";
        client_fd_ = fds[0];
        set_recv_timeout_ms(client_fd_, 5000);

        conn_ = std::make_unique<Connection>(fds[1]);
        handler_ = std::make_unique<PgProtocolHandler>(718);
        handler_->set_query_executor(
            [this](const std::string& /*sql*/,
                   const std::string& /*database*/) -> Result<QueryResult> {
                ++executor_calls_;
                return ok(next_result_);
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

    /// Configure the stub executor to return a single-row, single-column result.
    void set_single_column_result(const std::string& name, TypeId type, Value value) {
        next_result_ = QueryResult{};
        next_result_.column_names = {name};
        next_result_.column_types = {type};
        next_result_.rows.push_back({std::move(value)});
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

    /// Run one Parse/Bind/Execute/Sync batch with the given RESULT format
    /// codes and return the backend response bytes.
    std::vector<uint8_t> run_with_result_formats(const std::vector<int16_t>& result_formats) {
        std::vector<uint8_t> batch;
        auto append = [&batch](const std::vector<uint8_t>& msg) {
            batch.insert(batch.end(), msg.begin(), msg.end());
        };
        append(build_parse("", "SELECT c FROM t"));
        append(build_bind_with_result_formats("", "", result_formats));
        append(build_execute(""));
        append(build_sync());
        pump_to_handler(batch);
        return drain_until_ready();
    }

    int client_fd_ = -1;
    std::unique_ptr<Connection> conn_;
    std::unique_ptr<PgProtocolHandler> handler_;
    QueryResult next_result_;
    int executor_calls_ = 0;
};

} // namespace

// =============================================================================
// AC 1: Regression — the exact H19 failure mode. A TIMESTAMP column bound
// with result format code 1 must arrive as the 8-byte PG binary timestamp
// (int64 microseconds since 2000-01-01), not as ISO text bytes labeled
// binary. Before the fix the field was "2000-01-01 00:00:00"-style text.
// =============================================================================

TEST_F(QA_GDB718, BinaryTimestamp_PgEpochIsEightZeroBytes) {
    // 2000-01-01 00:00:00 UTC = PG timestamp 0.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    auto resp = run_with_result_formats({1});
    EXPECT_FALSE(has_message(resp, 'E')) << "SQLSTATE " << extract_sqlstate(resp);
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u) << "no DataRow received";
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(*fields[0], std::vector<uint8_t>(8, 0))
        << "timestamp field is not the PG binary encoding (got " << fields[0]->size()
        << " bytes — text fallback regression, GDB-718)";
}

TEST_F(QA_GDB718, BinaryTimestamp_NonEpochValue) {
    // 2024-01-15T16:00:00 UTC: PG us = unix us - 946'684'800'000'000.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{1705334400000000LL}));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    ASSERT_EQ(fields[0]->size(), 8u);
    EXPECT_EQ(read_be64_field(*fields[0]), 1705334400000000LL - 946684800000000LL);
}

TEST_F(QA_GDB718, BinaryDate_Int32DaysSincePgEpoch) {
    // 2000-01-02 = Unix day 10958 = PG day 1.
    set_single_column_result("d", TypeId::DATE, Value(Date{10958}));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    ASSERT_EQ(fields[0]->size(), 4u) << "date field is not int32 (text fallback regression)";
    EXPECT_EQ(read_be32_field(*fields[0]), 1);
}

TEST_F(QA_GDB718, BinaryTime_Int64MicrosecondsSinceMidnight) {
    set_single_column_result("t", TypeId::TIME, Value(Time{43200000000LL})); // 12:00:00.
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    ASSERT_EQ(fields[0]->size(), 8u) << "time field is not int64 (text fallback regression)";
    EXPECT_EQ(read_be64_field(*fields[0]), 43200000000LL);
}

TEST_F(QA_GDB718, BinaryUuid_SixteenRawBytes) {
    Uuid uuid{};
    for (size_t i = 0; i < 16; ++i) {
        uuid[i] = static_cast<uint8_t>(0xF0 + i);
    }
    set_single_column_result("u", TypeId::UUID, Value(uuid));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    ASSERT_EQ(fields[0]->size(), 16u)
        << "uuid field is not 16 raw bytes (36-char hyphenated text regression)";
    EXPECT_EQ(std::vector<uint8_t>(uuid.begin(), uuid.end()), *fields[0]);
}

TEST_F(QA_GDB718, BinaryBytea_RawBytesNotHexText) {
    Blob blob = {0xDE, 0xAD, 0x00, 0xEF};
    set_single_column_result("b", TypeId::BLOB, Value(blob));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(*fields[0], blob) << "bytea field is not raw bytes (\\x hex text regression)";
}

TEST_F(QA_GDB718, BinaryUint64_NumericDigitGroups) {
    // 12'345'678 -> ndigits=2, weight=1, sign=0, dscale=0, digits 1234, 5678.
    set_single_column_result("n", TypeId::UINT64, Value(static_cast<uint64_t>(12345678)));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    const std::vector<uint8_t> expected = {
        0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0xD2, 0x16, 0x2E};
    EXPECT_EQ(*fields[0], expected)
        << "uint64 field is not PG numeric binary (decimal text regression)";
}

TEST_F(QA_GDB718, BinaryJson_TextPayloadPerJsonOid) {
    // json (OID 114) binary IS the text payload — this equality is the
    // deliberate, correct encoding, not a fallback.
    const std::string payload = R"({"k":"v"})";
    set_single_column_result("j", TypeId::JSON, Value(JsonString{payload}));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(std::string(fields[0]->begin(), fields[0]->end()), payload);
}

TEST_F(QA_GDB718, BinaryInterval_MicrosDaysMonths) {
    set_single_column_result("iv", TypeId::INTERVAL, Value(Interval{3, 7200000000LL}));
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    ASSERT_EQ(fields[0]->size(), 16u) << "interval field is not 16 bytes (text regression)";
    EXPECT_EQ(read_be64_field(*fields[0]), 7200000000LL);
    EXPECT_EQ(read_be32_field({fields[0]->begin() + 12, fields[0]->end()}), 3);
}

// =============================================================================
// DECIMAL: result format 1 must be REJECTED with a clean ErrorResponse
// (0A000 feature_not_supported) before any DataRow — never text-as-binary.
// =============================================================================

TEST_F(QA_GDB718, BinaryDecimal_RejectedWith0A000BeforeAnyDataRow) {
    set_single_column_result("d", TypeId::DECIMAL, Value(Decimal128{12, 34}));
    auto resp = run_with_result_formats({1});
    EXPECT_TRUE(has_message(resp, 'E'))
        << "DECIMAL with binary result format must be rejected (GDB-718)";
    EXPECT_EQ(extract_sqlstate(resp), "0A000");
    EXPECT_FALSE(has_message(resp, 'D')) << "DataRow sent despite unsupported binary format";
    EXPECT_FALSE(has_message(resp, 'C')) << "CommandComplete sent despite error";
    EXPECT_TRUE(has_message(resp, 'Z')) << "Sync must still complete the batch";
}

TEST_F(QA_GDB718, TextDecimal_StillWorks) {
    // Format code 0 (text) for DECIMAL is unaffected by the rejection.
    set_single_column_result("d", TypeId::DECIMAL, Value(Decimal128{12, 34}));
    auto resp = run_with_result_formats({0});
    EXPECT_FALSE(has_message(resp, 'E')) << "SQLSTATE " << extract_sqlstate(resp);
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(std::string(fields[0]->begin(), fields[0]->end()),
              value_to_pg_text(Value(Decimal128{12, 34})));
}

// =============================================================================
// Format-code semantics around the new encodings
// =============================================================================

TEST_F(QA_GDB718, TextFormatUnchangedForTimestamp) {
    // Format code 0: the ISO text representation, exactly as before.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    auto resp = run_with_result_formats({0});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(std::string(fields[0]->begin(), fields[0]->end()), "2000-01-01 00:00:00");
}

TEST_F(QA_GDB718, PerColumnFormatCodes_TextAndBinaryMixed) {
    next_result_ = QueryResult{};
    next_result_.column_names = {"ts", "u"};
    next_result_.column_types = {TypeId::TIMESTAMP, TypeId::UUID};
    next_result_.rows.push_back({Value(Timestamp{946684800000000LL}), Value(Uuid{})});

    auto resp = run_with_result_formats({0, 1}); // ts text, uuid binary.
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 2u);
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(std::string(fields[0]->begin(), fields[0]->end()), "2000-01-01 00:00:00");
    ASSERT_TRUE(fields[1].has_value());
    EXPECT_EQ(fields[1]->size(), 16u);
}

TEST_F(QA_GDB718, SingleBinaryFormatCodeAppliesToAllColumns) {
    next_result_ = QueryResult{};
    next_result_.column_names = {"d", "t"};
    next_result_.column_types = {TypeId::DATE, TypeId::TIME};
    next_result_.rows.push_back({Value(Date{10957}), Value(Time{1000000LL})});

    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 2u);
    ASSERT_TRUE(fields[0].has_value());
    EXPECT_EQ(fields[0]->size(), 4u);
    EXPECT_EQ(read_be32_field(*fields[0]), 0);
    ASSERT_TRUE(fields[1].has_value());
    EXPECT_EQ(fields[1]->size(), 8u);
    EXPECT_EQ(read_be64_field(*fields[1]), 1000000);
}

TEST_F(QA_GDB718, NullBinaryFieldStaysNull) {
    set_single_column_result("u", TypeId::UUID, Value::make_null());
    auto resp = run_with_result_formats({1});
    auto fields = extract_first_data_row(resp);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_FALSE(fields[0].has_value()) << "NULL must use the -1 length indicator";
}
