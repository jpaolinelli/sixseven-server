/// @file test_qa_gdb_718_adversarial.cpp
/// @brief Adversarial QA tests for GDB-718 (audit H19): value_to_pg_binary
/// must emit real PostgreSQL binary encodings for every advertised OID, and
/// the handle_execute guard must reject result format code 1 for DECIMAL
/// (SQLSTATE 0A000) before any DataRow.
///
/// These go beyond the shipped tests/qa/test_qa_gdb_718.cpp happy-path
/// coverage and try to break the implementation:
///   1. Boundary values per encoding (epoch edges, min/max, NULs, large
///      payloads, Inf/NaN, all-zero / all-FF).
///   2. Format-code matrix attacks (count != column count, invalid codes,
///      mixed text/binary, simple-Query stays text, repeated Execute,
///      Describe-then-Execute).
///   3. DECIMAL rejection robustness (mixed columns, multiple DECIMALs,
///      error recovery — clean extended-protocol query after a 0A000).
///   4. Decode (GDB-712) / encode (GDB-718) byte symmetry cross-check.
///   5. Independent byte derivations that do NOT trust the unit pins.
///
/// The wire harness mirrors test_qa_gdb_718.cpp / test_qa_gdb_712.cpp: a
/// PgProtocolHandler driven over a socketpair with a stub executor.
///
/// NOTE: helpers use ::send/::recv and sixseven::platform_init() so they run
/// on Windows (socketpair() emulated with loopback TCP).

#include "sixseven/common/platform.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <sys/time.h>
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
// Socket + wire helpers (self-contained; anonymous namespace, no ODR clash)
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
    push_be32(body, 196608);
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
                                 const std::vector<uint32_t>& param_oids = {}) {
    std::vector<uint8_t> body;
    push_cstring(body, stmt_name);
    push_cstring(body, sql);
    push_be16(body, static_cast<uint16_t>(param_oids.size()));
    for (uint32_t oid : param_oids) {
        push_be32(body, oid);
    }
    return frame_message('P', body);
}

/// Bind with explicit parameter format codes / values AND result format codes.
std::vector<uint8_t> build_bind(std::string_view portal_name,
                                std::string_view stmt_name,
                                const std::vector<int16_t>& param_formats,
                                const std::vector<std::optional<std::string>>& param_values,
                                const std::vector<int16_t>& result_formats) {
    std::vector<uint8_t> body;
    push_cstring(body, portal_name);
    push_cstring(body, stmt_name);
    push_be16(body, static_cast<uint16_t>(param_formats.size()));
    for (int16_t code : param_formats) {
        push_be16(body, static_cast<uint16_t>(code));
    }
    push_be16(body, static_cast<uint16_t>(param_values.size()));
    for (const auto& val : param_values) {
        if (!val.has_value()) {
            push_be32(body, 0xFFFFFFFFu);
        } else {
            push_be32(body, static_cast<uint32_t>(val->size()));
            body.insert(body.end(), val->begin(), val->end());
        }
    }
    push_be16(body, static_cast<uint16_t>(result_formats.size()));
    for (int16_t code : result_formats) {
        push_be16(body, static_cast<uint16_t>(code));
    }
    return frame_message('B', body);
}

/// No-parameter Bind with the given RESULT format codes (the common case).
std::vector<uint8_t> build_bind_result_formats(std::string_view portal_name,
                                               std::string_view stmt_name,
                                               const std::vector<int16_t>& result_formats) {
    return build_bind(portal_name, stmt_name, {}, {}, result_formats);
}

std::vector<uint8_t> build_execute(std::string_view portal_name) {
    std::vector<uint8_t> body;
    push_cstring(body, portal_name);
    push_be32(body, 0);
    return frame_message('E', body);
}

std::vector<uint8_t> build_describe_portal(std::string_view portal_name) {
    std::vector<uint8_t> body;
    body.push_back('P');
    push_cstring(body, portal_name);
    return frame_message('D', body);
}

std::vector<uint8_t> build_sync() {
    return {'S', 0, 0, 0, 4};
}

/// Big-endian raw bytes a real driver sends as a binary parameter value.
std::string be_param64(int64_t v) {
    auto u = static_cast<uint64_t>(v);
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) {
        s[static_cast<size_t>(i)] = static_cast<char>((u >> (8 * (7 - i))) & 0xFF);
    }
    return s;
}

/// Big-endian 8-byte vector for an int64 (so comparisons don't mix iterators
/// from two distinct temporary strings under MSVC's debug-iterator checks).
std::vector<uint8_t> be_bytes64(int64_t v) {
    auto s = be_param64(v);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// =============================================================================
// Backend message scanning
// =============================================================================

struct Framed {
    uint8_t type = 0;
    size_t begin = 0; // index of type byte
    size_t total = 0; // 1 + length
};

std::vector<Framed> frame_all(const std::vector<uint8_t>& data) {
    std::vector<Framed> out;
    size_t pos = 0;
    while (pos + 5 <= data.size()) {
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size()) {
            break;
        }
        out.push_back(Framed{data[pos], pos, total});
        pos += total;
    }
    return out;
}

bool has_message(const std::vector<uint8_t>& data, uint8_t type) {
    for (const auto& f : frame_all(data)) {
        if (f.type == type) {
            return true;
        }
    }
    return false;
}

size_t count_messages(const std::vector<uint8_t>& data, uint8_t type) {
    size_t n = 0;
    for (const auto& f : frame_all(data)) {
        if (f.type == type) {
            ++n;
        }
    }
    return n;
}

std::string extract_sqlstate(const std::vector<uint8_t>& data) {
    for (const auto& f : frame_all(data)) {
        if (f.type != 'E') {
            continue;
        }
        size_t field_pos = f.begin + 5;
        size_t end = f.begin + f.total;
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
    return {};
}

using RowFields = std::vector<std::optional<std::vector<uint8_t>>>;

/// Parse the Nth (0-based) DataRow ('D') message in `data`.
RowFields extract_data_row(const std::vector<uint8_t>& data, size_t which = 0) {
    size_t seen = 0;
    for (const auto& f : frame_all(data)) {
        if (f.type != 'D') {
            continue;
        }
        if (seen++ != which) {
            continue;
        }
        RowFields fields;
        size_t p = f.begin + 5;
        uint16_t ncols = static_cast<uint16_t>((static_cast<uint16_t>(data[p]) << 8) | data[p + 1]);
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
    return {};
}

int64_t read_be64(const std::vector<uint8_t>& f, size_t off = 0) {
    uint64_t v = 0;
    for (size_t i = off; i < off + 8 && i < f.size(); ++i) {
        v = (v << 8) | f[i];
    }
    return static_cast<int64_t>(v);
}

int32_t read_be32(const std::vector<uint8_t>& f, size_t off = 0) {
    uint32_t v = 0;
    for (size_t i = off; i < off + 4 && i < f.size(); ++i) {
        v = (v << 8) | f[i];
    }
    return static_cast<int32_t>(v);
}

int16_t read_be16(const std::vector<uint8_t>& f, size_t off = 0) {
    uint16_t v = 0;
    for (size_t i = off; i < off + 2 && i < f.size(); ++i) {
        v = static_cast<uint16_t>((v << 8) | f[i]);
    }
    return static_cast<int16_t>(v);
}

double read_be_float8(const std::vector<uint8_t>& f, size_t off) {
    uint64_t bits = static_cast<uint64_t>(read_be64(f, off));
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

// =============================================================================
// Fixture: extended-query wire flow with a stub executor that captures SQL.
// =============================================================================

class QA_GDB718_Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(ensure_platform_init());

        int fds[2] = {-1, -1};
        ASSERT_EQ(sixseven_platform::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
            << "socketpair() failed";
        client_fd_ = fds[0];
        set_recv_timeout_ms(client_fd_, 5000);

        conn_ = std::make_unique<Connection>(fds[1]);
        handler_ = std::make_unique<PgProtocolHandler>(7180);
        handler_->set_query_executor(
            [this](const std::string& sql, const std::string& /*database*/) -> Result<QueryResult> {
                ++executor_calls_;
                received_sql_ = sql;
                return ok(next_result_);
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

    void set_single_column_result(const std::string& name, TypeId type, Value value) {
        next_result_ = QueryResult{};
        next_result_.column_names = {name};
        next_result_.column_types = {type};
        next_result_.rows.push_back({std::move(value)});
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

    /// Parse/Bind(result formats)/Execute/Sync for "SELECT ..." (no params).
    std::vector<uint8_t> run_with_result_formats(const std::vector<int16_t>& result_formats) {
        std::vector<uint8_t> batch;
        auto append = [&batch](const std::vector<uint8_t>& m) {
            batch.insert(batch.end(), m.begin(), m.end());
        };
        append(build_parse("", "SELECT c FROM t"));
        append(build_bind_result_formats("", "", result_formats));
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
    std::string received_sql_;
};

// =============================================================================
// 1. Boundary values per encoding
// =============================================================================

// --- DATE boundaries ---------------------------------------------------------

TEST_F(QA_GDB718_Adversarial, DatePgEpochExactlyZero) {
    // 2000-01-01 = Unix day 10957 = PG day 0.
    set_single_column_result("d", TypeId::DATE, Value(Date{10957}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 1u);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 4u);
    EXPECT_EQ(read_be32(*f[0]), 0);
}

TEST_F(QA_GDB718_Adversarial, DateOneDayBeforePgEpochIsNegativeOne) {
    set_single_column_result("d", TypeId::DATE, Value(Date{10956}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 4u);
    EXPECT_EQ(read_be32(*f[0]), -1);
}

TEST_F(QA_GDB718_Adversarial, DateUnixEpoch1970IsNegativePgOffset) {
    // Unix day 0 (1970-01-01) -> PG day -10957.
    set_single_column_result("d", TypeId::DATE, Value(Date{0}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(read_be32(*f[0]), -10957);
}

TEST_F(QA_GDB718_Adversarial, DateLargeFutureRepresentable) {
    // Year ~9999 — safely within int32 after the epoch shift, no overflow.
    set_single_column_result("d", TypeId::DATE, Value(Date{2932896}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 4u);
    EXPECT_EQ(read_be32(*f[0]), 2932896 - 10957);
}

// --- TIMESTAMP boundaries ----------------------------------------------------

TEST_F(QA_GDB718_Adversarial, TimestampPre2000IsNegative) {
    // 1999-12-31 23:59:59 UTC = PG epoch - 1s.
    int64_t unix_us = 946684800000000LL - 1000000LL;
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{unix_us}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 8u);
    EXPECT_EQ(read_be64(*f[0]), -1000000LL);
}

TEST_F(QA_GDB718_Adversarial, TimestampFarFuture) {
    // 2200-01-01 00:00:00 UTC. Independent: unix us minus PG offset.
    const int64_t unix_us = 7258118400000000LL; // 2200-01-01T00:00:00Z in us.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{unix_us}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(read_be64(*f[0]), unix_us - 946684800000000LL);
}

TEST_F(QA_GDB718_Adversarial, TimestampUnixEpochIsNegativePgOffsetUs) {
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{0}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(read_be64(*f[0]), -946684800000000LL);
}

// --- TIME boundaries ---------------------------------------------------------

TEST_F(QA_GDB718_Adversarial, TimeMidnightIsZero) {
    set_single_column_result("t", TypeId::TIME, Value(Time{0}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 8u);
    EXPECT_EQ(read_be64(*f[0]), 0);
}

TEST_F(QA_GDB718_Adversarial, TimeEndOfDayMaxMicros) {
    // 23:59:59.999999 = 86399999999 us.
    const int64_t us = 86399999999LL;
    set_single_column_result("t", TypeId::TIME, Value(Time{us}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(read_be64(*f[0]), us);
}

// --- UINT64 / numeric boundaries (independent digit-group derivation) --------

/// Independently encode an unsigned integer in PG numeric binary, computed
/// here from scratch (NOT borrowed from the production code or unit pins).
std::vector<uint8_t> expected_numeric(uint64_t v) {
    std::vector<int16_t> groups; // least significant first
    uint64_t t = v;
    while (t > 0) {
        groups.push_back(static_cast<int16_t>(t % 10000));
        t /= 10000;
    }
    size_t skip = 0;
    while (skip < groups.size() && groups[skip] == 0) {
        ++skip;
    }
    int16_t ndigits = static_cast<int16_t>(groups.size() - skip);
    int16_t weight = groups.empty() ? 0 : static_cast<int16_t>(groups.size() - 1);
    std::vector<uint8_t> buf;
    push_be16(buf, static_cast<uint16_t>(ndigits));
    push_be16(buf, static_cast<uint16_t>(weight));
    push_be16(buf, 0);
    push_be16(buf, 0);
    for (size_t i = groups.size(); i > skip; --i) {
        push_be16(buf, static_cast<uint16_t>(groups[i - 1]));
    }
    return buf;
}

TEST_F(QA_GDB718_Adversarial, NumericZeroIsHeaderOnly) {
    set_single_column_result("n", TypeId::UINT64, Value(static_cast<uint64_t>(0)));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    // ndigits=0, weight=0, sign=0, dscale=0, no digit groups -> 8 bytes of 0.
    EXPECT_EQ(*f[0], (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(*f[0], expected_numeric(0));
}

TEST_F(QA_GDB718_Adversarial, NumericNineNineNineNineSingleGroup) {
    set_single_column_result("n", TypeId::UINT64, Value(static_cast<uint64_t>(9999)));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], expected_numeric(9999));
    // ndigits=1, weight=0, digit group = 9999 (0x270F).
    EXPECT_EQ(*f[0], (std::vector<uint8_t>{0, 1, 0, 0, 0, 0, 0, 0, 0x27, 0x0F}));
}

TEST_F(QA_GDB718_Adversarial, NumericTenThousandRollsToTwoGroupsWeightOne) {
    // 10000 -> groups {0, 1} LS-first; trailing-zero group trimmed -> ndigits=1,
    // weight=1, single group = 1.
    set_single_column_result("n", TypeId::UINT64, Value(static_cast<uint64_t>(10000)));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], expected_numeric(10000));
    EXPECT_EQ(*f[0], (std::vector<uint8_t>{0, 1, 0, 1, 0, 0, 0, 0, 0, 1}));
}

TEST_F(QA_GDB718_Adversarial, NumericTenThousandOne) {
    // 10001 -> groups {1,1}; no trim -> ndigits=2, weight=1, digits 1,1.
    set_single_column_result("n", TypeId::UINT64, Value(static_cast<uint64_t>(10001)));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], expected_numeric(10001));
    EXPECT_EQ(*f[0], (std::vector<uint8_t>{0, 2, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1}));
}

TEST_F(QA_GDB718_Adversarial, NumericUint64MaxIndependentDerivation) {
    const uint64_t v = std::numeric_limits<uint64_t>::max();
    set_single_column_result("n", TypeId::UINT64, Value(v));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    // 18446744073709551615 is a 20-decimal-digit value = 5 base-10000 groups
    // (1844 6744 0737 0955 1615), so ndigits=5, weight=4 (NOT 11/10 — a 64-bit
    // unsigned never exceeds 20 decimal digits). expected_numeric() derives the
    // same groups independently of the production code.
    auto exp = expected_numeric(v);
    EXPECT_EQ(read_be16(*f[0], 0), 5) << "ndigits";
    EXPECT_EQ(read_be16(*f[0], 2), 4) << "weight";
    EXPECT_EQ(read_be16(*f[0], 4), 0) << "sign";
    EXPECT_EQ(read_be16(*f[0], 8), 1844);
    EXPECT_EQ(read_be16(*f[0], 16), 1615);
    EXPECT_EQ(*f[0], exp);
}

// --- INTERVAL boundaries -----------------------------------------------------

TEST_F(QA_GDB718_Adversarial, IntervalNegativeMonthsAndMicros) {
    set_single_column_result("iv", TypeId::INTERVAL, Value(Interval{-5, -123456789LL}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 16u);
    EXPECT_EQ(read_be64(*f[0], 0), -123456789LL); // microseconds
    EXPECT_EQ(read_be32(*f[0], 8), 0);            // days always 0
    EXPECT_EQ(read_be32(*f[0], 12), -5);          // months
}

TEST_F(QA_GDB718_Adversarial, IntervalZeroAllZeroBytes) {
    set_single_column_result("iv", TypeId::INTERVAL, Value(Interval{0, 0}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], std::vector<uint8_t>(16, 0));
}

// --- UUID boundaries ---------------------------------------------------------

TEST_F(QA_GDB718_Adversarial, UuidAllZeros) {
    set_single_column_result("u", TypeId::UUID, Value(Uuid{}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], std::vector<uint8_t>(16, 0));
}

TEST_F(QA_GDB718_Adversarial, UuidAllFF) {
    Uuid u{};
    u.fill(0xFF);
    set_single_column_result("u", TypeId::UUID, Value(u));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], std::vector<uint8_t>(16, 0xFF));
}

// --- BLOB boundaries ---------------------------------------------------------

TEST_F(QA_GDB718_Adversarial, BlobEmptyIsZeroLengthField) {
    set_single_column_result("b", TypeId::BLOB, Value(Blob{}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 1u);
    ASSERT_TRUE(f[0].has_value()) << "empty bytea is a zero-length field, not NULL";
    EXPECT_TRUE(f[0]->empty());
}

TEST_F(QA_GDB718_Adversarial, BlobWithInternalNulBytesPreserved) {
    Blob b = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x00, 0x7F};
    set_single_column_result("b", TypeId::BLOB, Value(b));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], b) << "internal NUL bytes must survive (raw bytea)";
}

TEST_F(QA_GDB718_Adversarial, BlobLarge64KBRoundTripsExactly) {
    Blob b;
    b.reserve(70000);
    for (size_t i = 0; i < 70000; ++i) {
        b.push_back(static_cast<uint8_t>(i * 31 + 7));
    }
    set_single_column_result("b", TypeId::BLOB, Value(b));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), b.size());
    EXPECT_EQ(*f[0], b);
}

// --- JSON boundaries ---------------------------------------------------------

TEST_F(QA_GDB718_Adversarial, JsonUnicodePayloadPreserved) {
    // UTF-8 bytes for {"k":"é☃"} written explicitly (é = C3 A9, ☃ = E2 98 83)
    // so the literal stays a narrow std::string regardless of source encoding.
    const std::string payload = "{\"k\":\"\xC3\xA9\xE2\x98\x83\"}";
    set_single_column_result("j", TypeId::JSON, Value(JsonString{payload}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(std::string(f[0]->begin(), f[0]->end()), payload);
    // json (OID 114) has NO jsonb version-prefix byte: first byte is '{'.
    ASSERT_FALSE(f[0]->empty());
    EXPECT_EQ((*f[0])[0], static_cast<uint8_t>('{'));
}

// --- POINT boundaries (Inf / NaN bit-preservation) ---------------------------

TEST_F(QA_GDB718_Adversarial, PointInfinityAndNaNCoordinatesBitPreserved) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    set_single_column_result("p", TypeId::POINT, Value(Point{inf, nan}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 16u);
    EXPECT_TRUE(std::isinf(read_be_float8(*f[0], 0)));
    EXPECT_GT(read_be_float8(*f[0], 0), 0.0);
    EXPECT_TRUE(std::isnan(read_be_float8(*f[0], 8)));
}

TEST_F(QA_GDB718_Adversarial, PointNegativeZeroDistinctBits) {
    set_single_column_result("p", TypeId::POINT, Value(Point{-0.0, 0.0}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 16u);
    // -0.0 has the sign bit set: first byte 0x80, rest zero.
    EXPECT_EQ((*f[0])[0], 0x80);
    EXPECT_EQ(read_be64(*f[0], 8), 0); // +0.0
}

// --- EMBEDDING boundaries ----------------------------------------------------

TEST_F(QA_GDB718_Adversarial, EmbeddingDimOne) {
    set_single_column_result("e", TypeId::EMBEDDING, Value(Embedding{1.5F}));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    // int16 dim=1, int16 reserved=0, then one float4 (1.5 = 0x3FC00000).
    ASSERT_EQ(f[0]->size(), 4u + 4u);
    EXPECT_EQ(read_be16(*f[0], 0), 1);
    EXPECT_EQ(read_be16(*f[0], 2), 0);
    EXPECT_EQ(static_cast<uint32_t>(read_be32(*f[0], 4)), 0x3FC00000u);
}

TEST_F(QA_GDB718_Adversarial, EmbeddingLargeDimWithinInt16) {
    const size_t dim = 1536; // typical OpenAI embedding size; fits int16.
    Embedding e(dim, 0.0F);
    for (size_t i = 0; i < dim; ++i) {
        e[i] = static_cast<float>(i);
    }
    set_single_column_result("e", TypeId::EMBEDDING, Value(e));
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_EQ(f[0]->size(), 4u + dim * 4u);
    EXPECT_EQ(read_be16(*f[0], 0), static_cast<int16_t>(dim));
    EXPECT_EQ(read_be16(*f[0], 2), 0);
    // Spot-check last element bit pattern.
    float last = static_cast<float>(dim - 1);
    uint32_t bits = 0;
    std::memcpy(&bits, &last, sizeof(bits));
    EXPECT_EQ(static_cast<uint32_t>(read_be32(*f[0], 4 + (dim - 1) * 4)), bits);
}

// =============================================================================
// 2. Format-code matrix attacks
// =============================================================================

TEST_F(QA_GDB718_Adversarial, ResultFormatCountExceedsColumnsExtraIgnored) {
    // 1 column, 3 result format codes {1,1,1}. PG strictly errors on a count
    // that is not 0/1/ncols; SixSevenDB tolerates it — col 0 uses code[0]=1
    // (binary), extras ignored. Documents current (safe) behavior; no crash.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    auto r = run_with_result_formats({1, 1, 1});
    EXPECT_FALSE(has_message(r, 'E')) << "SQLSTATE " << extract_sqlstate(r);
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 1u);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(f[0]->size(), 8u) << "binary timestamp expected from code[0]=1";
}

TEST_F(QA_GDB718_Adversarial, ResultFormatCountFewerThanColumnsMissingDefaultsText) {
    // 2 columns, 1 result format code {1} -> single code applies to ALL columns.
    next_result_ = QueryResult{};
    next_result_.column_names = {"d", "t"};
    next_result_.column_types = {TypeId::DATE, TypeId::TIME};
    next_result_.rows.push_back({Value(Date{10957}), Value(Time{0})});
    auto r = run_with_result_formats({1});
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 2u);
    ASSERT_TRUE(f[0].has_value());
    ASSERT_TRUE(f[1].has_value());
    EXPECT_EQ(f[0]->size(), 4u);
    EXPECT_EQ(f[1]->size(), 8u);
}

TEST_F(QA_GDB718_Adversarial, ResultFormatTwoCodesThreeColumnsThirdDefaultsText) {
    // 3 columns, 2 explicit codes {0,1}. resolve_format_code returns 0 (text)
    // for col index >= size, so the 3rd column is text. No OOB, no crash.
    next_result_ = QueryResult{};
    next_result_.column_names = {"a", "b", "c"};
    next_result_.column_types = {TypeId::INT32, TypeId::DATE, TypeId::DATE};
    next_result_.rows.push_back(
        {Value(static_cast<int32_t>(7)), Value(Date{10958}), Value(Date{10958})});
    auto r = run_with_result_formats({0, 1});
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 3u);
    // col0 text "7"; col1 binary date (4 bytes); col2 falls back to text date.
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(std::string(f[0]->begin(), f[0]->end()), "7");
    ASSERT_TRUE(f[1].has_value());
    EXPECT_EQ(f[1]->size(), 4u);
    ASSERT_TRUE(f[2].has_value());
    EXPECT_NE(f[2]->size(), 4u) << "3rd column should default to text, not binary date";
}

TEST_F(QA_GDB718_Adversarial, InvalidResultFormatCodeTwoTreatedAsText) {
    // Result format code 2 is not a valid PG code. send_data_row only treats
    // ==1 as binary, so code 2 yields TEXT (the safe direction — never
    // mislabels). Documents the lack of a strict result-format validator.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    auto r = run_with_result_formats({2});
    EXPECT_FALSE(has_message(r, 'E'));
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(std::string(f[0]->begin(), f[0]->end()), "2000-01-01 00:00:00")
        << "invalid result format code must not be misread as binary";
}

TEST_F(QA_GDB718_Adversarial, NegativeResultFormatCodeTreatedAsText) {
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    auto r = run_with_result_formats({-1});
    EXPECT_FALSE(has_message(r, 'E'));
    auto f = extract_data_row(r);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(std::string(f[0]->begin(), f[0]->end()), "2000-01-01 00:00:00");
}

TEST_F(QA_GDB718_Adversarial, ManyColumnsMixedTextBinaryAlternating) {
    next_result_ = QueryResult{};
    next_result_.column_names = {"a", "b", "c", "d"};
    next_result_.column_types = {TypeId::DATE, TypeId::TIME, TypeId::UUID, TypeId::INT32};
    next_result_.rows.push_back({Value(Date{10958}),
                                 Value(Time{1000000LL}),
                                 Value(Uuid{}),
                                 Value(static_cast<int32_t>(42))});
    auto r = run_with_result_formats({1, 0, 1, 0}); // binary, text, binary, text
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 4u);
    EXPECT_EQ(f[0]->size(), 4u);                                    // binary date
    EXPECT_EQ(std::string(f[1]->begin(), f[1]->end()), "00:00:01"); // text time
    EXPECT_EQ(f[2]->size(), 16u);                                   // binary uuid
    EXPECT_EQ(std::string(f[3]->begin(), f[3]->end()), "42");       // text int
}

TEST_F(QA_GDB718_Adversarial, SimpleQueryProtocolAlwaysTextNeverBinary) {
    // The simple Query ('Q') path passes empty format codes -> all text, even
    // for a type with a binary encoding. There is no way to request binary in
    // the simple protocol, so a binary mislabel is impossible there.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    std::vector<uint8_t> q;
    push_cstring(q, "SELECT c FROM t");
    auto msg = frame_message('Q', q);
    pump_to_handler(msg);
    auto r = drain_until_ready();
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 1u);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(std::string(f[0]->begin(), f[0]->end()), "2000-01-01 00:00:00");
}

TEST_F(QA_GDB718_Adversarial, RepeatedExecuteOnExhaustedPortalSendsNoMoreRows) {
    // GDB-1224: this test previously asserted that a second Execute on the
    // SAME portal re-runs the query and emits a second DataRow. That is not
    // how the PostgreSQL extended query protocol works: build_execute()
    // sends max_rows=0 (unlimited), so the FIRST Execute already drains the
    // portal's entire cached result (portal.rows_sent == total rows) --
    // see PgProtocolHandler::handle_execute() in src/server/pg_protocol.cpp,
    // which honors max_rows paging against a cached result rather than
    // re-invoking the query executor on every Execute. A second Execute on
    // an already-exhausted, unlimited-fetch portal must therefore send ZERO
    // additional DataRows and just emit CommandComplete again -- the
    // executor must NOT be invoked a second time. (The previous version of
    // this test's wrong expectation, combined with unconditionally indexing
    // a second DataRow that was never sent, crashed with "vector subscript
    // out of range" once the QA binary ran far enough to reach it.)
    set_single_column_result("d", TypeId::DATE, Value(Date{10958}));
    std::vector<uint8_t> batch;
    auto append = [&batch](const std::vector<uint8_t>& m) {
        batch.insert(batch.end(), m.begin(), m.end());
    };
    append(build_parse("", "SELECT c FROM t"));
    append(build_bind_result_formats("", "", {1}));
    append(build_execute(""));
    append(build_execute("")); // second Execute on the same, now-exhausted portal
    append(build_sync());
    pump_to_handler(batch);
    auto r = drain_until_ready();
    EXPECT_FALSE(has_message(r, 'E'));
    ASSERT_EQ(count_messages(r, 'D'), 1u)
        << "portal already sent all rows on the first Execute; the second Execute "
           "(still max_rows=0) has nothing left to send";
    EXPECT_EQ(executor_calls_, 1)
        << "query executor must run once per portal, not once per Execute";
    ASSERT_EQ(count_messages(r, 'C'), 2u)
        << "each Execute -- including the no-op second one -- still gets its own CommandComplete";

    auto row0 = extract_data_row(r, 0);
    ASSERT_TRUE(row0[0].has_value());
    EXPECT_EQ(read_be32(*row0[0]), 1) << "the single row must still be the binary-encoded date";
}

TEST_F(QA_GDB718_Adversarial, ReBindAndExecuteSamePortalNameProducesTwoBinaryRows) {
    // Complement to RepeatedExecuteOnExhaustedPortalSendsNoMoreRows: to
    // legitimately get a second DataRow for the same portal name, the client
    // must Bind again (which resets portal.executed / rows_sent), not just
    // send a second bare Execute.
    set_single_column_result("d", TypeId::DATE, Value(Date{10958}));
    std::vector<uint8_t> batch;
    auto append = [&batch](const std::vector<uint8_t>& m) {
        batch.insert(batch.end(), m.begin(), m.end());
    };
    append(build_parse("", "SELECT c FROM t"));
    append(build_bind_result_formats("", "", {1}));
    append(build_execute(""));
    append(build_bind_result_formats("", "", {1})); // re-Bind resets the portal
    append(build_execute(""));
    append(build_sync());
    pump_to_handler(batch);
    auto r = drain_until_ready();
    EXPECT_FALSE(has_message(r, 'E'));
    EXPECT_EQ(count_messages(r, 'D'), 2u) << "re-Bind + Execute must produce a fresh row each time";
    EXPECT_EQ(executor_calls_, 2) << "re-Bind must cause the query executor to run again";
    auto row0 = extract_data_row(r, 0);
    auto row1 = extract_data_row(r, 1);
    ASSERT_TRUE(row0[0].has_value());
    ASSERT_TRUE(row1[0].has_value());
    EXPECT_EQ(*row0[0], *row1[0]) << "both Executes must encode the same binary date";
    EXPECT_EQ(read_be32(*row0[0]), 1);
}

TEST_F(QA_GDB718_Adversarial, DescribePortalThenExecuteConsistentBinaryFormat) {
    // Describe the portal (RowDescription should advertise the bound result
    // format), then Execute and confirm the DataRow honors the same binary
    // format. Uses a describer returning a UUID column.
    handler_->set_query_describer(
        [](const std::string& /*sql*/,
           const std::string& /*database*/) -> Result<std::vector<ColumnDescription>> {
            std::vector<ColumnDescription> cols;
            ColumnDescription c;
            c.name = "u";
            c.type_id = TypeId::UUID;
            cols.push_back(c);
            return ok(std::move(cols));
        });
    Uuid u{};
    u.fill(0xAB);
    set_single_column_result("u", TypeId::UUID, Value(u));

    std::vector<uint8_t> batch;
    auto append = [&batch](const std::vector<uint8_t>& m) {
        batch.insert(batch.end(), m.begin(), m.end());
    };
    append(build_parse("", "SELECT u FROM t"));
    append(build_bind_result_formats("", "", {1}));
    append(build_describe_portal(""));
    append(build_execute(""));
    append(build_sync());
    pump_to_handler(batch);
    auto r = drain_until_ready();
    EXPECT_FALSE(has_message(r, 'E')) << "SQLSTATE " << extract_sqlstate(r);
    ASSERT_TRUE(has_message(r, 'T')) << "RowDescription expected from Describe";
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 1u);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(f[0]->size(), 16u);
    EXPECT_EQ(*f[0], std::vector<uint8_t>(16, 0xAB));
}

// =============================================================================
// 3. DECIMAL rejection robustness
// =============================================================================

TEST_F(QA_GDB718_Adversarial, DecimalBinaryOtherColumnsBinaryStillRejected) {
    // DECIMAL col bound binary alongside an INT32 col bound binary: the whole
    // result must be rejected (no partial DataRow) before any row is sent.
    next_result_ = QueryResult{};
    next_result_.column_names = {"i", "d"};
    next_result_.column_types = {TypeId::INT32, TypeId::DECIMAL};
    next_result_.rows.push_back({Value(static_cast<int32_t>(1)), Value(Decimal128{0, 5})});
    auto r = run_with_result_formats({1, 1});
    EXPECT_TRUE(has_message(r, 'E'));
    EXPECT_EQ(extract_sqlstate(r), "0A000");
    EXPECT_FALSE(has_message(r, 'D')) << "no DataRow may precede the rejection";
    EXPECT_TRUE(has_message(r, 'Z'));
}

TEST_F(QA_GDB718_Adversarial, DecimalTextWhileOthersBinarySucceeds) {
    // DECIMAL col is TEXT (code 0); the binary INT32 col is fine. The guard
    // only fires for DECIMAL columns whose resolved format is 1, so this must
    // succeed and produce a DataRow.
    next_result_ = QueryResult{};
    next_result_.column_names = {"i", "d"};
    next_result_.column_types = {TypeId::INT32, TypeId::DECIMAL};
    next_result_.rows.push_back({Value(static_cast<int32_t>(9)), Value(Decimal128{0, 5})});
    auto r = run_with_result_formats({1, 0}); // int binary, decimal text
    EXPECT_FALSE(has_message(r, 'E')) << "SQLSTATE " << extract_sqlstate(r);
    auto f = extract_data_row(r);
    ASSERT_EQ(f.size(), 2u);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(f[0]->size(), 4u); // binary int32
    EXPECT_EQ(read_be32(*f[0]), 9);
    ASSERT_TRUE(f[1].has_value()); // text decimal
    EXPECT_EQ(std::string(f[1]->begin(), f[1]->end()), value_to_pg_text(Value(Decimal128{0, 5})));
}

TEST_F(QA_GDB718_Adversarial, MultipleDecimalColumnsAllBinaryRejectedOnce) {
    next_result_ = QueryResult{};
    next_result_.column_names = {"d1", "d2"};
    next_result_.column_types = {TypeId::DECIMAL, TypeId::DECIMAL};
    next_result_.rows.push_back({Value(Decimal128{0, 1}), Value(Decimal128{0, 2})});
    auto r = run_with_result_formats({1});
    EXPECT_TRUE(has_message(r, 'E'));
    EXPECT_EQ(extract_sqlstate(r), "0A000");
    EXPECT_EQ(count_messages(r, 'E'), 1u) << "exactly one ErrorResponse";
    EXPECT_FALSE(has_message(r, 'D'));
}

TEST_F(QA_GDB718_Adversarial, SingleAllBinaryCodeStillRejectsDecimalColumn) {
    // A single result format code {1} applies to all columns; a DECIMAL column
    // in the mix must still be caught by the per-column guard.
    next_result_ = QueryResult{};
    next_result_.column_names = {"i", "d", "t"};
    next_result_.column_types = {TypeId::INT32, TypeId::DECIMAL, TypeId::TIME};
    next_result_.rows.push_back(
        {Value(static_cast<int32_t>(1)), Value(Decimal128{0, 1}), Value(Time{0})});
    auto r = run_with_result_formats({1});
    EXPECT_TRUE(has_message(r, 'E'));
    EXPECT_EQ(extract_sqlstate(r), "0A000");
    EXPECT_FALSE(has_message(r, 'D'));
}

TEST_F(QA_GDB718_Adversarial, ErrorRecovery_CleanQueryAfterDecimalReject) {
    // After a 0A000 rejection + Sync, the session must accept a fresh
    // extended-protocol query and answer it correctly (error_in_extended_
    // reset by Sync).
    next_result_ = QueryResult{};
    next_result_.column_names = {"d"};
    next_result_.column_types = {TypeId::DECIMAL};
    next_result_.rows.push_back({Value(Decimal128{0, 7})});
    auto r1 = run_with_result_formats({1});
    ASSERT_TRUE(has_message(r1, 'E'));
    ASSERT_EQ(extract_sqlstate(r1), "0A000");
    ASSERT_TRUE(has_message(r1, 'Z'));

    // Second, independent batch: a binary TIMESTAMP that must encode cleanly.
    set_single_column_result("ts", TypeId::TIMESTAMP, Value(Timestamp{946684800000000LL}));
    auto r2 = run_with_result_formats({1});
    EXPECT_FALSE(has_message(r2, 'E'))
        << "session not recovered: SQLSTATE " << extract_sqlstate(r2);
    auto f = extract_data_row(r2);
    ASSERT_EQ(f.size(), 1u);
    ASSERT_TRUE(f[0].has_value());
    EXPECT_EQ(*f[0], std::vector<uint8_t>(8, 0)) << "post-recovery binary timestamp wrong";
}

TEST_F(QA_GDB718_Adversarial, DecimalNullBinaryColumnStillRejected) {
    // Even a NULL DECIMAL value bound binary is rejected: the guard checks the
    // column TYPE, not the value, so it fires before NULL handling.
    set_single_column_result("d", TypeId::DECIMAL, Value::make_null());
    auto r = run_with_result_formats({1});
    EXPECT_TRUE(has_message(r, 'E'));
    EXPECT_EQ(extract_sqlstate(r), "0A000");
    EXPECT_FALSE(has_message(r, 'D'));
}

// =============================================================================
// 4. GDB-712 decode / GDB-718 encode byte symmetry
// =============================================================================

TEST_F(QA_GDB718_Adversarial, Symmetry_Int8BinaryParamDecodeMatchesResultEncode) {
    // GDB-712 decodes a binary int8 parameter; GDB-718 encodes an INT64 result
    // in binary. The wire byte format must be identical (8 BE bytes) so a value
    // bound binary and re-read binary round-trips byte-for-byte.
    const int64_t v = 0x0102030405060708LL;

    // (a) GDB-712: bind a binary int8 param into SQL; capture decoded SQL text.
    std::vector<uint8_t> batch;
    auto append = [&batch](const std::vector<uint8_t>& m) {
        batch.insert(batch.end(), m.begin(), m.end());
    };
    next_result_ = QueryResult{};
    next_result_.affected_rows = 1;
    next_result_.message = "INSERT";
    append(build_parse("", "INSERT INTO t VALUES ($1)", {20})); // OID 20 = int8
    append(build_bind("", "", {1}, {be_param64(v)}, {}));
    append(build_execute(""));
    append(build_sync());
    pump_to_handler(batch);
    auto r1 = drain_until_ready();
    EXPECT_FALSE(has_message(r1, 'E')) << "SQLSTATE " << extract_sqlstate(r1);
    EXPECT_NE(received_sql_.find(std::to_string(v)), std::string::npos)
        << "GDB-712 must decode binary int8 to its decimal text; got: " << received_sql_;

    // (b) GDB-718: encode the same value as a binary INT64 result.
    auto encoded = value_to_pg_binary(Value(v));
    EXPECT_EQ(encoded, be_bytes64(v)) << "encode is not the inverse of the int8 binary wire format";
}

TEST_F(QA_GDB718_Adversarial, Symmetry_UuidWireBytesEqualEncoderOutput) {
    // A binary UUID parameter on the wire is 16 raw bytes; GDB-718 emits the
    // identical 16 raw bytes. (GDB-712 cannot DECODE binary uuid params — OID
    // 2950 is unsupported — so the symmetry is verified at the encoder.)
    Uuid u{};
    for (size_t i = 0; i < 16; ++i) {
        u[i] = static_cast<uint8_t>(i * 17 + 3);
    }
    auto encoded = value_to_pg_binary(Value(u));
    EXPECT_EQ(encoded, std::vector<uint8_t>(u.begin(), u.end()));
}

TEST_F(QA_GDB718_Adversarial, Symmetry_TimestampEncodeInvertsDocumentedWireFormat) {
    // A binary-format timestamp on the wire is int64 us since 2000-01-01.
    // Build those bytes independently and confirm value_to_pg_binary produces
    // exactly them, i.e. encode inverts the documented decode format.
    const int64_t unix_us = 1705334400000000LL; // 2024-01-15T16:00:00Z
    const int64_t pg_us = unix_us - 946684800000000LL;
    auto encoded = value_to_pg_binary(Value(Timestamp{unix_us}));
    EXPECT_EQ(encoded, be_bytes64(pg_us));
}

// =============================================================================
// 5. NULL invariants across binary/text
// =============================================================================

TEST_F(QA_GDB718_Adversarial, NullUsesMinusOneLengthForEveryBinaryType) {
    const TypeId types[] = {TypeId::DATE,
                            TypeId::TIME,
                            TypeId::TIMESTAMP,
                            TypeId::UUID,
                            TypeId::BLOB,
                            TypeId::UINT64,
                            TypeId::INTERVAL,
                            TypeId::POINT,
                            TypeId::JSON,
                            TypeId::EMBEDDING};
    for (TypeId ty : types) {
        set_single_column_result("c", ty, Value::make_null());
        auto r = run_with_result_formats({1});
        EXPECT_FALSE(has_message(r, 'E'))
            << "NULL of type " << static_cast<int>(ty) << " unexpectedly errored";
        auto f = extract_data_row(r);
        ASSERT_EQ(f.size(), 1u) << "no DataRow for type " << static_cast<int>(ty);
        EXPECT_FALSE(f[0].has_value())
            << "NULL must be -1 length, not an empty binary field, type " << static_cast<int>(ty);
    }
}

} // namespace
