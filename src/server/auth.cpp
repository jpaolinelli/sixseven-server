#include "sixseven/server/auth.h"

#include "sixseven/common/logging.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

// Use CommonCrypto on macOS, OpenSSL-compatible API elsewhere.
#if defined(__APPLE__)
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonHMAC.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

namespace sixseven {

// -- Auth method parsing ------------------------------------------------------

Result<AuthMethod> parse_auth_method(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lower == "trust") {
        return ok(AuthMethod::TRUST);
    }
    if (lower == "md5") {
        return ok(AuthMethod::MD5);
    }
    if (lower == "scram-sha-256") {
        return ok(AuthMethod::SCRAM_SHA_256);
    }
    return make_error(StatusCode::INVALID_ARGUMENT,
                      "unknown auth method: " + name + " (valid: trust, md5, scram-sha-256)");
}

std::string auth_method_to_string(AuthMethod method) {
    switch (method) {
    case AuthMethod::TRUST:
        return "trust";
    case AuthMethod::MD5:
        return "md5";
    case AuthMethod::SCRAM_SHA_256:
        return "scram-sha-256";
    }
    return "trust";
}

// -- Cryptographic helpers ----------------------------------------------------

std::string md5_hex(const std::string& input) {
    unsigned char digest[16];
#if defined(__APPLE__)
    // Use CC_MD5 despite deprecation — MD5 is required by the PostgreSQL wire protocol
    // for MD5 authentication compatibility. This is not used for security-critical hashing.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5(input.data(), static_cast<CC_LONG>(input.size()), digest);
#pragma clang diagnostic pop
#else
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, nullptr);
    EVP_MD_CTX_free(ctx);
#endif

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned char byte : digest) {
        hex << std::setw(2) << static_cast<int>(byte);
    }
    return hex.str();
}

std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> digest(32);
#if defined(__APPLE__)
    CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), digest.data());
#else
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, digest.data(), &len);
    EVP_MD_CTX_free(ctx);
#endif
    return digest;
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key, const std::string& message) {
    std::vector<uint8_t> result(32);
#if defined(__APPLE__)
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), message.data(), message.size(), result.data());
#else
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         message.size(),
         result.data(),
         &len);
#endif
    return result;
}

std::vector<uint8_t> xor_bytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> result(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}

Result<std::vector<uint8_t>> random_bytes(size_t count) {
    std::vector<uint8_t> bytes(count);
    if (count == 0) {
        return ok(bytes);
    }
#if defined(__APPLE__)
    // SecRandomCopyBytes is the macOS CSPRNG; fall back via arc4random_buf.
    arc4random_buf(bytes.data(), count);
    return ok(bytes);
#else
    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
        return make_error(StatusCode::INTERNAL_ERROR, "CSPRNG failure: RAND_bytes returned != 1");
    }
    return ok(bytes);
#endif
}

namespace {

constexpr char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string result;
    size_t i = 0;
    uint32_t octets = 0;

    for (auto byte : data) {
        octets = (octets << 8) | byte;
        ++i;
        if (i % 3 == 0) {
            result += BASE64_CHARS[(octets >> 18) & 0x3F];
            result += BASE64_CHARS[(octets >> 12) & 0x3F];
            result += BASE64_CHARS[(octets >> 6) & 0x3F];
            result += BASE64_CHARS[octets & 0x3F];
            octets = 0;
        }
    }

    size_t remaining = i % 3;
    if (remaining == 1) {
        octets <<= 16;
        result += BASE64_CHARS[(octets >> 18) & 0x3F];
        result += BASE64_CHARS[(octets >> 12) & 0x3F];
        result += '=';
        result += '=';
    } else if (remaining == 2) {
        octets <<= 8;
        result += BASE64_CHARS[(octets >> 18) & 0x3F];
        result += BASE64_CHARS[(octets >> 12) & 0x3F];
        result += BASE64_CHARS[(octets >> 6) & 0x3F];
        result += '=';
    }

    return result;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
    // Build reverse lookup table.
    static const auto make_table = []() {
        std::array<uint8_t, 256> table{};
        table.fill(255);
        for (int i = 0; i < 64; ++i) {
            table[static_cast<uint8_t>(BASE64_CHARS[i])] = static_cast<uint8_t>(i);
        }
        return table;
    };
    static const auto table = make_table();

    std::vector<uint8_t> result;
    result.reserve(encoded.size() * 3 / 4);

    uint32_t accum = 0;
    int bits = 0;
    for (char c : encoded) {
        if (c == '=') {
            break;
        }
        uint8_t val = table[static_cast<uint8_t>(c)];
        if (val == 255) {
            continue; // Skip invalid chars.
        }
        accum = (accum << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }

    return result;
}

// -- PBKDF2-HMAC-SHA-256 (for SCRAM) -----------------------------------------

namespace {

/// PBKDF2 with HMAC-SHA-256 as the PRF.
std::vector<uint8_t> pbkdf2_hmac_sha256(const std::string& password,
                                        const std::vector<uint8_t>& salt,
                                        int32_t iterations) {
    // U_1 = HMAC(password, salt || INT(1))
    std::vector<uint8_t> key(password.begin(), password.end());

    // salt || INT32BE(1)
    std::string salt_str(salt.begin(), salt.end());
    salt_str += '\0';
    salt_str += '\0';
    salt_str += '\0';
    salt_str += '\1';

    auto u_prev = hmac_sha256(key, salt_str);
    auto result = u_prev;

    for (int32_t i = 1; i < iterations; ++i) {
        // U_i = HMAC(password, U_{i-1})
        std::string u_str(u_prev.begin(), u_prev.end());
        u_prev = hmac_sha256(key, u_str);
        result = xor_bytes(result, u_prev);
    }

    return result;
}

} // namespace

// -- Password hashing ---------------------------------------------------------

UserRecord hash_password_md5(const std::string& username, const std::string& password) {
    UserRecord record;
    record.username = username;
    // PostgreSQL MD5: md5(password + username)
    record.password_hash = "md5" + md5_hex(password + username);
    return record;
}

UserRecord hash_password_scram(const std::string& username, const std::string& password) {
    UserRecord record;
    record.username = username;
    record.iterations = 4096;

    // Generate random salt -- abort on CSPRNG failure; never use predictable bytes.
    auto salt_result = random_bytes(16);
    if (!salt_result) {
        SIXSEVEN_LOG_FATAL("CSPRNG failure during SCRAM salt generation: {}",
                           salt_result.error().message);
        std::abort();
    }
    auto salt = std::move(*salt_result);
    record.salt = base64_encode(salt);

    // Derive keys using PBKDF2.
    auto salted_password = pbkdf2_hmac_sha256(password, salt, record.iterations);
    auto client_key = hmac_sha256(salted_password, "Client Key");
    auto stored_key = sha256(client_key);
    auto server_key = hmac_sha256(salted_password, "Server Key");

    // Store as: SCRAM-SHA-256$iterations:salt$StoredKey:ServerKey (all base64).
    record.password_hash = "SCRAM-SHA-256$" + std::to_string(record.iterations) + ":" +
                           record.salt + "$" + base64_encode(stored_key) + ":" +
                           base64_encode(server_key);

    return record;
}

// -- UserManager --------------------------------------------------------------

namespace {

/// Build a UserRecord with the password hashed per the chosen auth method.
UserRecord
build_user_record(const std::string& username, const std::string& password, AuthMethod method) {
    UserRecord record;
    switch (method) {
    case AuthMethod::TRUST:
        record.username = username;
        break;
    case AuthMethod::MD5:
        record = hash_password_md5(username, password);
        break;
    case AuthMethod::SCRAM_SHA_256:
        record = hash_password_scram(username, password);
        break;
    }
    record.method = method;
    return record;
}

} // namespace

void UserManager::set_persistence(PersistUserFn on_upsert, RemoveUserFn on_remove) {
    std::lock_guard<std::mutex> lock(mu_);
    on_upsert_ = std::move(on_upsert);
    on_remove_ = std::move(on_remove);
}

void UserManager::load(std::vector<UserRecord> records) {
    std::lock_guard<std::mutex> lock(mu_);
    users_.clear();
    for (auto& record : records) {
        users_[record.username] = std::move(record);
    }
}

bool UserManager::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return users_.empty();
}

Result<void> UserManager::create_user(const std::string& username,
                                      const std::string& password,
                                      AuthMethod method) {
    std::unique_lock<std::mutex> lock(mu_);

    if (users_.contains(username)) {
        return make_error(StatusCode::ALREADY_EXISTS, "user already exists: " + username);
    }

    UserRecord record = build_user_record(username, password, method);
    users_[username] = record;

    // Write through to durable storage outside the lock (the hook touches the
    // buffer pool). Roll back the in-memory insert if persistence fails so disk
    // and memory never diverge.
    PersistUserFn upsert = on_upsert_;
    lock.unlock();
    if (upsert) {
        auto persisted = upsert(record);
        if (!persisted) {
            lock.lock();
            users_.erase(username);
            return persisted;
        }
    }
    return ok();
}

Result<void> UserManager::drop_user(const std::string& username, bool if_exists) {
    std::unique_lock<std::mutex> lock(mu_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        if (if_exists) {
            return ok();
        }
        return make_error(StatusCode::NOT_FOUND, "user does not exist: " + username);
    }

    UserRecord removed = it->second; // Saved for rollback.
    users_.erase(it);

    RemoveUserFn remove = on_remove_;
    lock.unlock();
    if (remove) {
        auto removed_ok = remove(username);
        if (!removed_ok) {
            lock.lock();
            users_[username] = std::move(removed);
            return removed_ok;
        }
    }
    return ok();
}

Result<void> UserManager::alter_user(const std::string& username,
                                     const std::string& new_password,
                                     AuthMethod method) {
    std::unique_lock<std::mutex> lock(mu_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        return make_error(StatusCode::NOT_FOUND, "user does not exist: " + username);
    }

    UserRecord previous = it->second; // Saved for rollback.
    UserRecord record = build_user_record(username, new_password, method);
    it->second = record;

    PersistUserFn upsert = on_upsert_;
    lock.unlock();
    if (upsert) {
        auto persisted = upsert(record);
        if (!persisted) {
            lock.lock();
            users_[username] = std::move(previous);
            return persisted;
        }
    }
    return ok();
}

std::optional<UserRecord> UserManager::get_user(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = users_.find(username);
    if (it == users_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool UserManager::user_exists(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mu_);
    return users_.count(username) > 0;
}

void UserManager::ensure_default_admin(AuthMethod method) {
    std::unique_lock<std::mutex> lock(mu_);
    if (users_.contains("demo")) {
        return;
    }

    UserRecord record = build_user_record("demo", "demo", method);
    users_["demo"] = record;
    SIXSEVEN_LOG_INFO("created default admin user 'demo'");

    // Persist the seeded admin. If persistence fails we keep the in-memory
    // record so the running server still has a working login; it will be
    // re-seeded (idempotently) on the next restart.
    PersistUserFn upsert = on_upsert_;
    lock.unlock();
    if (upsert) {
        auto persisted = upsert(record);
        if (!persisted) {
            SIXSEVEN_LOG_ERROR("failed to persist default admin user 'demo': {}",
                               persisted.error().message);
        }
    }
}

// -- MD5 verification ---------------------------------------------------------

bool verify_md5_password(const std::string& stored_hash,
                         const std::string& /*username*/,
                         const std::array<uint8_t, 4>& salt,
                         const std::string& client_response) {
    // stored_hash is "md5" + md5(password + username).
    // The inner hash (without "md5" prefix).
    if (stored_hash.size() < 3) {
        return false;
    }
    std::string inner_hash = stored_hash.substr(3);

    // The client computes: "md5" + md5(inner_hash + salt_hex).
    std::ostringstream salt_hex;
    salt_hex << std::hex << std::setfill('0');
    for (auto b : salt) {
        salt_hex << std::setw(2) << static_cast<int>(b);
    }

    std::string expected = "md5" + md5_hex(inner_hash + salt_hex.str());
    return expected == client_response;
}

// -- SCRAM-SHA-256 helpers ----------------------------------------------------

namespace {

/// Parse a SCRAM attribute from a comma-separated message.
/// Looks for "attr=value" and returns "value".
std::string scram_attr(const std::string& msg, char attr) {
    std::string prefix;
    prefix += attr;
    prefix += '=';

    size_t pos = 0;
    while (pos < msg.size()) {
        // Find next comma or start.
        if (pos == 0 || msg[pos - 1] == ',') {
            if (msg.compare(pos, prefix.size(), prefix) == 0) {
                size_t end = msg.find(',', pos + prefix.size());
                if (end == std::string::npos) {
                    end = msg.size();
                }
                return msg.substr(pos + prefix.size(), end - pos - prefix.size());
            }
        }
        ++pos;
    }
    return "";
}

/// Parse SCRAM stored key material from password_hash.
/// Format: SCRAM-SHA-256$iterations:salt$StoredKey:ServerKey
bool parse_scram_hash(const std::string& password_hash,
                      std::vector<uint8_t>& stored_key,
                      std::vector<uint8_t>& server_key) {
    // Find second '$'.
    auto first_dollar = password_hash.find('$');
    if (first_dollar == std::string::npos) {
        return false;
    }
    auto second_dollar = password_hash.find('$', first_dollar + 1);
    if (second_dollar == std::string::npos) {
        return false;
    }

    std::string keys_part = password_hash.substr(second_dollar + 1);
    auto colon = keys_part.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    stored_key = base64_decode(keys_part.substr(0, colon));
    server_key = base64_decode(keys_part.substr(colon + 1));
    return true;
}

} // namespace

Result<std::string> scram_server_first(const std::string& client_first_message,
                                       const UserRecord& user,
                                       ScramServerState& state) {
    // client-first-message = gs2-header, client-first-message-bare
    // gs2-header = "n,," (no channel binding, no authzid)
    // client-first-message-bare = "n=username,r=client-nonce"

    // Strip GS2 header.
    std::string bare;
    if (client_first_message.size() > 3 && client_first_message[0] == 'n' &&
        client_first_message[1] == ',') {
        // Skip past the second comma.
        auto pos = client_first_message.find(',', 2);
        if (pos != std::string::npos && pos + 1 < client_first_message.size()) {
            bare = client_first_message.substr(pos + 1);
        }
    }

    if (bare.empty()) {
        return make_error(StatusCode::AUTH_ERROR, "invalid SCRAM client-first-message");
    }

    state.client_first_bare = bare;

    // Extract client nonce.
    state.client_nonce = scram_attr(bare, 'r');
    if (state.client_nonce.empty()) {
        return make_error(StatusCode::AUTH_ERROR, "missing nonce in client-first-message");
    }

    // Generate server nonce and combine.
    auto server_nonce_result = random_bytes(18);
    if (!server_nonce_result) {
        return make_error(server_nonce_result.error().code, server_nonce_result.error().message);
    }
    std::string server_nonce_part = base64_encode(*server_nonce_result);
    state.server_nonce = state.client_nonce + server_nonce_part;

    // Get salt and iterations from user record.
    state.salt = base64_decode(user.salt);
    state.iterations = user.iterations;

    // Parse stored keys.
    if (!parse_scram_hash(user.password_hash, state.stored_key, state.server_key)) {
        return make_error(StatusCode::AUTH_ERROR,
                          "corrupted SCRAM credentials for user: " + user.username);
    }

    // Build server-first-message: r=combined-nonce,s=salt,i=iterations
    state.server_first =
        "r=" + state.server_nonce + ",s=" + user.salt + ",i=" + std::to_string(state.iterations);

    return ok(state.server_first);
}

Result<std::string> scram_server_final(const std::string& client_final_message,
                                       const ScramServerState& state) {
    // client-final-message = client-final-message-without-proof "," proof
    // client-final-message-without-proof = "c=biws,r=combined-nonce"
    // proof = "p=" base64(ClientProof)

    // Verify the combined nonce matches.
    std::string received_nonce = scram_attr(client_final_message, 'r');
    if (received_nonce != state.server_nonce) {
        return make_error(StatusCode::AUTH_ERROR, "SCRAM nonce mismatch");
    }

    // Extract proof.
    std::string proof_b64 = scram_attr(client_final_message, 'p');
    if (proof_b64.empty()) {
        return make_error(StatusCode::AUTH_ERROR, "missing proof in client-final-message");
    }
    auto client_proof = base64_decode(proof_b64);

    // Build AuthMessage = client-first-bare + "," + server-first + "," + client-final-without-proof
    // client-final-without-proof is everything before ",p=".
    auto proof_pos = client_final_message.find(",p=");
    if (proof_pos == std::string::npos) {
        return make_error(StatusCode::AUTH_ERROR, "malformed client-final-message");
    }
    std::string client_final_without_proof = client_final_message.substr(0, proof_pos);

    std::string auth_message =
        state.client_first_bare + "," + state.server_first + "," + client_final_without_proof;

    // ClientSignature = HMAC(StoredKey, AuthMessage)
    auto client_signature = hmac_sha256(state.stored_key, auth_message);

    // ClientKey = ClientProof XOR ClientSignature
    if (client_proof.size() != client_signature.size()) {
        return make_error(StatusCode::AUTH_ERROR, "SCRAM proof size mismatch");
    }
    auto client_key = xor_bytes(client_proof, client_signature);

    // StoredKey = H(ClientKey) — verify it matches.
    auto computed_stored_key = sha256(client_key);
    if (computed_stored_key != state.stored_key) {
        return make_error(StatusCode::AUTH_ERROR, "SCRAM authentication failed");
    }

    // ServerSignature = HMAC(ServerKey, AuthMessage)
    auto server_signature = hmac_sha256(state.server_key, auth_message);
    std::string server_final = "v=" + base64_encode(server_signature);

    return ok(server_final);
}

} // namespace sixseven
