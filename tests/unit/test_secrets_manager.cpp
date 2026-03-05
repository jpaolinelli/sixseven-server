#include "sixseven/common/secrets_manager.h"
#include "sixseven/common/secure_string.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace sixseven;

// =============================================================================
// Test fixture for SecretsManager (GDB-191)
// =============================================================================

class SecretsManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_secrets";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
        key_path_ = (test_dir_ / "master.key").string();
    }

    void TearDown() override { std::filesystem::remove_all(test_dir_); }

    std::filesystem::path test_dir_;
    std::string key_path_;
};

// ---------------------------------------------------------------------------
// Master key generation and loading
// ---------------------------------------------------------------------------

TEST_F(SecretsManagerTest, AutoGenerateKeyOnFirstInit) {
    // Key file should not exist yet.
    ASSERT_FALSE(std::filesystem::exists(key_path_));

    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    // Key file should now exist with correct size (32 bytes).
    ASSERT_TRUE(std::filesystem::exists(key_path_));
    EXPECT_EQ(std::filesystem::file_size(key_path_), SecretsManager::kKeySize);
}

TEST_F(SecretsManagerTest, KeyFilePermissions) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    auto perms = std::filesystem::status(key_path_).permissions();
    // Owner read/write only (0600).
    EXPECT_NE(perms & std::filesystem::perms::owner_read, std::filesystem::perms::none);
    EXPECT_NE(perms & std::filesystem::perms::owner_write, std::filesystem::perms::none);
    EXPECT_EQ(perms & std::filesystem::perms::group_read, std::filesystem::perms::none);
    EXPECT_EQ(perms & std::filesystem::perms::group_write, std::filesystem::perms::none);
    EXPECT_EQ(perms & std::filesystem::perms::others_read, std::filesystem::perms::none);
    EXPECT_EQ(perms & std::filesystem::perms::others_write, std::filesystem::perms::none);
}

TEST_F(SecretsManagerTest, LoadExistingKey) {
    // First create generates the key.
    auto mgr1 = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr1.has_value()) << mgr1.error().message;

    // Encrypt something with the first manager.
    auto encrypted = mgr1->encrypt("test-secret");
    ASSERT_TRUE(encrypted.has_value()) << encrypted.error().message;

    // Second create loads the same key.
    auto mgr2 = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr2.has_value()) << mgr2.error().message;

    // Decrypting with the second manager should produce the same plaintext.
    auto decrypted = mgr2->decrypt(*encrypted);
    ASSERT_TRUE(decrypted.has_value()) << decrypted.error().message;
    EXPECT_EQ(*decrypted, "test-secret");
}

TEST_F(SecretsManagerTest, InvalidKeyFileTooShort) {
    // Write a truncated key file.
    std::ofstream f(key_path_, std::ios::binary);
    f << "short";
    f.close();

    auto mgr = SecretsManager::create(key_path_);
    ASSERT_FALSE(mgr.has_value());
    EXPECT_EQ(mgr.error().code, StatusCode::IO_ERROR);
}

TEST_F(SecretsManagerTest, CreateParentDirectories) {
    auto nested_path = (test_dir_ / "sub" / "dir" / "master.key").string();
    auto mgr = SecretsManager::create(nested_path);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;
    EXPECT_TRUE(std::filesystem::exists(nested_path));
}

// ---------------------------------------------------------------------------
// Encrypt/decrypt round-trip
// ---------------------------------------------------------------------------

TEST_F(SecretsManagerTest, EncryptDecryptRoundTrip) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    std::string secret = "sk-proj-1234567890abcdef";
    auto encrypted = mgr->encrypt(secret);
    ASSERT_TRUE(encrypted.has_value()) << encrypted.error().message;

    // Encrypted value should differ from plaintext.
    EXPECT_NE(*encrypted, secret);

    auto decrypted = mgr->decrypt(*encrypted);
    ASSERT_TRUE(decrypted.has_value()) << decrypted.error().message;
    EXPECT_EQ(*decrypted, secret);
}

TEST_F(SecretsManagerTest, EncryptDecryptEmptyString) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    auto encrypted = mgr->encrypt("");
    ASSERT_TRUE(encrypted.has_value()) << encrypted.error().message;

    auto decrypted = mgr->decrypt(*encrypted);
    ASSERT_TRUE(decrypted.has_value()) << decrypted.error().message;
    EXPECT_EQ(*decrypted, "");
}

TEST_F(SecretsManagerTest, EncryptDecryptLongString) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    // A long API key (some services have very long keys).
    std::string secret(1024, 'A');
    auto encrypted = mgr->encrypt(secret);
    ASSERT_TRUE(encrypted.has_value()) << encrypted.error().message;

    auto decrypted = mgr->decrypt(*encrypted);
    ASSERT_TRUE(decrypted.has_value()) << decrypted.error().message;
    EXPECT_EQ(*decrypted, secret);
}

TEST_F(SecretsManagerTest, UniqueNoncePerEncryption) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    std::string secret = "same-secret";
    auto enc1 = mgr->encrypt(secret);
    auto enc2 = mgr->encrypt(secret);
    ASSERT_TRUE(enc1.has_value());
    ASSERT_TRUE(enc2.has_value());

    // Same plaintext should produce different ciphertext (different nonce).
    EXPECT_NE(*enc1, *enc2);

    // Both should decrypt to the same value.
    auto dec1 = mgr->decrypt(*enc1);
    auto dec2 = mgr->decrypt(*enc2);
    ASSERT_TRUE(dec1.has_value());
    ASSERT_TRUE(dec2.has_value());
    EXPECT_EQ(*dec1, secret);
    EXPECT_EQ(*dec2, secret);
}

TEST_F(SecretsManagerTest, SpecialCharacters) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    std::string secret = "sk-proj_key!@#$%^&*()+={}[]|\\:\";<>?,./~`";
    auto encrypted = mgr->encrypt(secret);
    ASSERT_TRUE(encrypted.has_value()) << encrypted.error().message;

    auto decrypted = mgr->decrypt(*encrypted);
    ASSERT_TRUE(decrypted.has_value()) << decrypted.error().message;
    EXPECT_EQ(*decrypted, secret);
}

// ---------------------------------------------------------------------------
// Decryption error cases
// ---------------------------------------------------------------------------

TEST_F(SecretsManagerTest, DecryptInvalidBase64) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    auto result = mgr->decrypt("not-valid-base64!!!");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SecretsManagerTest, DecryptTooShort) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    auto result = mgr->decrypt("AQID"); // Very short base64.
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(SecretsManagerTest, DecryptTamperedCiphertext) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value()) << mgr.error().message;

    auto encrypted = mgr->encrypt("secret-key");
    ASSERT_TRUE(encrypted.has_value());

    // Tamper with the encrypted value (flip a character in the middle).
    std::string tampered = *encrypted;
    if (tampered.size() > 10) {
        tampered[10] = (tampered[10] == 'A') ? 'B' : 'A';
    }

    auto result = mgr->decrypt(tampered);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(SecretsManagerTest, DecryptWithWrongKey) {
    // Create two managers with different keys.
    auto mgr1_path = (test_dir_ / "key1.key").string();
    auto mgr2_path = (test_dir_ / "key2.key").string();

    auto mgr1 = SecretsManager::create(mgr1_path);
    auto mgr2 = SecretsManager::create(mgr2_path);
    ASSERT_TRUE(mgr1.has_value());
    ASSERT_TRUE(mgr2.has_value());

    auto encrypted = mgr1->encrypt("secret");
    ASSERT_TRUE(encrypted.has_value());

    // Decrypting with a different key should fail.
    auto result = mgr2->decrypt(*encrypted);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// looks_encrypted heuristic
// ---------------------------------------------------------------------------

TEST_F(SecretsManagerTest, LooksEncryptedPlaintext) {
    // Short strings should not look encrypted.
    EXPECT_FALSE(SecretsManager::looks_encrypted(""));
    EXPECT_FALSE(SecretsManager::looks_encrypted("sk-12345"));
    EXPECT_FALSE(SecretsManager::looks_encrypted("short"));
}

TEST_F(SecretsManagerTest, LooksEncryptedActualCiphertext) {
    auto mgr = SecretsManager::create(key_path_);
    ASSERT_TRUE(mgr.has_value());

    auto encrypted = mgr->encrypt("sk-test-key");
    ASSERT_TRUE(encrypted.has_value());

    EXPECT_TRUE(SecretsManager::looks_encrypted(*encrypted));
}

TEST_F(SecretsManagerTest, LooksEncryptedInvalidChars) {
    // Long string but with invalid base64 characters.
    std::string invalid_b64(50, '!');
    EXPECT_FALSE(SecretsManager::looks_encrypted(invalid_b64));
}

// ---------------------------------------------------------------------------
// SecureString
// ---------------------------------------------------------------------------

TEST_F(SecretsManagerTest, SecureStringZerosMemory) {
    // We can't directly observe memory zeroing, but we can verify the API works.
    SecureString ss("sensitive-data");
    EXPECT_EQ(ss.str(), "sensitive-data");
    EXPECT_EQ(ss.size(), 14u);
    EXPECT_FALSE(ss.empty());

    ss.clear();
    EXPECT_TRUE(ss.empty());
    EXPECT_EQ(ss.size(), 0u);
}

TEST_F(SecretsManagerTest, SecureStringCopyAndMove) {
    SecureString ss1("secret");
    SecureString ss2 = ss1; // Copy.
    EXPECT_EQ(ss1.str(), "secret");
    EXPECT_EQ(ss2.str(), "secret");

    SecureString ss3 = std::move(ss1); // Move.
    EXPECT_EQ(ss3.str(), "secret");
}

TEST_F(SecretsManagerTest, SecureStringComparison) {
    SecureString ss1("abc");
    SecureString ss2("abc");
    SecureString ss3("xyz");

    EXPECT_TRUE(ss1 == ss2);
    EXPECT_FALSE(ss1 == ss3);
    EXPECT_TRUE(ss1 == std::string("abc"));
}

TEST_F(SecretsManagerTest, SecureStringImplicitConversion) {
    SecureString ss("hello");
    const std::string& ref = ss; // Implicit conversion.
    EXPECT_EQ(ref, "hello");
}
