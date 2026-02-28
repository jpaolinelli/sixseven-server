/// @file test_qa_gdb_148.cpp
/// @brief QA adversarial tests for GDB-148: Authentication (Trust, MD5, SCRAM-SHA-256).
///
/// Targets edge cases: empty inputs, boundary values, XOR size mismatch,
/// base64 padding, corrupted hashes, concurrent user ops, wire-protocol
/// auth flows for MD5 and SCRAM, invalid SASL mechanisms, wrong password.

#include "giodb/server/auth.h"
#include "giodb/server/connection.h"
#include "giodb/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace giodb;

// =============================================================================
// Crypto edge cases
// =============================================================================

TEST(QA148Crypto, Md5EmptyString) {
    auto h = md5_hex("");
    EXPECT_EQ(h.size(), 32u);
    // MD5("") = d41d8cd98f00b204e9800998ecf8427e
    EXPECT_EQ(h, "d41d8cd98f00b204e9800998ecf8427e");
}

TEST(QA148Crypto, Sha256EmptyData) {
    auto h = sha256({});
    EXPECT_EQ(h.size(), 32u);
}

TEST(QA148Crypto, HmacSha256EmptyKeyAndMessage) {
    auto m = hmac_sha256({}, "");
    EXPECT_EQ(m.size(), 32u);
}

TEST(QA148Crypto, XorBytesEmptyVectors) {
    auto r = xor_bytes({}, {});
    EXPECT_TRUE(r.empty());
}

TEST(QA148Crypto, RandomBytesZeroLength) {
    auto r = random_bytes(0);
    EXPECT_TRUE(r.empty());
}

TEST(QA148Crypto, RandomBytesLargeAllocation) {
    auto r = random_bytes(1024);
    EXPECT_EQ(r.size(), 1024u);
}

TEST(QA148Crypto, Base64RoundTripEmpty) {
    auto encoded = base64_encode({});
    EXPECT_TRUE(encoded.empty());
    auto decoded = base64_decode(encoded);
    EXPECT_TRUE(decoded.empty());
}

TEST(QA148Crypto, Base64RoundTrip1Byte) {
    std::vector<uint8_t> data = {0x42};
    auto encoded = base64_encode(data);
    // 1 byte → 4 chars with 2 padding '='
    EXPECT_EQ(encoded.size(), 4u);
    EXPECT_EQ(encoded.back(), '=');
    auto decoded = base64_decode(encoded);
    EXPECT_EQ(decoded, data);
}

TEST(QA148Crypto, Base64RoundTrip2Bytes) {
    std::vector<uint8_t> data = {0x42, 0x43};
    auto encoded = base64_encode(data);
    EXPECT_EQ(encoded.size(), 4u);
    auto decoded = base64_decode(encoded);
    EXPECT_EQ(decoded, data);
}

TEST(QA148Crypto, Base64RoundTrip3Bytes) {
    std::vector<uint8_t> data = {0x42, 0x43, 0x44};
    auto encoded = base64_encode(data);
    // 3 bytes → 4 chars, no padding
    EXPECT_EQ(encoded.size(), 4u);
    EXPECT_NE(encoded.back(), '=');
    auto decoded = base64_decode(encoded);
    EXPECT_EQ(decoded, data);
}

TEST(QA148Crypto, Base64DecodeInvalidChars) {
    // Invalid base64 chars should be skipped.
    auto decoded = base64_decode("!!!!");
    EXPECT_TRUE(decoded.empty());
}

TEST(QA148Crypto, Base64DecodeJustPadding) {
    auto decoded = base64_decode("====");
    EXPECT_TRUE(decoded.empty());
}

// =============================================================================
// Auth method parsing edge cases
// =============================================================================

TEST(QA148AuthParsing, EmptyString) {
    auto r = parse_auth_method("");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA148AuthParsing, WhitespaceOnly) {
    auto r = parse_auth_method("  ");
    EXPECT_FALSE(r.has_value());
}

TEST(QA148AuthParsing, NearMiss) {
    auto r = parse_auth_method("md6");
    EXPECT_FALSE(r.has_value());
}

// =============================================================================
// Password hashing edge cases
// =============================================================================

TEST(QA148Hashing, Md5EmptyPassword) {
    auto r = hash_password_md5("user", "");
    EXPECT_EQ(r.username, "user");
    EXPECT_TRUE(r.password_hash.substr(0, 3) == "md5");
    EXPECT_EQ(r.password_hash.size(), 35u);
}

TEST(QA148Hashing, Md5EmptyUsername) {
    auto r = hash_password_md5("", "password");
    EXPECT_TRUE(r.password_hash.substr(0, 3) == "md5");
}

TEST(QA148Hashing, ScramEmptyPassword) {
    auto r = hash_password_scram("user", "");
    EXPECT_EQ(r.username, "user");
    EXPECT_TRUE(r.password_hash.substr(0, 14) == "SCRAM-SHA-256$");
}

TEST(QA148Hashing, ScramVeryLongPassword) {
    std::string long_pass(10000, 'x');
    auto r = hash_password_scram("user", long_pass);
    EXPECT_TRUE(r.password_hash.substr(0, 14) == "SCRAM-SHA-256$");
}

// =============================================================================
// MD5 verification edge cases
// =============================================================================

TEST(QA148Md5Verify, EmptyStoredHash) {
    std::array<uint8_t, 4> salt = {1, 2, 3, 4};
    EXPECT_FALSE(verify_md5_password("", "user", salt, "md5anything"));
}

TEST(QA148Md5Verify, StoredHashTooShort) {
    std::array<uint8_t, 4> salt = {1, 2, 3, 4};
    EXPECT_FALSE(verify_md5_password("md", "user", salt, "md5anything"));
}

TEST(QA148Md5Verify, EmptyClientResponse) {
    auto record = hash_password_md5("user", "pass");
    std::array<uint8_t, 4> salt = {1, 2, 3, 4};
    EXPECT_FALSE(verify_md5_password(record.password_hash, "user", salt, ""));
}

TEST(QA148Md5Verify, AllZeroSalt) {
    auto record = hash_password_md5("user", "pass");
    std::array<uint8_t, 4> salt = {0, 0, 0, 0};
    std::string inner = record.password_hash.substr(3);
    std::string expected = "md5" + md5_hex(inner + "00000000");
    EXPECT_TRUE(verify_md5_password(record.password_hash, "user", salt, expected));
}

TEST(QA148Md5Verify, AllFFSalt) {
    auto record = hash_password_md5("user", "pass");
    std::array<uint8_t, 4> salt = {0xFF, 0xFF, 0xFF, 0xFF};
    std::string inner = record.password_hash.substr(3);
    std::string expected = "md5" + md5_hex(inner + "ffffffff");
    EXPECT_TRUE(verify_md5_password(record.password_hash, "user", salt, expected));
}

// =============================================================================
// SCRAM exchange edge cases
// =============================================================================

TEST(QA148Scram, ServerFirstMissingGS2Header) {
    auto record = hash_password_scram("user", "pass");
    ScramServerState state;
    // No GS2 header prefix — should fail.
    auto r = scram_server_first("n=user,r=somenonce", record, state);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::AUTH_ERROR);
}

TEST(QA148Scram, ServerFirstMissingNonce) {
    auto record = hash_password_scram("user", "pass");
    ScramServerState state;
    // Valid GS2 header but no 'r=' attribute.
    auto r = scram_server_first("n,,n=user", record, state);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::AUTH_ERROR);
}

TEST(QA148Scram, ServerFirstEmptyMessage) {
    auto record = hash_password_scram("user", "pass");
    ScramServerState state;
    auto r = scram_server_first("", record, state);
    EXPECT_FALSE(r.has_value());
}

TEST(QA148Scram, ServerFinalNonceMismatch) {
    auto record = hash_password_scram("user", "pass");
    ScramServerState state;
    std::string nonce = base64_encode(random_bytes(18));
    auto sf = scram_server_first("n,,n=user,r=" + nonce, record, state);
    ASSERT_TRUE(sf.has_value());

    // Client final with wrong nonce.
    auto r = scram_server_final("c=biws,r=WRONG_NONCE,p=AAAA", state);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::AUTH_ERROR);
}

TEST(QA148Scram, ServerFinalMissingProof) {
    auto record = hash_password_scram("user", "pass");
    ScramServerState state;
    std::string nonce = base64_encode(random_bytes(18));
    auto sf = scram_server_first("n,,n=user,r=" + nonce, record, state);
    ASSERT_TRUE(sf.has_value());

    // Client final without ',p=' separator.
    auto r = scram_server_final("c=biws,r=" + state.server_nonce, state);
    EXPECT_FALSE(r.has_value());
}

TEST(QA148Scram, CorruptedPasswordHash) {
    UserRecord record;
    record.username = "user";
    record.password_hash = "SCRAM-SHA-256$4096:AAAA$INVALID"; // No colon in keys.
    record.salt = base64_encode(random_bytes(16));
    record.iterations = 4096;

    ScramServerState state;
    auto r = scram_server_first("n,,n=user,r=somenonce", record, state);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::AUTH_ERROR);
}

// =============================================================================
// UserManager edge cases
// =============================================================================

TEST(QA148UserMgr, CreateAndGetUser) {
    UserManager mgr;
    auto r = mgr.create_user("test", "pass", AuthMethod::MD5);
    ASSERT_TRUE(r.has_value());

    auto user = mgr.get_user("test");
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->username, "test");
}

TEST(QA148UserMgr, DropAndRecreate) {
    UserManager mgr;
    (void)mgr.create_user("temp", "pass1", AuthMethod::MD5);
    (void)mgr.drop_user("temp", false);
    EXPECT_FALSE(mgr.user_exists("temp"));

    auto r = mgr.create_user("temp", "pass2", AuthMethod::MD5);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(mgr.user_exists("temp"));
}

TEST(QA148UserMgr, AlterFromMd5ToScram) {
    UserManager mgr;
    (void)mgr.create_user("switcher", "pass", AuthMethod::MD5);
    auto before = mgr.get_user("switcher");
    ASSERT_TRUE(before.has_value());
    EXPECT_TRUE(before->password_hash.substr(0, 3) == "md5");

    auto r = mgr.alter_user("switcher", "pass", AuthMethod::SCRAM_SHA_256);
    ASSERT_TRUE(r.has_value());
    auto after = mgr.get_user("switcher");
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(after->password_hash.substr(0, 14) == "SCRAM-SHA-256$");
}

TEST(QA148UserMgr, EnsureDefaultAdminDoesNotOverwrite) {
    UserManager mgr;
    (void)mgr.create_user("giodb", "custom_pass", AuthMethod::MD5);
    auto before = mgr.get_user("giodb");

    mgr.ensure_default_admin(AuthMethod::SCRAM_SHA_256);

    auto after = mgr.get_user("giodb");
    ASSERT_TRUE(after.has_value());
    // Should NOT have been overwritten to SCRAM.
    EXPECT_EQ(before->password_hash, after->password_hash);
}

TEST(QA148UserMgr, ConcurrentCreates) {
    UserManager mgr;
    std::vector<std::thread> threads;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&mgr, &successes, &failures, i] {
            auto r = mgr.create_user("user" + std::to_string(i), "pass", AuthMethod::MD5);
            if (r.has_value()) {
                ++successes;
            } else {
                ++failures;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successes.load(), 10);
    EXPECT_EQ(failures.load(), 0);
}

TEST(QA148UserMgr, ConcurrentCreateSameUser) {
    UserManager mgr;
    std::vector<std::thread> threads;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&mgr, &successes, &failures] {
            auto r = mgr.create_user("contested", "pass", AuthMethod::MD5);
            if (r.has_value()) {
                ++successes;
            } else {
                ++failures;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successes.load(), 1);
    EXPECT_EQ(failures.load(), 9);
}

TEST(QA148UserMgr, DropIfExistsIdempotent) {
    UserManager mgr;
    for (int i = 0; i < 5; ++i) {
        auto r = mgr.drop_user("ghost", true);
        EXPECT_TRUE(r.has_value()) << "iteration " << i;
    }
}

TEST(QA148UserMgr, EmptyUsernameAllowed) {
    UserManager mgr;
    auto r = mgr.create_user("", "pass", AuthMethod::MD5);
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(mgr.user_exists(""));
}

// =============================================================================
// Wire-protocol auth integration
// =============================================================================

namespace {

int create_socketpair148(int& client_fd_out) {
    int fds[2];
    int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(rc, 0);
    client_fd_out = fds[1];
    return fds[0];
}

void write_to_fd148(int fd, const std::vector<uint8_t>& data) {
    size_t written = 0;
    while (written < data.size()) {
        auto n = ::write(fd, data.data() + written, data.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> read_from_fd148(int fd, size_t max_bytes = 16384) {
    std::vector<uint8_t> buf(max_bytes);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) return {};
    buf.resize(static_cast<size_t>(n));
    return buf;
}

bool find_message148(const std::vector<uint8_t>& data, size_t& pos, uint8_t type,
                     const uint8_t*& payload, size_t& payload_len) {
    while (pos + 5 <= data.size()) {
        uint8_t msg_type = data[pos];
        uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 24) |
                          (static_cast<uint32_t>(data[pos + 2]) << 16) |
                          (static_cast<uint32_t>(data[pos + 3]) << 8) |
                          static_cast<uint32_t>(data[pos + 4]);
        size_t total = 1 + static_cast<size_t>(length);
        if (pos + total > data.size()) return false;
        if (msg_type == type) {
            payload = data.data() + pos + 5;
            payload_len = length - 4;
            pos += total;
            return true;
        }
        pos += total;
    }
    return false;
}

bool has_message148(const std::vector<uint8_t>& data, uint8_t type) {
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    return find_message148(data, pos, type, payload, payload_len);
}

std::vector<uint8_t> build_startup148(
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<uint8_t> payload;
    uint32_t version = 0x00030000;
    payload.push_back(static_cast<uint8_t>((version >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((version >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(version & 0xFF));
    for (const auto& [key, val] : params) {
        payload.insert(payload.end(), key.begin(), key.end());
        payload.push_back(0);
        payload.insert(payload.end(), val.begin(), val.end());
        payload.push_back(0);
    }
    payload.push_back(0);
    uint32_t total_len = static_cast<uint32_t>(4 + payload.size());
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>((total_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((total_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(total_len & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

/// Build a PasswordMessage ('p').
std::vector<uint8_t> build_password_message(const std::string& password) {
    std::vector<uint8_t> msg;
    msg.push_back('p');
    uint32_t body_len = static_cast<uint32_t>(4 + password.size() + 1);
    msg.push_back(static_cast<uint8_t>((body_len >> 24) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((body_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(body_len & 0xFF));
    msg.insert(msg.end(), password.begin(), password.end());
    msg.push_back(0);
    return msg;
}

} // namespace

// --------------------------------------------------------------------------
// Trust auth: immediate AuthOk
// --------------------------------------------------------------------------

TEST(QA148Wire, TrustAuthImmediateOk) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);
    // Trust is default, no need to set.

    auto startup = build_startup148({{"user", "anyone"}, {"database", "test"}});
    write_to_fd148(client_fd, startup);

    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::READY);

    (void)conn.write_to_socket();
    auto resp = read_from_fd148(client_fd);

    // Should have AuthOk ('R'), ParameterStatus ('S'), BackendKeyData ('K'), ReadyForQuery ('Z').
    EXPECT_TRUE(has_message148(resp, 'R'));
    EXPECT_TRUE(has_message148(resp, 'Z'));

    conn.close();
    ::close(client_fd);
}

// --------------------------------------------------------------------------
// MD5 auth: correct password
// --------------------------------------------------------------------------

TEST(QA148Wire, Md5AuthCorrectPassword) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    UserManager mgr;
    (void)mgr.create_user("alice", "secret", AuthMethod::MD5);
    handler.set_auth(AuthMethod::MD5, &mgr);

    // Step 1: Send startup.
    auto startup = build_startup148({{"user", "alice"}, {"database", "test"}});
    write_to_fd148(client_fd, startup);
    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_AUTH);

    // Read the AuthenticationMD5Password message to get the salt.
    (void)conn.write_to_socket();
    auto resp1 = read_from_fd148(client_fd);
    size_t pos = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    ASSERT_TRUE(find_message148(resp1, pos, 'R', payload, payload_len));
    ASSERT_GE(payload_len, 8u);
    // Auth type should be 5 (MD5Password).
    MessageReader auth_reader(payload, payload_len);
    int32_t auth_type = auth_reader.read_int32();
    EXPECT_EQ(auth_type, 5);
    // Next 4 bytes are the salt.
    std::array<uint8_t, 4> salt{};
    salt[0] = auth_reader.read_byte();
    salt[1] = auth_reader.read_byte();
    salt[2] = auth_reader.read_byte();
    salt[3] = auth_reader.read_byte();

    // Step 2: Compute MD5 response and send.
    auto user_record = mgr.get_user("alice");
    ASSERT_TRUE(user_record.has_value());
    std::string inner_hash = user_record->password_hash.substr(3);
    std::string salt_hex;
    for (auto b : salt) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        salt_hex += hex;
    }
    std::string client_response = "md5" + md5_hex(inner_hash + salt_hex);

    auto pw_msg = build_password_message(client_response);
    write_to_fd148(client_fd, pw_msg);
    rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::READY);

    (void)conn.write_to_socket();
    auto resp2 = read_from_fd148(client_fd);
    EXPECT_TRUE(has_message148(resp2, 'Z'));

    conn.close();
    ::close(client_fd);
}

// --------------------------------------------------------------------------
// MD5 auth: wrong password
// --------------------------------------------------------------------------

TEST(QA148Wire, Md5AuthWrongPassword) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    UserManager mgr;
    (void)mgr.create_user("alice", "secret", AuthMethod::MD5);
    handler.set_auth(AuthMethod::MD5, &mgr);

    auto startup = build_startup148({{"user", "alice"}, {"database", "test"}});
    write_to_fd148(client_fd, startup);
    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_AUTH);

    // Drain the MD5 challenge.
    (void)conn.write_to_socket();
    (void)read_from_fd148(client_fd);

    // Send wrong password.
    auto pw_msg = build_password_message("md5thisiswrong1234567890abcdef12");
    write_to_fd148(client_fd, pw_msg);
    rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    (void)conn.write_to_socket();
    auto resp = read_from_fd148(client_fd);
    // Should get ErrorResponse ('E').
    EXPECT_TRUE(has_message148(resp, 'E'));

    conn.close();
    ::close(client_fd);
}

// --------------------------------------------------------------------------
// MD5 auth: nonexistent user
// --------------------------------------------------------------------------

TEST(QA148Wire, Md5AuthNonexistentUser) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    UserManager mgr;
    handler.set_auth(AuthMethod::MD5, &mgr);

    auto startup = build_startup148({{"user", "nobody"}, {"database", "test"}});
    write_to_fd148(client_fd, startup);
    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());

    // Should be CLOSED — user doesn't exist.
    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    (void)conn.write_to_socket();
    auto resp = read_from_fd148(client_fd);
    EXPECT_TRUE(has_message148(resp, 'E'));

    conn.close();
    ::close(client_fd);
}

// --------------------------------------------------------------------------
// SCRAM auth: nonexistent user
// --------------------------------------------------------------------------

TEST(QA148Wire, ScramAuthNonexistentUser) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    UserManager mgr;
    handler.set_auth(AuthMethod::SCRAM_SHA_256, &mgr);

    auto startup = build_startup148({{"user", "nobody"}, {"database", "test"}});
    write_to_fd148(client_fd, startup);
    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());

    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    (void)conn.write_to_socket();
    auto resp = read_from_fd148(client_fd);
    EXPECT_TRUE(has_message148(resp, 'E'));

    conn.close();
    ::close(client_fd);
}

// --------------------------------------------------------------------------
// Auth: non-password message during auth phase
// --------------------------------------------------------------------------

TEST(QA148Wire, NonPasswordMessageDuringAuth) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    UserManager mgr;
    (void)mgr.create_user("alice", "pass", AuthMethod::MD5);
    handler.set_auth(AuthMethod::MD5, &mgr);

    auto startup = build_startup148({{"user", "alice"}, {"database", "test"}});
    write_to_fd148(client_fd, startup);
    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(handler.state(), ProtocolState::WAIT_FOR_AUTH);

    (void)conn.write_to_socket();
    (void)read_from_fd148(client_fd);

    // Send a Query message instead of PasswordMessage.
    std::vector<uint8_t> query_msg = {'Q', 0, 0, 0, 12};
    query_msg.insert(query_msg.end(), {'S', 'E', 'L', 'E', 'C', 'T', ' ', 0});
    write_to_fd148(client_fd, query_msg);
    rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());

    // Should close — wrong message type.
    EXPECT_EQ(handler.state(), ProtocolState::CLOSED);

    conn.close();
    ::close(client_fd);
}

// --------------------------------------------------------------------------
// Trust auth: no user param in startup
// --------------------------------------------------------------------------

TEST(QA148Wire, TrustAuthNoUserParam) {
    int client_fd = -1;
    int server_fd = create_socketpair148(client_fd);
    Connection conn(server_fd);
    PgProtocolHandler handler(1);

    // Startup without "user" parameter.
    auto startup = build_startup148({{"database", "test"}});
    write_to_fd148(client_fd, startup);
    auto rr = conn.read_from_socket();
    ASSERT_TRUE(rr.has_value());
    auto pr = handler.process(conn);
    ASSERT_TRUE(pr.has_value());

    // Trust mode should still succeed even without user.
    EXPECT_EQ(handler.state(), ProtocolState::READY);

    conn.close();
    ::close(client_fd);
}
