// QA regression tests for GDB-903
// Ticket: MD5 password hashing/verification tests are self-referential;
//         no known-answer vector pins the PostgreSQL md5(password+username) convention.
//
// Fix: Added two known-answer tests to tests/unit/test_auth.cpp:
//   - CryptoHelpers.Md5HexKnownAnswerHello
//   - PasswordHashing.Md5HashKnownAnswerPostgresWireOrder
//
// QA focus:
//   1. Independently confirm the pinned MD5 hex values are correct.
//   2. Confirm the wrong-order value (md5(username+password)) is detectably
//      different from the correct-order value, so the test is mutation-grade.
//   3. Confirm Md5Verification challenge-response is not trivially self-referential
//      by verifying a wrong stored-hash rejects a correct client response.
//   4. Confirm no production code was regressed (hash still matches pinned value).

#include "sixseven/server/auth.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

using namespace sixseven;

// =============================================================================
// GDB903 — Known-answer mutation-grade checks
// =============================================================================

// Independently computable: md5("hello") = 5d41402abc4b2a76b9719d911017c592
// Verifiable via: echo -n hello | md5sum
// This test would FAIL if md5_hex produced garbage or used a different algorithm.
TEST(QA_GDB903_CryptoHelpers, Md5HexKnownAnswerHelloPins) {
    EXPECT_EQ(md5_hex("hello"), "5d41402abc4b2a76b9719d911017c592");
}

// Empty string MD5: md5("") = d41d8cd98f00b204e9800998ecf8427e (RFC 1321 test vector)
// Independently verifiable: echo -n "" | md5sum
TEST(QA_GDB903_CryptoHelpers, Md5HexEmptyStringRfc1321Vector) {
    EXPECT_EQ(md5_hex(""), "d41d8cd98f00b204e9800998ecf8427e");
}

// "abc" MD5: md5("abc") = 900150983cd24fb0d6963f7d28e17f72 (RFC 1321 test vector)
TEST(QA_GDB903_CryptoHelpers, Md5HexAbcRfc1321Vector) {
    EXPECT_EQ(md5_hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
}

// PostgreSQL wire order: md5(password + username).
// Pinned value for ("alice", "secret") = "md5" + md5("secretalice")
//   md5("secretalice") = 4a0a68b43b6cd5cf266fa02f196e2371
// Independently verifiable: echo -n secretalice | md5sum
TEST(QA_GDB903_PasswordHashing, WireOrderKnownAnswerAliceSecret) {
    auto record = hash_password_md5("alice", "secret");
    EXPECT_EQ(record.password_hash, "md54a0a68b43b6cd5cf266fa02f196e2371");
}

// Mutation check: wrong order md5(username + password) = md5("alicesecret")
//   = c4e31313222cf05fcdd1fc068af5570e  (distinct from correct value)
// If auth.cpp:253 were flipped to md5(username+password), the pinned test would
// produce this value instead.  Assert it is NOT equal to the stored hash,
// confirming the two values are distinguishable (i.e., the known-answer test has
// actual detection power).
TEST(QA_GDB903_PasswordHashing, WrongOrderDetectablyDifferent) {
    auto record = hash_password_md5("alice", "secret");
    // Correct order: md5("secretalice") pinned above.
    // Wrong order: md5("alicesecret") = c4e31313222cf05fcdd1fc068af5570e
    std::string wrong_order_hash = "md5c4e31313222cf05fcdd1fc068af5570e";
    EXPECT_NE(record.password_hash, wrong_order_hash)
        << "CRITICAL: production code is using the wrong concatenation order "
           "(username+password) instead of (password+username). "
           "This would break all real psql/libpq clients.";
}

// A different user/password pair also produces the expected known-answer.
// Verifiable: echo -n passwordbob | md5sum
// md5("passwordbob") = 90703014bfcfb5a29afaddb29e5d4cc0
TEST(QA_GDB903_PasswordHashing, WireOrderKnownAnswerBobPassword) {
    auto record = hash_password_md5("bob", "password");
    EXPECT_EQ(record.password_hash, "md590703014bfcfb5a29afaddb29e5d4cc0");
}

// =============================================================================
// GDB903 — Md5Verification challenge-response is not trivially self-referential
// =============================================================================

// verify_md5_password must reject a client response built from a DIFFERENT
// stored hash (i.e., wrong password), even though both the stored hash and the
// client response are internally consistent.  This tests that verify_md5_password
// actually compares to the *provided* stored_hash, not just checks well-formedness.
TEST(QA_GDB903_Md5Verification, WrongStoredHashRejectsCorrectClientResponse) {
    // Build stored hash for "alice"/"correct_password"
    auto correct_record = hash_password_md5("alice", "correct_password");
    // Build stored hash for "alice"/"wrong_password"
    auto wrong_record = hash_password_md5("alice", "wrong_password");

    std::array<uint8_t, 4> salt = {0xAA, 0xBB, 0xCC, 0xDD};

    // Build a syntactically-valid client response for wrong_password.
    std::string inner_hash = wrong_record.password_hash.substr(3);
    std::string salt_hex;
    for (auto b : salt) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        salt_hex += hex;
    }
    std::string client_response = "md5" + md5_hex(inner_hash + salt_hex);

    // Verifying against correct_record's stored hash must FAIL.
    EXPECT_FALSE(
        verify_md5_password(correct_record.password_hash, "alice", salt, client_response))
        << "verify_md5_password accepted a response built from the wrong password hash";
}

// Malformed client response (wrong prefix) must be rejected cleanly.
TEST(QA_GDB903_Md5Verification, MalformedResponseMissingMd5Prefix) {
    auto record = hash_password_md5("alice", "secret");
    std::array<uint8_t, 4> salt = {0x01, 0x02, 0x03, 0x04};
    // No "md5" prefix — just raw hex.
    std::string bare_hex = md5_hex("secretalice");
    EXPECT_FALSE(verify_md5_password(record.password_hash, "alice", salt, bare_hex));
}

// Empty client response must not crash and must be rejected.
TEST(QA_GDB903_Md5Verification, EmptyResponseRejected) {
    auto record = hash_password_md5("alice", "secret");
    std::array<uint8_t, 4> salt = {0x01, 0x02, 0x03, 0x04};
    EXPECT_FALSE(verify_md5_password(record.password_hash, "alice", salt, ""));
}

// Truncated client response (only "md5") must not crash.
TEST(QA_GDB903_Md5Verification, TruncatedResponseRejected) {
    auto record = hash_password_md5("alice", "secret");
    std::array<uint8_t, 4> salt = {0x01, 0x02, 0x03, 0x04};
    EXPECT_FALSE(verify_md5_password(record.password_hash, "alice", salt, "md5"));
}
