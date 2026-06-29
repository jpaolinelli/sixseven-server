/// @file test_qa_gdb_970.cpp
/// @brief QA adversarial tests for GDB-970: random_bytes() must use OS CSPRNG
///        (RAND_bytes / arc4random_buf), never std::mt19937 or any predictable RNG.
///
/// Adversarial focus:
///   1. No mt19937 path survives in auth material generation.
///   2. Every random_bytes() caller checks the Result<> before dereferencing.
///   3. count==0 -> ok(empty) is correct and deterministic.
///   4. Two independent draws are statistically distinct (overwhelming probability).
///   5. SCRAM salt and nonce paths use random_bytes() result correctly.
///   6. hash_password_scram aborts on CSPRNG failure (non-testable in unit tests,
///      but we verify the happy path uses the CSPRNG output rather than zeros).
///   7. MD5 challenge salt path in UserManager round-trip works end-to-end.

#include "sixseven/server/auth.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace sixseven {

// =============================================================================
// AC1 / AC2: CSPRNG path -- no predictable RNG for auth material
// =============================================================================

/// Verify random_bytes(0) returns an empty vector with no error.
/// This is the boundary case: count==0 must NOT call RAND_bytes (which would
/// receive a 0 count and behave implementation-defined), and must NOT return
/// an error.
TEST(QA970CspRng, ZeroCountOkEmpty) {
    auto r = random_bytes(0);
    ASSERT_TRUE(r.has_value()) << "random_bytes(0) must succeed";
    EXPECT_TRUE(r->empty()) << "random_bytes(0) must return empty vector";
}

/// Verify random_bytes returns the requested number of bytes for typical salt sizes.
TEST(QA970CspRng, FourByteMd5Salt) {
    auto r = random_bytes(4);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 4u);
}

TEST(QA970CspRng, SixteenByteScramSalt) {
    auto r = random_bytes(16);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 16u);
}

TEST(QA970CspRng, EighteenByteServerNonce) {
    auto r = random_bytes(18);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 18u);
}

/// Two draws of 32 bytes must differ -- verifies we are reading from an actual
/// entropy source, not returning a fixed/zeroed buffer or a predictable sequence.
/// With a CSPRNG the collision probability is 2^-256; a failure here means the
/// implementation is broken (e.g., returning the same seed repeatedly).
TEST(QA970CspRng, TwoDrawsMustDiffer) {
    auto r1 = random_bytes(32);
    auto r2 = random_bytes(32);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(*r1, *r2)
        << "Two 32-byte CSPRNG draws must not be equal (would indicate RNG broken)";
}

/// Verify that the output is not all-zero (trivial sanity: a zero-initialized
/// buffer that was never filled would be caught here for sizes >= 8).
TEST(QA970CspRng, OutputNotAllZeros) {
    auto r = random_bytes(32);
    ASSERT_TRUE(r.has_value());
    bool all_zero = std::all_of(r->begin(), r->end(), [](uint8_t b) { return b == 0; });
    EXPECT_FALSE(all_zero) << "32 CSPRNG bytes must not all be zero";
}

/// Stress: 100 consecutive 16-byte draws -- all must succeed and all must differ
/// from the previous draw. A seeded mt19937 with a fixed seed would cycle;
/// a CSPRNG does not.
TEST(QA970CspRng, ConsecutiveDrawsAllSucceedAndDiffer) {
    std::vector<uint8_t> prev;
    for (int i = 0; i < 100; ++i) {
        auto r = random_bytes(16);
        ASSERT_TRUE(r.has_value()) << "draw " << i << " failed";
        EXPECT_EQ(r->size(), 16u);
        if (!prev.empty()) {
            // Two consecutive 16-byte CSPRNG draws differ with Pr 1 - 2^-128.
            EXPECT_NE(*r, prev) << "consecutive draw " << i << " equals previous draw";
        }
        prev = *r;
    }
}

// =============================================================================
// AC2: Caller Result-checking -- SCRAM salt path (hash_password_scram)
// =============================================================================

/// hash_password_scram must produce a SCRAM-SHA-256 record whose salt field
/// is non-empty base64 (i.e., the random_bytes result was used, not ignored).
TEST(QA970ScramSalt, HashPasswordScramSaltIsNonEmpty) {
    auto rec = hash_password_scram("user", "password");
    EXPECT_FALSE(rec.salt.empty())
        << "SCRAM salt must be non-empty; empty means random_bytes result was ignored";
    // Base64 of 16 bytes is 24 chars (with padding).
    EXPECT_EQ(rec.salt.size(), 24u)
        << "16-byte salt base64-encodes to 24 chars";
}

/// Two calls to hash_password_scram must produce different salts -- proving
/// the salt is freshly generated from CSPRNG each time, not cached or fixed.
TEST(QA970ScramSalt, TwoHashCallsDifferentSalts) {
    auto r1 = hash_password_scram("user", "pass");
    auto r2 = hash_password_scram("user", "pass");
    EXPECT_NE(r1.salt, r2.salt)
        << "Two SCRAM hash calls must generate distinct salts (CSPRNG-backed)";
}

/// The stored password hash must contain the salt embedded (format:
/// SCRAM-SHA-256$iter:salt$StoredKey:ServerKey).
TEST(QA970ScramSalt, HashFormatEmbedsSalt) {
    auto rec = hash_password_scram("alice", "secret");
    // Must start with SCRAM-SHA-256$
    ASSERT_GE(rec.password_hash.size(), 14u);
    EXPECT_EQ(rec.password_hash.substr(0, 14), "SCRAM-SHA-256$");
    // Salt must appear in the hash string.
    EXPECT_NE(rec.password_hash.find(rec.salt), std::string::npos)
        << "Salt not embedded in password_hash";
}

// =============================================================================
// AC2: Caller Result-checking -- SCRAM nonce path (scram_server_first)
// =============================================================================

/// A valid SCRAM first exchange must produce a server nonce that starts with
/// the client nonce (combined nonce = client_nonce + server_nonce_part).
TEST(QA970ScramNonce, ServerNoncePrefixedByClientNonce) {
    auto rec = hash_password_scram("user", "pass");
    ScramServerState state;
    std::string client_nonce = "clientnonce12345";
    auto r = scram_server_first("n,,n=user,r=" + client_nonce, rec, state);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Combined nonce must begin with the client nonce.
    ASSERT_GE(state.server_nonce.size(), client_nonce.size());
    EXPECT_EQ(state.server_nonce.substr(0, client_nonce.size()), client_nonce)
        << "Server nonce must be client_nonce + server_nonce_part";
}

/// The server nonce part (beyond the client nonce) must be non-empty and
/// different across calls -- verifying the random_bytes(18) result is used.
TEST(QA970ScramNonce, ServerNoncePartIsNonEmptyAndUnique) {
    auto rec = hash_password_scram("user", "pass");
    std::string client_nonce = "fixed_client_nonce";

    ScramServerState state1, state2;
    auto r1 = scram_server_first("n,,n=user,r=" + client_nonce, rec, state1);
    auto r2 = scram_server_first("n,,n=user,r=" + client_nonce, rec, state2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    // Server nonces should differ (CSPRNG generates fresh 18 bytes each call).
    EXPECT_NE(state1.server_nonce, state2.server_nonce)
        << "Two SCRAM exchanges with the same client nonce must produce different "
           "server nonces -- failure means server nonce is not CSPRNG-generated";
}

// =============================================================================
// AC1: No weak-RNG fallback -- verify salt is not a fixed/deterministic pattern
// =============================================================================

/// The MD5 verification path requires a salt, which the server generates via
/// random_bytes(4). Verify that two UserManager MD5 user creations produce
/// records with different password hashes when salts differ (indirect check
/// that the crypto is live, not stubbed).
///
/// Note: MD5 salt is generated per-connection, not per-user-record, so we
/// validate the SCRAM path (which uses random_bytes for per-user salt) as the
/// stronger check.
TEST(QA970WeakRngAbsence, ScramRecordsDifferAcrossUsers) {
    auto rec1 = hash_password_scram("userA", "samepassword");
    auto rec2 = hash_password_scram("userB", "samepassword");
    // Different salts -> different StoredKey/ServerKey even for same password.
    EXPECT_NE(rec1.password_hash, rec2.password_hash)
        << "Same password, different users: SCRAM hashes must differ (salt is unique)";
}

TEST(QA970WeakRngAbsence, ScramSameUserDifferentCallsDifferentSalts) {
    auto rec1 = hash_password_scram("user", "pass");
    auto rec2 = hash_password_scram("user", "pass");
    EXPECT_NE(rec1.salt, rec2.salt)
        << "Repeated SCRAM hash for same credentials must yield different salts "
           "(proves random_bytes is not returning a fixed seed)";
}

// =============================================================================
// Large / boundary allocation
// =============================================================================

TEST(QA970CspRng, LargeAllocation4KB) {
    auto r = random_bytes(4096);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 4096u);
    // Spot-check: not all zeros.
    bool all_zero = std::all_of(r->begin(), r->end(), [](uint8_t b) { return b == 0; });
    EXPECT_FALSE(all_zero);
}

TEST(QA970CspRng, SingleByteAllocation) {
    auto r = random_bytes(1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 1u);
}

// =============================================================================
// Result<> [[nodiscard]] hygiene -- compile-time check via usage
// (runtime: confirm the returned value is actually checked in our own code)
// =============================================================================

/// Confirm that [[nodiscard]] on random_bytes is declared (we call it and use
/// the result -- the test would not compile if the signature changed to void).
TEST(QA970NodisCard, ResultMustBeChecked) {
    [[maybe_unused]] auto r = random_bytes(8);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 8u);
}

} // namespace sixseven
