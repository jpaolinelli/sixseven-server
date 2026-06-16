// GDB-846: Adversarial QA tests for SecretsManager::decrypt()
//
// The bug: decrypt() returned ok() with empty/garbage plaintext when
// EVP_DecryptInit_ex or EVP_DecryptUpdate failed, because the error return
// was gated behind `success &&` and skipped, falling through to
// `return ok(std::move(plaintext))`.
//
// The fix: added `if (!success) return make_error(...)` after
// EVP_CIPHER_CTX_free, before returning plaintext.
//
// These tests attack the decrypt() path hard: truncation boundaries, bit-flips
// in every region, wrong key, binary plaintext, large plaintext, cross-message
// tag swap, and partial-plaintext exposure checks.

#include "sixseven/common/secrets_manager.h"
#include "sixseven/common/status.h"

#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Base64 encode without newlines (matches OpenSSL EVP_EncodeBlock output).
std::string b64_encode(const std::vector<uint8_t>& data) {
    size_t out_len = 4 * ((data.size() + 2) / 3) + 1;
    std::string result(out_len, '\0');
    int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                  data.data(),
                                  static_cast<int>(data.size()));
    result.resize(static_cast<size_t>(written));
    return result;
}

// Build a crafted encrypted blob: base64(raw_bytes).
// raw_bytes is whatever we want to feed to decrypt() post-base64-decode.
std::string make_crafted_blob(const std::vector<uint8_t>& raw_bytes) {
    return b64_encode(raw_bytes);
}

// Decrypt a real message, get the raw decoded bytes back out.
// We re-implement the decode here so we can mutate the wire bytes.
std::vector<uint8_t> get_raw_bytes(const std::string& b64) {
    std::vector<uint8_t> buf(b64.size());
    int n = EVP_DecodeBlock(buf.data(),
                            reinterpret_cast<const unsigned char*>(b64.data()),
                            static_cast<int>(b64.size()));
    if (n < 0) {
        return {};
    }
    // Adjust for padding.
    size_t padding = 0;
    if (!b64.empty() && b64.back() == '=') {
        ++padding;
    }
    if (b64.size() > 1 && b64[b64.size() - 2] == '=') {
        ++padding;
    }
    buf.resize(static_cast<size_t>(n) - padding);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB846_SecretsManagerAdversarial : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb846";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
        key_path_ = (test_dir_ / "master.key").string();
        key2_path_ = (test_dir_ / "master2.key").string();

        auto mgr = SecretsManager::create(key_path_);
        ASSERT_TRUE(mgr.has_value()) << mgr.error().message;
        mgr_ = std::make_unique<SecretsManager>(std::move(*mgr));

        auto mgr2 = SecretsManager::create(key2_path_);
        ASSERT_TRUE(mgr2.has_value()) << mgr2.error().message;
        mgr2_ = std::make_unique<SecretsManager>(std::move(*mgr2));
    }

    void TearDown() override { std::filesystem::remove_all(test_dir_); }

    std::filesystem::path test_dir_;
    std::string key_path_;
    std::string key2_path_;
    std::unique_ptr<SecretsManager> mgr_;
    std::unique_ptr<SecretsManager> mgr2_;
};

// ---------------------------------------------------------------------------
// AC: The fix exists — decrypt() errors (never ok()) when GCM tag is bad
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TamperedGCMTagReturnsAuthError) {
    auto enc = mgr_->encrypt("api-secret-key");
    ASSERT_TRUE(enc.has_value()) << enc.error().message;

    auto raw = get_raw_bytes(*enc);
    ASSERT_GE(raw.size(), SecretsManager::kNonceSize + SecretsManager::kTagSize);

    // Flip every byte of the GCM tag region (last kTagSize bytes).
    for (size_t i = raw.size() - SecretsManager::kTagSize; i < raw.size(); ++i) {
        raw[i] ^= 0xFF;
    }

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Truncation boundary: empty input
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TruncatedCiphertext_EmptyInput) {
    // Empty string → base64_decode returns error directly.
    auto result = mgr_->decrypt("");
    ASSERT_FALSE(result.has_value());
    // Could be INVALID_ARGUMENT from base64_decode or from size check — not ok().
    EXPECT_NE(result.error().code, StatusCode::OK);
}

// ---------------------------------------------------------------------------
// Truncation boundary: 1 raw byte (shorter than nonce)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TruncatedCiphertext_OneRawByte) {
    std::vector<uint8_t> raw = {0xAB};
    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Truncation boundary: nonce-length bytes only (missing tag and body)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TruncatedCiphertext_NonceOnly) {
    std::vector<uint8_t> raw(SecretsManager::kNonceSize, 0x42);
    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    // Size = kNonceSize < kNonceSize + kTagSize → INVALID_ARGUMENT.
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Truncation boundary: nonce + 1 byte (still shorter than nonce + tag)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TruncatedCiphertext_NoncePlusOneByte) {
    std::vector<uint8_t> raw(SecretsManager::kNonceSize + 1, 0x42);
    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Truncation boundary: nonce + tag - 1 (still one short)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TruncatedCiphertext_OneShortOfMinimum) {
    size_t min_size = SecretsManager::kNonceSize + SecretsManager::kTagSize;
    std::vector<uint8_t> raw(min_size - 1, 0x42);
    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Truncation boundary: valid ciphertext minus 1 body byte (tag is still
// appended but the body is truncated — GCM auth will fail)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_TruncatedCiphertext_BodyMinusOneByte) {
    auto enc = mgr_->encrypt("hello-world");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    ASSERT_GT(raw.size(), SecretsManager::kNonceSize + SecretsManager::kTagSize);

    // Remove one byte from the ciphertext body (before the tag).
    size_t tag_offset = raw.size() - SecretsManager::kTagSize;
    raw.erase(raw.begin() + static_cast<std::ptrdiff_t>(tag_offset) - 1);

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    // Auth tag won't match → AUTH_ERROR.
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Bit-flip in the IV/nonce region — authentication must fail
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_BitFlipInNonce_ReturnsError) {
    auto enc = mgr_->encrypt("some-sensitive-key");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    ASSERT_GE(raw.size(), SecretsManager::kNonceSize + SecretsManager::kTagSize);

    // Flip the first byte of the nonce (IV region).
    raw[0] ^= 0x01;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    // Wrong nonce → GCM decryption with wrong key stream → AUTH_ERROR.
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_BitFlipInNonceLastByte_ReturnsError) {
    auto enc = mgr_->encrypt("another-key");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    raw[SecretsManager::kNonceSize - 1] ^= 0x80;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Bit-flip in the ciphertext body region — authentication must fail
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_BitFlipInCiphertextBody_ReturnsAuthError) {
    auto enc = mgr_->encrypt("database-password-123");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    // Body is between nonce and tag.
    size_t body_start = SecretsManager::kNonceSize;
    size_t body_end = raw.size() - SecretsManager::kTagSize;
    ASSERT_GT(body_end, body_start) << "Need non-empty ciphertext body";

    // Flip the first byte of the body.
    raw[body_start] ^= 0x55;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial,
       GDB846_BitFlipInCiphertextBodyMidpoint_ReturnsAuthError) {
    auto enc = mgr_->encrypt("midpoint-flip-test-secret");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    size_t body_start = SecretsManager::kNonceSize;
    size_t body_end = raw.size() - SecretsManager::kTagSize;
    ASSERT_GT(body_end, body_start);

    size_t mid = body_start + (body_end - body_start) / 2;
    raw[mid] ^= 0xAA;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Bit-flip in the GCM tag only (body and nonce intact) — AUTH_ERROR
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_BitFlipInTagFirstByte_ReturnsAuthError) {
    auto enc = mgr_->encrypt("token-value");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    size_t tag_start = raw.size() - SecretsManager::kTagSize;
    raw[tag_start] ^= 0x01;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_BitFlipInTagLastByte_ReturnsAuthError) {
    auto enc = mgr_->encrypt("token-value-b");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    raw.back() ^= 0x01;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Wrong key — different manager, same ciphertext → AUTH_ERROR (never empty-ok)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_WrongKeyReturnsAuthErrorNotOk) {
    auto enc = mgr_->encrypt("secret-payload");
    ASSERT_TRUE(enc.has_value());

    // Decrypt with a completely different key.
    auto result = mgr2_->decrypt(*enc);
    ASSERT_FALSE(result.has_value());
    // GCM tag won't match with wrong key → AUTH_ERROR.
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_WrongKeyDoesNotReturnEmptyOk) {
    auto enc = mgr_->encrypt("critical-api-key-do-not-leak");
    ASSERT_TRUE(enc.has_value());

    auto result = mgr2_->decrypt(*enc);
    // This is the core bug regression: before fix, this returned ok("").
    // It MUST NOT return ok() with any value — empty string or otherwise.
    ASSERT_FALSE(result.has_value()) << "decrypt() must not return ok() with wrong key; got: '"
                                     << (result.has_value() ? *result : "<error>") << "'";
}

// ---------------------------------------------------------------------------
// Empty plaintext round-trip — must distinguish ok("") from error
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_EmptyPlaintextRoundTrip) {
    // A legitimately empty plaintext must round-trip correctly.
    auto enc = mgr_->encrypt("");
    ASSERT_TRUE(enc.has_value()) << enc.error().message;

    auto dec = mgr_->decrypt(*enc);
    ASSERT_TRUE(dec.has_value()) << dec.error().message;
    EXPECT_EQ(*dec, "");
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_EmptyPlaintextWrongKey_ReturnsError) {
    // Encrypting empty string with key1, decrypting with key2 must error.
    // Pre-fix: this returned ok("") which is indistinguishable from a legitimate
    // empty-string decrypt — the fix must make this an error.
    auto enc = mgr_->encrypt("");
    ASSERT_TRUE(enc.has_value());

    auto result = mgr2_->decrypt(*enc);
    ASSERT_FALSE(result.has_value())
        << "decrypt(empty) with wrong key must not return ok('') — got empty ok()";
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_EmptyPlaintextTamperedTag_ReturnsAuthError) {
    auto enc = mgr_->encrypt("");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    // Flip the last tag byte.
    raw.back() ^= 0xFF;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Large plaintext round-trip (1 MB)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_LargePlaintextRoundTrip_1MB) {
    std::string large(1024 * 1024, '\xAB');
    // Vary content to avoid compression-like patterns.
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<char>(i & 0xFF);
    }

    auto enc = mgr_->encrypt(large);
    ASSERT_TRUE(enc.has_value()) << enc.error().message;

    auto dec = mgr_->decrypt(*enc);
    ASSERT_TRUE(dec.has_value()) << dec.error().message;
    ASSERT_EQ(dec->size(), large.size());
    EXPECT_EQ(*dec, large);
}

// ---------------------------------------------------------------------------
// Binary plaintext with embedded NUL bytes — no truncation at NUL
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_BinaryPlaintextWithNulBytes_ExactRoundTrip) {
    // String with NUL bytes at start, middle, and end.
    std::string binary;
    binary.push_back('\0');
    binary += "normal-text";
    binary.push_back('\0');
    binary += "more-text";
    binary.push_back('\0');

    ASSERT_EQ(binary.size(), 23u);

    auto enc = mgr_->encrypt(binary);
    ASSERT_TRUE(enc.has_value()) << enc.error().message;

    auto dec = mgr_->decrypt(*enc);
    ASSERT_TRUE(dec.has_value()) << dec.error().message;
    ASSERT_EQ(dec->size(), binary.size()) << "NUL bytes must not truncate plaintext";
    EXPECT_EQ(*dec, binary);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_AllNulBytesPlaintext_ExactRoundTrip) {
    std::string all_nuls(64, '\0');

    auto enc = mgr_->encrypt(all_nuls);
    ASSERT_TRUE(enc.has_value()) << enc.error().message;

    auto dec = mgr_->decrypt(*enc);
    ASSERT_TRUE(dec.has_value()) << dec.error().message;
    ASSERT_EQ(dec->size(), 64u);
    EXPECT_EQ(*dec, all_nuls);
}

// ---------------------------------------------------------------------------
// Cross-message tag swap — valid IV+body from one message, tag from another
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_CrossMessageTagSwap_ReturnsAuthError) {
    auto enc_a = mgr_->encrypt("message-alpha");
    auto enc_b = mgr_->encrypt("message-beta-longer");
    ASSERT_TRUE(enc_a.has_value());
    ASSERT_TRUE(enc_b.has_value());

    auto raw_a = get_raw_bytes(*enc_a);
    auto raw_b = get_raw_bytes(*enc_b);
    ASSERT_GE(raw_a.size(), SecretsManager::kNonceSize + SecretsManager::kTagSize);
    ASSERT_GE(raw_b.size(), SecretsManager::kNonceSize + SecretsManager::kTagSize);

    // Take message A's nonce + ciphertext body, but attach message B's tag.
    std::vector<uint8_t> tampered(
        raw_a.begin(), raw_a.end() - static_cast<std::ptrdiff_t>(SecretsManager::kTagSize));
    // Append B's tag.
    tampered.insert(tampered.end(),
                    raw_b.end() - static_cast<std::ptrdiff_t>(SecretsManager::kTagSize),
                    raw_b.end());

    auto result = mgr_->decrypt(make_crafted_blob(tampered));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_CrossMessageNonceSwap_ReturnsAuthError) {
    auto enc_a = mgr_->encrypt("payload-one");
    auto enc_b = mgr_->encrypt("payload-two");
    ASSERT_TRUE(enc_a.has_value());
    ASSERT_TRUE(enc_b.has_value());

    auto raw_a = get_raw_bytes(*enc_a);
    auto raw_b = get_raw_bytes(*enc_b);

    // Use message A's nonce, but message B's body + tag.
    std::vector<uint8_t> tampered(
        raw_a.begin(), raw_a.begin() + static_cast<std::ptrdiff_t>(SecretsManager::kNonceSize));
    tampered.insert(tampered.end(),
                    raw_b.begin() + static_cast<std::ptrdiff_t>(SecretsManager::kNonceSize),
                    raw_b.end());

    auto result = mgr_->decrypt(make_crafted_blob(tampered));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// No partial plaintext exposed on failure
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_NoPartialPlaintextOnAuthFailure) {
    auto enc = mgr_->encrypt("top-secret-value-1234");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    // Corrupt tag only — body is intact so OpenSSL may partially decrypt it,
    // but the result must never be returned.
    size_t tag_start = raw.size() - SecretsManager::kTagSize;
    raw[tag_start] ^= 0xFF;
    raw[tag_start + 1] ^= 0xFF;

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    // Must fail — no partial plaintext.
    ASSERT_FALSE(result.has_value());
    // There should be no value to read — accessing it would be UB.
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_NoPartialPlaintextOnWrongKey) {
    auto enc = mgr_->encrypt("critical-data-must-not-leak");
    ASSERT_TRUE(enc.has_value());

    // Wrong key means EVP_DecryptFinal_ex fails; no plaintext must leak.
    auto result = mgr2_->decrypt(*enc);
    ASSERT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Regression: swallowed EVP_DecryptInit_ex failure returns INTERNAL_ERROR
// (not ok() with garbage). We trigger the init failure by providing a
// corrupted key via a specially-crafted manager backed by a key file of
// correct length but zeroed bytes. The OpenSSL init itself won't fail for
// a zero key (EVP accepts any 32-byte key), but if the nonce bytes are all
// zero AND the tag is garbage, we get AUTH_ERROR rather than silent success.
// The critical regression guard is the wrong-key test above.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_AllZeroKeyDecryptsWithWrongTagGivesAuthError) {
    // Write a 32-byte all-zeros key.
    std::string zero_key_path = (test_dir_ / "zero.key").string();
    {
        std::ofstream f(zero_key_path, std::ios::binary);
        std::array<char, 32> zeros{};
        f.write(zeros.data(), 32);
    }

    auto zero_mgr = SecretsManager::create(zero_key_path);
    ASSERT_TRUE(zero_mgr.has_value());

    // Encrypt with all-zeros key.
    auto enc = zero_mgr->encrypt("test-value");
    ASSERT_TRUE(enc.has_value());

    // Decrypt with a DIFFERENT all-zeros manager (should be same key — round-trip).
    auto zero_mgr2 = SecretsManager::create(zero_key_path);
    ASSERT_TRUE(zero_mgr2.has_value());
    auto dec = zero_mgr2->decrypt(*enc);
    ASSERT_TRUE(dec.has_value()) << dec.error().message;
    EXPECT_EQ(*dec, "test-value");

    // Now corrupt the tag and ensure we get AUTH_ERROR (not ok()).
    auto raw = get_raw_bytes(*enc);
    raw.back() ^= 0x01;
    auto result = zero_mgr2->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Exact StatusCode assertions for each error region
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_StatusCode_TooShort_IsINVALID_ARGUMENT) {
    // Raw bytes shorter than nonce+tag → INVALID_ARGUMENT from size check.
    std::vector<uint8_t> raw(10, 0x00);
    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_StatusCode_BadTag_IsAUTH_ERROR) {
    auto enc = mgr_->encrypt("hello");
    ASSERT_TRUE(enc.has_value());

    auto raw = get_raw_bytes(*enc);
    // Flip all tag bytes.
    for (size_t i = raw.size() - SecretsManager::kTagSize; i < raw.size(); ++i) {
        raw[i] = static_cast<uint8_t>(~raw[i]);
    }

    auto result = mgr_->decrypt(make_crafted_blob(raw));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_StatusCode_WrongKey_IsAUTH_ERROR) {
    auto enc = mgr_->encrypt("hello");
    ASSERT_TRUE(enc.has_value());

    auto result = mgr2_->decrypt(*enc);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
}

// ---------------------------------------------------------------------------
// Multiple round-trips with same manager (key reuse safety)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_MultipleRoundTripsExact) {
    const std::vector<std::string> secrets = {
        "",
        "a",
        "short",
        std::string(255, 'X'),
        std::string(256, 'Y'),
        "binary\x00\x01\x02\x03end",
        "special !@#$%^&*()_+-=[]{}|;':\",./<>?",
    };

    for (const auto& secret : secrets) {
        auto enc = mgr_->encrypt(secret);
        ASSERT_TRUE(enc.has_value()) << "encrypt failed for secret of size " << secret.size();
        auto dec = mgr_->decrypt(*enc);
        ASSERT_TRUE(dec.has_value()) << "decrypt failed for secret: " << secret.substr(0, 20);
        EXPECT_EQ(*dec, secret) << "round-trip mismatch for secret of size " << secret.size();
    }
}

// ---------------------------------------------------------------------------
// Mutation-grade regression: with the old code (no `if (!success)` guard),
// a decrypt of a ciphertext encrypted by a DIFFERENT key would fall through
// to `return ok(std::move(plaintext))` — returning ok("") silently.
// This test MUST fail when the guard is removed.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB846_SecretsManagerAdversarial,
       GDB846_Regression_WrongKeyMustNotReturnOkWithEmptyString) {
    // Encrypt a 10-byte plaintext with key1.
    auto enc = mgr_->encrypt("0123456789");
    ASSERT_TRUE(enc.has_value());

    // Decrypt with key2 — pre-fix would return ok("").
    auto result = mgr2_->decrypt(*enc);

    // Asserting BOTH: result must be an error AND must NOT be ok().
    EXPECT_FALSE(result.has_value())
        << "REGRESSION: decrypt() returned ok() with wrong key — pre-fix behavior detected. "
           "Value was: '"
        << (result.has_value() ? *result : "<error>") << "'";

    if (!result.has_value()) {
        // If it errored (correct), verify it's AUTH_ERROR specifically.
        EXPECT_EQ(result.error().code, StatusCode::AUTH_ERROR);
    }
}

TEST_F(QA_GDB846_SecretsManagerAdversarial, GDB846_Regression_EmptyPlaintextWrongKeyMustError) {
    // This is the most dangerous case pre-fix: encrypt("") with key1, decrypt
    // with key2 → pre-fix returns ok(""), which is IDENTICAL to a legitimate
    // decrypt of an empty secret. The fix must make this AUTH_ERROR.
    auto enc = mgr_->encrypt("");
    ASSERT_TRUE(enc.has_value());

    auto result = mgr2_->decrypt(*enc);
    EXPECT_FALSE(result.has_value())
        << "REGRESSION: decrypt(empty) with wrong key returned ok('') — "
           "indistinguishable from a real empty secret; pre-fix behavior";
}
