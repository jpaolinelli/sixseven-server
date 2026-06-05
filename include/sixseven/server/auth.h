#pragma once

#include "sixseven/common/result.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

// -- Authentication method enum -----------------------------------------------

/// Supported authentication methods.
enum class AuthMethod : uint8_t {
    TRUST,
    MD5,
    SCRAM_SHA_256,
};

/// Parse an auth method name string (e.g., "trust", "md5", "scram-sha-256").
Result<AuthMethod> parse_auth_method(const std::string& name);

/// Serialize an auth method to its canonical name ("trust"/"md5"/"scram-sha-256").
/// Inverse of parse_auth_method; used when persisting user records.
std::string auth_method_to_string(AuthMethod method);

// -- Cryptographic helpers ----------------------------------------------------

/// Compute MD5 hash of input data, returning a 32-character hex string.
std::string md5_hex(const std::string& input);

/// Compute HMAC-SHA-256 of message using the given key.
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key, const std::string& message);

/// Compute SHA-256 hash of input data.
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

/// XOR two equal-length byte arrays.
std::vector<uint8_t> xor_bytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

/// Generate cryptographically random bytes.
std::vector<uint8_t> random_bytes(size_t count);

/// Base64-encode a byte array.
std::string base64_encode(const std::vector<uint8_t>& data);

/// Base64-decode a string.
std::vector<uint8_t> base64_decode(const std::string& encoded);

// -- User record --------------------------------------------------------------

/// Stored user credentials.
struct UserRecord {
    std::string username;
    std::string password_hash;             ///< MD5: "md5" + hex. SCRAM: stored key material.
    std::string salt;                      ///< Base64-encoded salt (for SCRAM).
    int32_t iterations = 4096;             ///< PBKDF2 iteration count (for SCRAM).
    AuthMethod method = AuthMethod::TRUST; ///< Method used to hash this record.
};

// -- User manager -------------------------------------------------------------

/// Thread-safe in-memory user credential store.
///
/// Credentials live in memory for fast auth lookups. When persistence hooks are
/// installed via set_persistence(), every mutating operation also writes through
/// to durable storage (the sys_users table) so users survive restart. At startup
/// load() repopulates the in-memory map from persisted records.
class UserManager {
public:
    /// Called to durably upsert a user record (e.g., write to sys_users).
    using PersistUserFn = std::function<Result<void>(const UserRecord&)>;
    /// Called to durably remove a user by name (e.g., delete from sys_users).
    using RemoveUserFn = std::function<Result<void>(const std::string&)>;

    UserManager() = default;

    /// Install write-through persistence hooks. When set, create/alter/drop and
    /// ensure_default_admin synchronize their changes to durable storage. Without
    /// hooks the manager is purely in-memory (the prior behavior).
    void set_persistence(PersistUserFn on_upsert, RemoveUserFn on_remove);

    /// Bulk-load persisted user records into the in-memory map (startup only).
    /// Does NOT fire persistence hooks — the records already exist on disk.
    void load(std::vector<UserRecord> records);

    /// True when no users are registered.
    [[nodiscard]] bool empty() const;

    /// Create a user with the given plain-text password.
    /// Stores the password hash appropriate for the given auth method.
    Result<void>
    create_user(const std::string& username, const std::string& password, AuthMethod method);

    /// Drop a user. Returns error if the user does not exist and if_exists is false.
    Result<void> drop_user(const std::string& username, bool if_exists);

    /// Alter a user's password.
    Result<void>
    alter_user(const std::string& username, const std::string& new_password, AuthMethod method);

    /// Look up a user record. Returns nullopt if not found.
    std::optional<UserRecord> get_user(const std::string& username) const;

    /// Check if a user exists.
    bool user_exists(const std::string& username) const;

    /// Ensure default admin user exists (username: "sixseven", password: "sixseven").
    void ensure_default_admin(AuthMethod method);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, UserRecord> users_;
    PersistUserFn on_upsert_;
    RemoveUserFn on_remove_;
};

// -- SCRAM-SHA-256 state machine ----------------------------------------------

/// Server-side state for a SCRAM-SHA-256 authentication exchange.
struct ScramServerState {
    std::string client_first_bare;   ///< Client-first-message-bare.
    std::string server_first;        ///< Server-first-message.
    std::string client_nonce;        ///< Nonce from client.
    std::string server_nonce;        ///< Combined nonce.
    std::vector<uint8_t> salt;       ///< User's salt.
    int32_t iterations = 4096;       ///< PBKDF2 iteration count.
    std::vector<uint8_t> stored_key; ///< StoredKey from user record.
    std::vector<uint8_t> server_key; ///< ServerKey from user record.
};

// -- Authentication protocol functions ----------------------------------------

/// Verify an MD5 password response from the client.
/// Expected: "md5" + md5(md5(password + username) + salt_hex).
/// @param stored_hash  The stored MD5 hash: "md5" + md5(password + username).
/// @param username     The username.
/// @param salt         The 4-byte salt sent to the client.
/// @param client_response The client's PasswordMessage payload.
bool verify_md5_password(const std::string& stored_hash,
                         const std::string& username,
                         const std::array<uint8_t, 4>& salt,
                         const std::string& client_response);

/// Generate a SCRAM-SHA-256 server-first-message given the client-first-message.
/// Initializes the ScramServerState for use in the next step.
Result<std::string> scram_server_first(const std::string& client_first_message,
                                       const UserRecord& user,
                                       ScramServerState& state);

/// Verify the SCRAM-SHA-256 client-final-message and produce the server-final-message.
Result<std::string> scram_server_final(const std::string& client_final_message,
                                       const ScramServerState& state);

/// Hash a password for storage using SCRAM-SHA-256.
/// Returns a UserRecord with stored_key and server_key encoded in password_hash,
/// along with the salt and iteration count.
UserRecord hash_password_scram(const std::string& username, const std::string& password);

/// Hash a password for storage using MD5.
/// Returns a UserRecord with the MD5 hash in password_hash.
UserRecord hash_password_md5(const std::string& username, const std::string& password);

} // namespace sixseven
