#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/server/auth.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Auth method parsing
// =============================================================================

TEST(AuthMethodParsing, TrustMethod) {
    auto method = parse_auth_method("trust");
    ASSERT_TRUE(method.has_value()) << method.error().message;
    EXPECT_EQ(*method, AuthMethod::TRUST);
}

TEST(AuthMethodParsing, Md5Method) {
    auto method = parse_auth_method("md5");
    ASSERT_TRUE(method.has_value()) << method.error().message;
    EXPECT_EQ(*method, AuthMethod::MD5);
}

TEST(AuthMethodParsing, ScramSha256Method) {
    auto method = parse_auth_method("scram-sha-256");
    ASSERT_TRUE(method.has_value()) << method.error().message;
    EXPECT_EQ(*method, AuthMethod::SCRAM_SHA_256);
}

TEST(AuthMethodParsing, CaseInsensitive) {
    auto upper = parse_auth_method("TRUST");
    ASSERT_TRUE(upper.has_value()) << upper.error().message;
    EXPECT_EQ(*upper, AuthMethod::TRUST);

    auto mixed = parse_auth_method("Md5");
    ASSERT_TRUE(mixed.has_value()) << mixed.error().message;
    EXPECT_EQ(*mixed, AuthMethod::MD5);

    auto scram = parse_auth_method("SCRAM-SHA-256");
    ASSERT_TRUE(scram.has_value()) << scram.error().message;
    EXPECT_EQ(*scram, AuthMethod::SCRAM_SHA_256);
}

TEST(AuthMethodParsing, InvalidMethodReturnsError) {
    auto method = parse_auth_method("kerberos");
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(AuthMethodParsing, EmptyStringReturnsError) {
    auto method = parse_auth_method("");
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);
}

// GDB-1221: parse_auth_method's case-folding must handle high-bit (non-ASCII)
// bytes safely (no UB from signed-char promotion to std::tolower) and must
// still reject the input since no known auth method contains such bytes.
TEST(AuthMethodParsing, HighBitByteDoesNotCrashAndIsRejected) {
    std::string input = "md5";
    input += static_cast<char>(0xFF);
    auto method = parse_auth_method(input);
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Crypto helpers
// =============================================================================

TEST(CryptoHelpers, Md5HexProducesCorrectLength) {
    auto hash = md5_hex("hello");
    EXPECT_EQ(hash.size(), 32u);
}

TEST(CryptoHelpers, Md5HexDeterministic) {
    auto h1 = md5_hex("test_input");
    auto h2 = md5_hex("test_input");
    EXPECT_EQ(h1, h2);
}

TEST(CryptoHelpers, Md5HexDifferentInputsDifferentHashes) {
    auto h1 = md5_hex("input_a");
    auto h2 = md5_hex("input_b");
    EXPECT_NE(h1, h2);
}

// Known-answer RFC vector: md5("hello") = 5d41402abc4b2a76b9719d911017c592.
// Independently verifiable: echo -n hello | md5sum
// This pins the md5_hex implementation against an externally-computable constant.
TEST(CryptoHelpers, Md5HexKnownAnswerHello) {
    EXPECT_EQ(md5_hex("hello"), "5d41402abc4b2a76b9719d911017c592");
}

TEST(CryptoHelpers, Sha256ProducesCorrectLength) {
    std::vector<uint8_t> data = {'h', 'e', 'l', 'l', 'o'};
    auto hash = sha256(data);
    EXPECT_EQ(hash.size(), 32u);
}

TEST(CryptoHelpers, Sha256Deterministic) {
    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    auto h1 = sha256(data);
    auto h2 = sha256(data);
    EXPECT_EQ(h1, h2);
}

TEST(CryptoHelpers, HmacSha256ProducesCorrectLength) {
    std::vector<uint8_t> key = {'k', 'e', 'y'};
    auto mac = hmac_sha256(key, "message");
    EXPECT_EQ(mac.size(), 32u);
}

TEST(CryptoHelpers, HmacSha256DeterministicWithSameKey) {
    std::vector<uint8_t> key = {'k', 'e', 'y'};
    auto m1 = hmac_sha256(key, "message");
    auto m2 = hmac_sha256(key, "message");
    EXPECT_EQ(m1, m2);
}

TEST(CryptoHelpers, HmacSha256DifferentKeysProduceDifferentMacs) {
    std::vector<uint8_t> key1 = {'k', 'e', 'y', '1'};
    std::vector<uint8_t> key2 = {'k', 'e', 'y', '2'};
    auto m1 = hmac_sha256(key1, "message");
    auto m2 = hmac_sha256(key2, "message");
    EXPECT_NE(m1, m2);
}

TEST(CryptoHelpers, XorBytesIdentity) {
    std::vector<uint8_t> a = {0x12, 0x34, 0x56};
    std::vector<uint8_t> b = {0x00, 0x00, 0x00};
    auto result = xor_bytes(a, b);
    EXPECT_EQ(result, a);
}

TEST(CryptoHelpers, XorBytesSelfIsZero) {
    std::vector<uint8_t> a = {0x12, 0x34, 0x56};
    auto result = xor_bytes(a, a);
    EXPECT_EQ(result, (std::vector<uint8_t>{0x00, 0x00, 0x00}));
}

TEST(CryptoHelpers, RandomBytesCorrectLength) {
    auto bytes = random_bytes(32);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->size(), 32u);
}

TEST(CryptoHelpers, RandomBytesNotAllZeroes) {
    auto bytes_result = random_bytes(32);
    ASSERT_TRUE(bytes_result.has_value());
    auto bytes = *bytes_result;
    bool all_zero = true;
    for (auto b : bytes) {
        if (b != 0) {
            all_zero = false;
            break;
        }
    }
    EXPECT_FALSE(all_zero);
}

TEST(CryptoHelpers, Base64RoundTrip) {
    std::vector<uint8_t> original = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
    auto encoded = base64_encode(original);
    auto decoded = base64_decode(encoded);
    EXPECT_EQ(decoded, original);
}

TEST(CryptoHelpers, Base64RoundTripRandomData) {
    auto original_result = random_bytes(64);
    ASSERT_TRUE(original_result.has_value());
    auto original = *original_result;
    auto encoded = base64_encode(original);
    auto decoded = base64_decode(encoded);
    EXPECT_EQ(decoded, original);
}

TEST(CryptoHelpers, Base64EncodeKnownValue) {
    // "Hello" in base64 is "SGVsbG8="
    std::vector<uint8_t> hello = {'H', 'e', 'l', 'l', 'o'};
    auto encoded = base64_encode(hello);
    EXPECT_EQ(encoded, "SGVsbG8=");
}

// =============================================================================
// Password hashing
// =============================================================================

TEST(PasswordHashing, Md5HashFormat) {
    auto record = hash_password_md5("alice", "secret");
    EXPECT_EQ(record.username, "alice");
    // MD5 hash starts with "md5" prefix.
    EXPECT_TRUE(record.password_hash.substr(0, 3) == "md5");
    // Total length: "md5" (3) + 32 hex chars = 35.
    EXPECT_EQ(record.password_hash.size(), 35u);
}

TEST(PasswordHashing, Md5HashDeterministic) {
    auto r1 = hash_password_md5("alice", "secret");
    auto r2 = hash_password_md5("alice", "secret");
    EXPECT_EQ(r1.password_hash, r2.password_hash);
}

TEST(PasswordHashing, Md5HashDifferentForDifferentPasswords) {
    auto r1 = hash_password_md5("alice", "password1");
    auto r2 = hash_password_md5("alice", "password2");
    EXPECT_NE(r1.password_hash, r2.password_hash);
}

TEST(PasswordHashing, Md5HashIncorporatesUsername) {
    auto r1 = hash_password_md5("alice", "secret");
    auto r2 = hash_password_md5("bob", "secret");
    EXPECT_NE(r1.password_hash, r2.password_hash);
}

// Wire-interop guard: PostgreSQL md5 auth stores md5(password + username).
// The stored hash for password="secret", username="alice" must equal
// "md5" + md5("secret" + "alice") = "md5" + md5("secretalice").
//
// Independently verifiable: echo -n secretalice | md5sum
//   => 4a0a68b43b6cd5cf266fa02f196e2371
//
// Wrong order md5("alice" + "secret") = md5("alicesecret")
//   => c4e31313222cf05fcdd1fc068af5570e  (distinct - regression is detectable)
//
// A regression in src/server/auth.cpp:253 that flips to md5(username+password)
// would produce "md5c4e31313222cf05fcdd1fc068af5570e" and FAIL this assertion,
// while breaking every real psql/libpq client that cannot authenticate.
TEST(PasswordHashing, Md5HashKnownAnswerPostgresWireOrder) {
    auto record = hash_password_md5("alice", "secret");
    EXPECT_EQ(record.password_hash, "md54a0a68b43b6cd5cf266fa02f196e2371");
}

TEST(PasswordHashing, ScramHashFormat) {
    auto record = hash_password_scram("alice", "secret");
    EXPECT_EQ(record.username, "alice");
    // SCRAM format: "SCRAM-SHA-256$iterations:salt$StoredKey:ServerKey"
    EXPECT_TRUE(record.password_hash.substr(0, 14) == "SCRAM-SHA-256$");
    EXPECT_EQ(record.iterations, 4096);
    EXPECT_FALSE(record.salt.empty());
}

TEST(PasswordHashing, ScramHashNeverStorePlaintext) {
    auto record = hash_password_scram("alice", "secret");
    // Password must never appear in the stored hash.
    EXPECT_EQ(record.password_hash.find("secret"), std::string::npos);
}

TEST(PasswordHashing, ScramHashDifferentSaltsEachTime) {
    auto r1 = hash_password_scram("alice", "secret");
    auto r2 = hash_password_scram("alice", "secret");
    // Because salt is random, hashes should differ.
    EXPECT_NE(r1.password_hash, r2.password_hash);
    EXPECT_NE(r1.salt, r2.salt);
}

// =============================================================================
// MD5 password verification
// =============================================================================

TEST(Md5Verification, CorrectPasswordVerifies) {
    auto record = hash_password_md5("alice", "secret");
    std::array<uint8_t, 4> salt = {0x01, 0x02, 0x03, 0x04};

    // The inner hash is the stored hash minus the "md5" prefix.
    std::string inner_hash = record.password_hash.substr(3);

    // Build the client response: "md5" + md5(inner_hash + salt_hex).
    std::string salt_hex;
    for (auto b : salt) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        salt_hex += hex;
    }
    std::string client_response = "md5" + md5_hex(inner_hash + salt_hex);

    EXPECT_TRUE(verify_md5_password(record.password_hash, "alice", salt, client_response));
}

TEST(Md5Verification, WrongPasswordFails) {
    auto record = hash_password_md5("alice", "secret");
    std::array<uint8_t, 4> salt = {0x01, 0x02, 0x03, 0x04};

    // Build client response with the wrong password.
    auto wrong_record = hash_password_md5("alice", "wrong");
    std::string inner_hash = wrong_record.password_hash.substr(3);
    std::string salt_hex;
    for (auto b : salt) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        salt_hex += hex;
    }
    std::string client_response = "md5" + md5_hex(inner_hash + salt_hex);

    EXPECT_FALSE(verify_md5_password(record.password_hash, "alice", salt, client_response));
}

TEST(Md5Verification, GarbageResponseFails) {
    auto record = hash_password_md5("alice", "secret");
    std::array<uint8_t, 4> salt = {0x01, 0x02, 0x03, 0x04};
    EXPECT_FALSE(verify_md5_password(record.password_hash, "alice", salt, "garbage"));
}

// =============================================================================
// SCRAM-SHA-256 exchange
// =============================================================================

TEST(ScramExchange, FullSuccessfulExchange) {
    // Hash the password.
    auto record = hash_password_scram("alice", "secret");

    // Step 1: Client sends client-first-message.
    auto client_nonce_bytes = random_bytes(18);
    ASSERT_TRUE(client_nonce_bytes.has_value());
    std::string client_nonce = base64_encode(*client_nonce_bytes);
    std::string client_first_bare = "n=alice,r=" + client_nonce;
    std::string client_first = "n,," + client_first_bare;

    ScramServerState state;
    auto server_first = scram_server_first(client_first, record, state);
    ASSERT_TRUE(server_first.has_value()) << server_first.error().message;
    // Server-first should contain the combined nonce, salt, and iterations.
    EXPECT_NE(server_first->find("r="), std::string::npos);
    EXPECT_NE(server_first->find("s="), std::string::npos);
    EXPECT_NE(server_first->find("i="), std::string::npos);

    // Step 2: Client computes proof and sends client-final-message.
    // Re-derive salted password.
    auto salt = base64_decode(record.salt);
    // PBKDF2 — we use the same algo as the server.
    // For this test, we derive the proof from scratch.
    // SaltedPassword = Hi(password, salt, iterations)
    // We need PBKDF2 which is in the anon namespace of auth.cpp,
    // so we use a different approach: derive ClientKey from StoredKey.
    // This is not how a real client works, but tests that the server
    // correctly verifies a proof built from the stored keys.

    // Parse stored key and server key from the hash.
    std::string keys_part = record.password_hash.substr(record.password_hash.rfind('$') + 1);
    auto colon_pos = keys_part.find(':');
    auto stored_key = base64_decode(keys_part.substr(0, colon_pos));
    auto server_key = base64_decode(keys_part.substr(colon_pos + 1));

    // AuthMessage = client-first-bare + "," + server-first + "," + client-final-without-proof
    std::string client_final_without_proof = "c=biws,r=" + state.server_nonce;
    std::string auth_message =
        client_first_bare + "," + *server_first + "," + client_final_without_proof;

    // ClientSignature = HMAC(StoredKey, AuthMessage)
    auto client_signature = hmac_sha256(stored_key, auth_message);

    // To compute ClientProof, we need ClientKey.
    // ClientKey = HMAC(SaltedPassword, "Client Key")
    // StoredKey = H(ClientKey)
    // We can't invert SHA-256, so we need to derive SaltedPassword from scratch.
    // Use internal PBKDF2 by re-hashing with the known salt/iterations.
    // Actually, we can compute from the original password since we know it:
    // Let's just build the salted password properly.
    auto pbkdf2 = [&](const std::string& password,
                      const std::vector<uint8_t>& s,
                      int32_t iter) -> std::vector<uint8_t> {
        std::vector<uint8_t> pass_key(password.begin(), password.end());
        std::string salt_str(s.begin(), s.end());
        salt_str += '\0';
        salt_str += '\0';
        salt_str += '\0';
        salt_str += '\1';
        auto u_prev = hmac_sha256(pass_key, salt_str);
        auto result = u_prev;
        for (int32_t i = 1; i < iter; ++i) {
            std::string u_str(u_prev.begin(), u_prev.end());
            u_prev = hmac_sha256(pass_key, u_str);
            result = xor_bytes(result, u_prev);
        }
        return result;
    };

    auto salted_password = pbkdf2("secret", salt, record.iterations);
    auto client_key = hmac_sha256(salted_password, "Client Key");
    auto client_proof = xor_bytes(client_key, client_signature);

    std::string client_final = client_final_without_proof + ",p=" + base64_encode(client_proof);

    auto server_final = scram_server_final(client_final, state);
    ASSERT_TRUE(server_final.has_value()) << server_final.error().message;
    // Server-final should contain the server signature.
    EXPECT_TRUE(server_final->substr(0, 2) == "v=");
}

TEST(ScramExchange, InvalidClientFirstMessage) {
    auto record = hash_password_scram("alice", "secret");
    ScramServerState state;
    auto result = scram_server_first("garbage", record, state);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST(ScramExchange, WrongProofFails) {
    auto record = hash_password_scram("alice", "secret");

    auto client_nonce_bytes = random_bytes(18);
    ASSERT_TRUE(client_nonce_bytes.has_value());
    std::string client_nonce = base64_encode(*client_nonce_bytes);
    std::string client_first = "n,,n=alice,r=" + client_nonce;

    ScramServerState state;
    auto server_first = scram_server_first(client_first, record, state);
    ASSERT_TRUE(server_first.has_value()) << server_first.error().message;

    // Send a bogus proof.
    auto bogus_proof_bytes = random_bytes(32);
    ASSERT_TRUE(bogus_proof_bytes.has_value());
    std::string client_final =
        "c=biws,r=" + state.server_nonce + ",p=" + base64_encode(*bogus_proof_bytes);

    auto server_final = scram_server_final(client_final, state);
    ASSERT_FALSE(server_final.has_value());
    EXPECT_EQ(server_final.error().code, StatusCode::AUTH_ERROR);
}

// =============================================================================
// UserManager CRUD
// =============================================================================

TEST(UserManager, CreateUserSucceeds) {
    UserManager mgr;
    auto result = mgr.create_user("alice", "secret", AuthMethod::SCRAM_SHA_256);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(mgr.user_exists("alice"));
}

TEST(UserManager, CreateDuplicateUserFails) {
    UserManager mgr;
    auto r1 = mgr.create_user("alice", "secret", AuthMethod::MD5);
    ASSERT_TRUE(r1.has_value());

    auto r2 = mgr.create_user("alice", "other", AuthMethod::MD5);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(UserManager, DropUserSucceeds) {
    UserManager mgr;
    (void)mgr.create_user("alice", "secret", AuthMethod::MD5);

    auto result = mgr.drop_user("alice", false);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(mgr.user_exists("alice"));
}

TEST(UserManager, DropNonexistentUserFails) {
    UserManager mgr;
    auto result = mgr.drop_user("bob", false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(UserManager, DropNonexistentUserWithIfExists) {
    UserManager mgr;
    auto result = mgr.drop_user("bob", true);
    EXPECT_TRUE(result.has_value());
}

TEST(UserManager, AlterUserPassword) {
    UserManager mgr;
    (void)mgr.create_user("alice", "old_password", AuthMethod::MD5);

    auto old_record = mgr.get_user("alice");
    ASSERT_TRUE(old_record.has_value());

    auto result = mgr.alter_user("alice", "new_password", AuthMethod::MD5);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto new_record = mgr.get_user("alice");
    ASSERT_TRUE(new_record.has_value());
    EXPECT_NE(old_record->password_hash, new_record->password_hash);
}

TEST(UserManager, AlterNonexistentUserFails) {
    UserManager mgr;
    auto result = mgr.alter_user("ghost", "pass", AuthMethod::MD5);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(UserManager, GetUserReturnsRecord) {
    UserManager mgr;
    (void)mgr.create_user("alice", "secret", AuthMethod::MD5);

    auto record = mgr.get_user("alice");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->username, "alice");
    // Password should be hashed, not plaintext.
    EXPECT_NE(record->password_hash, "secret");
}

TEST(UserManager, GetNonexistentUserReturnsNullopt) {
    UserManager mgr;
    auto record = mgr.get_user("nobody");
    EXPECT_FALSE(record.has_value());
}

TEST(UserManager, EnsureDefaultAdmin) {
    UserManager mgr;
    EXPECT_FALSE(mgr.user_exists("demo"));

    mgr.ensure_default_admin(AuthMethod::SCRAM_SHA_256);
    EXPECT_TRUE(mgr.user_exists("demo"));

    auto record = mgr.get_user("demo");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->username, "demo");
    // Should be SCRAM-formatted hash.
    EXPECT_TRUE(record->password_hash.substr(0, 14) == "SCRAM-SHA-256$");
}

TEST(UserManager, EnsureDefaultAdminIdempotent) {
    UserManager mgr;
    mgr.ensure_default_admin(AuthMethod::MD5);
    auto first_hash = mgr.get_user("demo")->password_hash;

    // Calling again should not overwrite.
    mgr.ensure_default_admin(AuthMethod::MD5);
    auto second_hash = mgr.get_user("demo")->password_hash;
    EXPECT_EQ(first_hash, second_hash);
}

TEST(UserManager, TrustModeStoresNoHash) {
    UserManager mgr;
    auto result = mgr.create_user("trustuser", "", AuthMethod::TRUST);
    ASSERT_TRUE(result.has_value());

    auto record = mgr.get_user("trustuser");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->password_hash.empty());
}

// =============================================================================
// Config auth_method
// =============================================================================

TEST(AuthConfig, DefaultAuthMethodIsScram) {
    Config config;
    EXPECT_EQ(config.auth_method, "scram-sha-256");
}

TEST(AuthConfig, AuthMethodFromConfigFile) {
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto config_path = tmp_dir / "sixseven_test_auth_config_md5.json";

    // Write config file with a NON-default auth_method ("md5").
    // If the parse branch in config.cpp were removed, auth_method would
    // fall back to the default "scram-sha-256" and this assertion would fail.
    {
        std::ofstream ofs(config_path);
        ofs << R"({"auth_method": "md5"})";
    }

    auto config = Config::load_from_file(config_path.string());
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->auth_method, "md5");

    std::filesystem::remove(config_path);
}

TEST(AuthConfig, AuthMethodDefaultsToScramWhenAbsentInFile) {
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto config_path = tmp_dir / "sixseven_test_auth_config_nokey.json";

    // Write a config file that has no auth_method key at all.
    // The field must fall back to the default "scram-sha-256" through the
    // file-load path (complementing DefaultAuthMethodIsScram which covers
    // the in-memory default).
    {
        std::ofstream ofs(config_path);
        ofs << R"({"port": 5432})";
    }

    auto config = Config::load_from_file(config_path.string());
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->auth_method, "scram-sha-256");

    std::filesystem::remove(config_path);
}

// =============================================================================
// Parser: CREATE/DROP/ALTER USER
// =============================================================================

class AuthParserTest : public ::testing::Test {
protected:
    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    UserManager user_mgr_;

    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_auth";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        engine_->set_user_manager(&user_mgr_);
        engine_->set_auth_method(AuthMethod::MD5);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }
};

TEST_F(AuthParserTest, CreateUser) {
    auto result = engine_->execute("CREATE USER alice WITH PASSWORD 'secret123'");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "CREATE USER");
    EXPECT_TRUE(user_mgr_.user_exists("alice"));
}

TEST_F(AuthParserTest, CreateDuplicateUserFails) {
    auto r1 = engine_->execute("CREATE USER bob WITH PASSWORD 'pass1'");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = engine_->execute("CREATE USER bob WITH PASSWORD 'pass2'");
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST_F(AuthParserTest, DropUser) {
    (void)engine_->execute("CREATE USER charlie WITH PASSWORD 'pass'");
    ASSERT_TRUE(user_mgr_.user_exists("charlie"));

    auto result = engine_->execute("DROP USER charlie");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "DROP USER");
    EXPECT_FALSE(user_mgr_.user_exists("charlie"));
}

TEST_F(AuthParserTest, DropUserIfExists) {
    auto result = engine_->execute("DROP USER IF EXISTS nonexistent");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "DROP USER");
}

TEST_F(AuthParserTest, DropNonexistentUserFails) {
    auto result = engine_->execute("DROP USER nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(AuthParserTest, AlterUserPassword) {
    (void)engine_->execute("CREATE USER dave WITH PASSWORD 'old_pass'");
    auto old_record = user_mgr_.get_user("dave");
    ASSERT_TRUE(old_record.has_value());

    auto result = engine_->execute("ALTER USER dave WITH PASSWORD 'new_pass'");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "ALTER USER");

    auto new_record = user_mgr_.get_user("dave");
    ASSERT_TRUE(new_record.has_value());
    EXPECT_NE(old_record->password_hash, new_record->password_hash);
}

TEST_F(AuthParserTest, AlterNonexistentUserFails) {
    auto result = engine_->execute("ALTER USER ghost WITH PASSWORD 'pass'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(AuthParserTest, CreateUserWithScramAuth) {
    engine_->set_auth_method(AuthMethod::SCRAM_SHA_256);
    auto result = engine_->execute("CREATE USER eve WITH PASSWORD 'secure'");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto record = user_mgr_.get_user("eve");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->password_hash.substr(0, 14) == "SCRAM-SHA-256$");
}

TEST_F(AuthParserTest, CreateUserWithTrustAuth) {
    engine_->set_auth_method(AuthMethod::TRUST);
    auto result = engine_->execute("CREATE USER trusty WITH PASSWORD 'ignored'");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto record = user_mgr_.get_user("trusty");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->password_hash.empty());
}

TEST_F(AuthParserTest, NoUserManagerReturnsError) {
    engine_->set_user_manager(nullptr);

    auto result = engine_->execute("CREATE USER fail WITH PASSWORD 'pass'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// Standby mode rejects user DDL
// =============================================================================

TEST_F(AuthParserTest, StandbyRejectsCreateUser) {
    engine_->set_standby_mode(true);

    auto result = engine_->execute("CREATE USER blocked WITH PASSWORD 'pass'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
}

TEST_F(AuthParserTest, StandbyRejectsDropUser) {
    engine_->set_standby_mode(true);

    auto result = engine_->execute("DROP USER blocked");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
}

TEST_F(AuthParserTest, StandbyRejectsAlterUser) {
    engine_->set_standby_mode(true);

    auto result = engine_->execute("ALTER USER blocked WITH PASSWORD 'pass'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::READ_ONLY);
}
