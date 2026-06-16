#include "sixseven/common/secrets_manager.h"

#include "sixseven/common/logging.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sixseven {

// ---------------------------------------------------------------------------
// Base64 encode/decode using OpenSSL EVP
// ---------------------------------------------------------------------------

namespace {

std::string base64_encode(const uint8_t* data, size_t len) {
    // Base64 output size: ceil(len/3)*4 + 1 for null terminator.
    size_t out_len = 4 * ((len + 2) / 3) + 1;
    std::string result(out_len, '\0');
    int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()), data, static_cast<int>(len));
    result.resize(static_cast<size_t>(written));
    return result;
}

Result<std::vector<uint8_t>> base64_decode(const std::string& encoded) {
    if (encoded.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "empty base64 input");
    }

    // Base64 decoded size is at most 3/4 of encoded size.
    std::vector<uint8_t> result(encoded.size());
    int decoded_len = EVP_DecodeBlock(result.data(),
                                      reinterpret_cast<const unsigned char*>(encoded.data()),
                                      static_cast<int>(encoded.size()));
    if (decoded_len < 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "invalid base64 encoding");
    }

    // EVP_DecodeBlock may include padding bytes; adjust for trailing '='.
    size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') {
        ++padding;
    }
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') {
        ++padding;
    }
    result.resize(static_cast<size_t>(decoded_len) - padding);
    return ok(std::move(result));
}

} // namespace

// ---------------------------------------------------------------------------
// SecretsManager
// ---------------------------------------------------------------------------

Result<SecretsManager> SecretsManager::create(const std::string& master_key_path) {
    SecretsManager mgr;

    if (std::filesystem::exists(master_key_path)) {
        // Load existing key.
        std::ifstream key_file(master_key_path, std::ios::binary);
        if (!key_file.is_open()) {
            return make_error(StatusCode::IO_ERROR,
                              "failed to open master key file: " + master_key_path);
        }

        mgr.master_key_.resize(kKeySize);
        key_file.read(reinterpret_cast<char*>(mgr.master_key_.data()),
                      static_cast<std::streamsize>(kKeySize));

        if (key_file.gcount() != static_cast<std::streamsize>(kKeySize)) {
            return make_error(StatusCode::IO_ERROR,
                              "master key file is invalid (expected " + std::to_string(kKeySize) +
                                  " bytes, got " + std::to_string(key_file.gcount()) + ")");
        }

        SIXSEVEN_LOG_INFO("secrets manager: loaded master key from {}", master_key_path);
    } else {
        // Generate a new random key.
        mgr.master_key_.resize(kKeySize);
        if (RAND_bytes(mgr.master_key_.data(), static_cast<int>(kKeySize)) != 1) {
            return make_error(StatusCode::INTERNAL_ERROR, "failed to generate random master key");
        }

        // Ensure parent directory exists.
        auto parent = std::filesystem::path(master_key_path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        // Write key file with restricted permissions.
        std::ofstream key_file(master_key_path, std::ios::binary | std::ios::trunc);
        if (!key_file.is_open()) {
            return make_error(StatusCode::IO_ERROR,
                              "failed to create master key file: " + master_key_path);
        }
        key_file.write(reinterpret_cast<const char*>(mgr.master_key_.data()),
                       static_cast<std::streamsize>(kKeySize));
        key_file.close();

        // Set file permissions to 0600 (owner read/write only).
        std::filesystem::permissions(master_key_path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);

        SIXSEVEN_LOG_INFO("secrets manager: generated new master key at {}", master_key_path);
    }

    return ok(std::move(mgr));
}

Result<std::string> SecretsManager::encrypt(const std::string& plaintext) const {
    // Generate a random nonce.
    std::array<uint8_t, kNonceSize> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(kNonceSize)) != 1) {
        return make_error(StatusCode::INTERNAL_ERROR, "failed to generate random nonce");
    }

    // Allocate output buffer: nonce + ciphertext + tag.
    std::vector<uint8_t> output(kNonceSize + plaintext.size() + kTagSize);

    // Copy nonce to output.
    std::memcpy(output.data(), nonce.data(), kNonceSize);

    // Encrypt using AES-256-GCM.
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "failed to create cipher context");
    }

    int len = 0;
    int ciphertext_len = 0;
    bool success = true;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        success = false;
    }

    if (success &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, master_key_.data(), nonce.data()) != 1) {
        success = false;
    }

    if (success && EVP_EncryptUpdate(ctx,
                                     output.data() + kNonceSize,
                                     &len,
                                     reinterpret_cast<const uint8_t*>(plaintext.data()),
                                     static_cast<int>(plaintext.size())) != 1) {
        success = false;
    }
    ciphertext_len = len;

    if (success &&
        EVP_EncryptFinal_ex(ctx, output.data() + kNonceSize + ciphertext_len, &len) != 1) {
        success = false;
    }
    ciphertext_len += len;

    // Get the authentication tag.
    if (success && EVP_CIPHER_CTX_ctrl(ctx,
                                       EVP_CTRL_GCM_GET_TAG,
                                       static_cast<int>(kTagSize),
                                       output.data() + kNonceSize +
                                           static_cast<size_t>(ciphertext_len)) != 1) {
        success = false;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!success) {
        return make_error(StatusCode::INTERNAL_ERROR, "AES-256-GCM encryption failed");
    }

    // Resize to actual size: nonce + ciphertext + tag.
    output.resize(kNonceSize + static_cast<size_t>(ciphertext_len) + kTagSize);

    return ok(base64_encode(output.data(), output.size()));
}

Result<std::string> SecretsManager::decrypt(const std::string& encrypted) const {
    // Base64-decode.
    auto decoded = base64_decode(encrypted);
    if (!decoded) {
        return make_error(decoded.error().code,
                          "failed to decode encrypted value: " + decoded.error().message);
    }

    auto& data = *decoded;

    // Minimum size: nonce + tag (no ciphertext for empty plaintext).
    if (data.size() < kNonceSize + kTagSize) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "encrypted data too short (need at least " +
                              std::to_string(kNonceSize + kTagSize) + " bytes, got " +
                              std::to_string(data.size()) + ")");
    }

    // Extract nonce, ciphertext, and tag.
    const uint8_t* nonce = data.data();
    const uint8_t* ciphertext = data.data() + kNonceSize;
    size_t ciphertext_len = data.size() - kNonceSize - kTagSize;
    const uint8_t* tag = data.data() + kNonceSize + ciphertext_len;

    // Decrypt using AES-256-GCM.
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "failed to create cipher context");
    }

    std::string plaintext(ciphertext_len, '\0');
    int len = 0;
    int plaintext_len = 0;
    bool success = true;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        success = false;
    }

    if (success && EVP_DecryptInit_ex(ctx, nullptr, nullptr, master_key_.data(), nonce) != 1) {
        success = false;
    }

    if (success && ciphertext_len > 0 &&
        EVP_DecryptUpdate(ctx,
                          reinterpret_cast<uint8_t*>(plaintext.data()),
                          &len,
                          ciphertext,
                          static_cast<int>(ciphertext_len)) != 1) {
        success = false;
    }
    plaintext_len = len;

    // Set the expected tag before finalization.
    if (success && EVP_CIPHER_CTX_ctrl(ctx,
                                       EVP_CTRL_GCM_SET_TAG,
                                       static_cast<int>(kTagSize),
                                       const_cast<uint8_t*>(tag)) != 1) {
        success = false;
    }

    // Finalize — this verifies the authentication tag.
    if (success && EVP_DecryptFinal_ex(ctx,
                                       reinterpret_cast<uint8_t*>(plaintext.data()) + plaintext_len,
                                       &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return make_error(StatusCode::AUTH_ERROR,
                          "AES-256-GCM decryption failed (authentication tag mismatch)");
    }
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);

    if (!success) {
        return make_error(StatusCode::INTERNAL_ERROR, "AES-256-GCM decryption failed");
    }

    plaintext.resize(static_cast<size_t>(plaintext_len));
    return ok(std::move(plaintext));
}

bool SecretsManager::looks_encrypted(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    // Minimum base64 size for nonce(12) + tag(16) = 28 bytes → 40 base64 chars.
    // With ciphertext it will be larger. Use 36 as minimum (accounting for padding).
    if (value.size() < 36) {
        return false;
    }

    // Check that all characters are valid base64.
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '+' || c == '/' || c == '=';
    });
}

} // namespace sixseven
