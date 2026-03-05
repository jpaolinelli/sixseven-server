#pragma once

#include <openssl/crypto.h>

#include <string>
#include <utility>

namespace sixseven {

/// A string wrapper that zeros its memory on destruction.
///
/// Used for holding decrypted secrets (API keys) in memory. When the
/// SecureString goes out of scope or is reassigned, the underlying
/// buffer is overwritten with zeros before deallocation, preventing
/// secrets from lingering in freed memory.
///
/// Uses OpenSSL's OPENSSL_cleanse() for guaranteed secure zeroing across
/// all platforms without compiler optimization issues.
class SecureString {
public:
    SecureString() = default;

    // NOLINTNEXTLINE(google-explicit-constructor)
    SecureString(std::string value) : data_(std::move(value)) {}

    // NOLINTNEXTLINE(google-explicit-constructor)
    SecureString(const char* value) : data_(value) {}

    ~SecureString() { clear(); }

    SecureString(const SecureString& other) : data_(other.data_) {}

    SecureString(SecureString&& other) noexcept : data_(std::move(other.data_)) {
        // The moved-from string may still hold memory; zero it.
        secure_zero(other.data_);
    }

    SecureString& operator=(const SecureString& other) {
        if (this != &other) {
            clear();
            data_ = other.data_;
        }
        return *this;
    }

    SecureString& operator=(SecureString&& other) noexcept {
        if (this != &other) {
            clear();
            data_ = std::move(other.data_);
            secure_zero(other.data_);
        }
        return *this;
    }

    SecureString& operator=(const std::string& value) {
        clear();
        data_ = value;
        return *this;
    }

    SecureString& operator=(std::string&& value) noexcept {
        clear();
        data_ = std::move(value);
        return *this;
    }

    /// Access the underlying string data.
    [[nodiscard]] const std::string& str() const { return data_; }

    /// Implicit conversion to const std::string& for convenience.
    // NOLINTNEXTLINE(google-explicit-constructor)
    operator const std::string&() const { return data_; }

    [[nodiscard]] const char* c_str() const { return data_.c_str(); }
    [[nodiscard]] size_t size() const { return data_.size(); }
    [[nodiscard]] bool empty() const { return data_.empty(); }

    bool operator==(const SecureString& other) const { return data_ == other.data_; }
    bool operator==(const std::string& other) const { return data_ == other; }
    bool operator==(const char* other) const { return data_ == other; }

    /// Zero the contents and release memory.
    void clear() {
        secure_zero(data_);
        data_.clear();
        data_.shrink_to_fit();
    }

private:
    static void secure_zero(std::string& s) {
        if (!s.empty()) {
            OPENSSL_cleanse(s.data(), s.size());
        }
    }

    std::string data_;
};

} // namespace sixseven
