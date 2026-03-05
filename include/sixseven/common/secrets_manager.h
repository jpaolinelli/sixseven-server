#pragma once

#include "sixseven/common/result.h"

#include <openssl/crypto.h>

#include <string>
#include <vector>

namespace sixseven {

/// Manages encryption/decryption of secrets using AES-256-GCM.
///
/// The SecretsManager loads (or auto-generates) a 256-bit master key from a
/// file on disk. Each encrypt() call generates a unique 12-byte nonce.
/// The stored format is: base64(nonce || ciphertext || auth_tag).
///
/// Decrypted secrets should be held in SecureString to ensure they are
/// zeroed from memory on deallocation.
class SecretsManager {
public:
    /// Key size in bytes (256 bits).
    static constexpr size_t kKeySize = 32;

    /// Nonce/IV size in bytes (96 bits for GCM).
    static constexpr size_t kNonceSize = 12;

    /// Authentication tag size in bytes (128 bits).
    static constexpr size_t kTagSize = 16;

    /// Create a SecretsManager from a master key file.
    ///
    /// If the file does not exist, a new random 256-bit key is generated
    /// and written to the file with mode 0600.
    /// If the file exists, the key is loaded from it.
    ///
    /// @param master_key_path  Path to the master key file.
    /// @return SecretsManager on success, or an error.
    [[nodiscard]] static Result<SecretsManager> create(const std::string& master_key_path);

    /// Encrypt a plaintext secret for storage.
    ///
    /// @param plaintext  The secret to encrypt.
    /// @return base64-encoded string: base64(nonce || ciphertext || auth_tag).
    [[nodiscard]] Result<std::string> encrypt(const std::string& plaintext) const;

    /// Decrypt a stored secret.
    ///
    /// @param encrypted  base64-encoded string from encrypt().
    /// @return The original plaintext secret.
    [[nodiscard]] Result<std::string> decrypt(const std::string& encrypted) const;

    /// Check whether a stored value looks like it was encrypted by this manager.
    ///
    /// Returns true if the value is valid base64 and has the minimum size for
    /// a nonce + auth_tag (even with empty plaintext). This is a heuristic —
    /// the definitive check is whether decrypt() succeeds.
    [[nodiscard]] static bool looks_encrypted(const std::string& value);

    ~SecretsManager() {
        if (!master_key_.empty()) {
            OPENSSL_cleanse(master_key_.data(), master_key_.size());
        }
    }

    SecretsManager(const SecretsManager&) = delete;
    SecretsManager& operator=(const SecretsManager&) = delete;

    SecretsManager(SecretsManager&& other) noexcept : master_key_(std::move(other.master_key_)) {
        // The moved-from vector may retain memory; zero it.
        if (!other.master_key_.empty()) {
            OPENSSL_cleanse(other.master_key_.data(), other.master_key_.size());
        }
        other.master_key_.clear();
    }

    SecretsManager& operator=(SecretsManager&& other) noexcept {
        if (this != &other) {
            if (!master_key_.empty()) {
                OPENSSL_cleanse(master_key_.data(), master_key_.size());
            }
            master_key_ = std::move(other.master_key_);
            if (!other.master_key_.empty()) {
                OPENSSL_cleanse(other.master_key_.data(), other.master_key_.size());
            }
            other.master_key_.clear();
        }
        return *this;
    }

private:
    SecretsManager() = default;

    /// The 256-bit master encryption key (zeroed on destruction).
    std::vector<uint8_t> master_key_;
};

} // namespace sixseven
