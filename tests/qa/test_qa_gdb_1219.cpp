// QA regression tests for GDB-1219.
//
// GDB-1219 relocated the byte-identical `starts_with_ci` helper out of the
// anonymous namespaces in src/server/session.cpp and src/server/pg_protocol.cpp
// into a single shared definition in include/sixseven/common/string_util.h.
// Both files now include the shared header instead of each defining their own
// copy.
//
// This suite is adversarial verification that:
//   1. The end-to-end SQL-command dispatch in Session::try_handle_command
//      (the real consumer in session.cpp) still routes mixed-case, exact,
//      and boundary-case commands identically to before the relocation.
//   2. starts_with_ci itself tolerates high-bit / non-ASCII bytes without UB
//      or crashes (unsigned-char tolower contract preserved).
//   3. No coverage was lost relative to the two removed inline copies -- in
//      particular empty-prefix, prefix-longer-than-input, and exact-match
//      cases that both original copies implicitly relied on for dispatch.

#include "sixseven/common/string_util.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

#include <string>

using namespace sixseven;

// =============================================================================
// End-to-end dispatch through Session::try_handle_command (session.cpp
// consumer of the relocated starts_with_ci).
// =============================================================================

TEST(QA_GDB1219_SessionDispatch, SetCommandUpperCase) {
    Session session(1);
    auto result = session.try_handle_command("SET work_mem = '8MB'");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(*session.get_variable("work_mem"), "8MB");
}

TEST(QA_GDB1219_SessionDispatch, SetCommandLowerCase) {
    Session session(1);
    auto result = session.try_handle_command("set work_mem = '8MB'");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(*session.get_variable("work_mem"), "8MB");
}

TEST(QA_GDB1219_SessionDispatch, SetCommandMixedCase) {
    Session session(1);
    auto result = session.try_handle_command("SeT work_mem = '8MB'");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(*session.get_variable("work_mem"), "8MB");
}

TEST(QA_GDB1219_SessionDispatch, ShowCommandMixedCase) {
    Session session(1);
    auto result = session.try_handle_command("ShOw work_mem");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
}

TEST(QA_GDB1219_SessionDispatch, ResetCommandMixedCase) {
    Session session(1);
    session.set_variable("work_mem", "8MB");
    auto result = session.try_handle_command("reSet work_mem");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
    EXPECT_EQ(*session.get_variable("work_mem"), "4MB");
}

TEST(QA_GDB1219_SessionDispatch, SavepointCommandMixedCaseRoutesToHandler) {
    // SAVEPOINT is intentionally unsupported (subtransaction rollback isn't
    // implemented), so the handler returns an error -- but the important
    // thing for this ticket is that starts_with_ci still correctly routes
    // the mixed-case command to try_handle_savepoint at all (i.e. does not
    // fall through to nullopt). A regression in the relocated prefix check
    // would make this return std::nullopt instead of an error Result.
    Session session(1);
    auto result = session.try_handle_command("SaVePoInT sp1");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
    EXPECT_NE(result->error().message.find("SAVEPOINT"), std::string::npos);
}

TEST(QA_GDB1219_SessionDispatch, PrepareCommandMixedCase) {
    Session session(1);
    auto result = session.try_handle_command("prepare stmt1 as select 1");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
}

TEST(QA_GDB1219_SessionDispatch, DeallocateCommandMixedCase) {
    Session session(1);
    session.try_handle_command("PREPARE stmt1 AS SELECT 1");
    auto result = session.try_handle_command("DeAlLoCaTe stmt1");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value()) << result->error().message;
}

// Non-matching prefixes (e.g. SELECT/BEGIN/COMMIT) must NOT be intercepted by
// the session-level dispatcher -- they fall through to nullopt so the query
// executor handles them. This proves the relocation didn't accidentally
// widen or narrow the matched-prefix set.
TEST(QA_GDB1219_SessionDispatch, SelectDoesNotMatchAnySessionPrefix) {
    Session session(1);
    auto result = session.try_handle_command("SELECT 1");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB1219_SessionDispatch, SelectLowerCaseDoesNotMatchAnySessionPrefix) {
    Session session(1);
    auto result = session.try_handle_command("select 1");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB1219_SessionDispatch, BeginDoesNotMatchAnySessionPrefix) {
    Session session(1);
    auto result = session.try_handle_command("BEGIN");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB1219_SessionDispatch, CommitDoesNotMatchAnySessionPrefix) {
    Session session(1);
    auto result = session.try_handle_command("COMMIT");
    EXPECT_FALSE(result.has_value());
}

// A prefix-only match (no trailing content) must not falsely dispatch, e.g.
// "SET" without a trailing space/argument should not be treated as a SET
// command by the "SET " (with trailing space) prefix check.
TEST(QA_GDB1219_SessionDispatch, BareSetKeywordWithoutSpaceDoesNotDispatch) {
    Session session(1);
    auto result = session.try_handle_command("SET");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB1219_SessionDispatch, BareShowKeywordWithoutSpaceDoesNotDispatch) {
    Session session(1);
    auto result = session.try_handle_command("SHOW");
    EXPECT_FALSE(result.has_value());
}

// Input shorter than the matched keyword's prefix must not crash and must
// not match.
TEST(QA_GDB1219_SessionDispatch, VeryShortInputDoesNotMatchAndDoesNotCrash) {
    Session session(1);
    auto result = session.try_handle_command("SE");
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB1219_SessionDispatch, SingleCharacterInputDoesNotCrash) {
    Session session(1);
    auto result = session.try_handle_command("S");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Direct adversarial tests on the shared starts_with_ci (used identically by
// both session.cpp and pg_protocol.cpp call sites).
// =============================================================================

TEST(QA_GDB1219_StartsWithCi, MixedCaseBeginCommitSelectStillMatchTheirOwnPrefix) {
    // Simulate the style of check both original call sites performed for
    // command-routing keywords, across all-case permutations.
    EXPECT_TRUE(starts_with_ci("BEGIN", "BEGIN"));
    EXPECT_TRUE(starts_with_ci("begin", "BEGIN"));
    EXPECT_TRUE(starts_with_ci("Begin", "BEGIN"));
    EXPECT_TRUE(starts_with_ci("bEgIn", "BEGIN"));

    EXPECT_TRUE(starts_with_ci("COMMIT", "COMMIT"));
    EXPECT_TRUE(starts_with_ci("commit", "COMMIT"));
    EXPECT_TRUE(starts_with_ci("CoMmIt", "COMMIT"));

    EXPECT_TRUE(starts_with_ci("SELECT * FROM t", "SELECT "));
    EXPECT_TRUE(starts_with_ci("select * from t", "SELECT "));
    EXPECT_TRUE(starts_with_ci("SeLeCt * from t", "SELECT "));
}

TEST(QA_GDB1219_StartsWithCi, ExecutePrefixCaseVariants) {
    // pg_protocol.cpp's try_handle_execute consumer relies on exactly this.
    EXPECT_TRUE(starts_with_ci("EXECUTE stmt1", "EXECUTE "));
    EXPECT_TRUE(starts_with_ci("execute stmt1", "EXECUTE "));
    EXPECT_TRUE(starts_with_ci("ExEcUtE stmt1", "EXECUTE "));
    EXPECT_FALSE(starts_with_ci("EXEC stmt1", "EXECUTE "));
    EXPECT_FALSE(starts_with_ci("EXECUT stmt1", "EXECUTE "));
}

TEST(QA_GDB1219_StartsWithCi, EmptyPrefixAlwaysMatchesRegardlessOfInput) {
    EXPECT_TRUE(starts_with_ci("anything at all", ""));
    EXPECT_TRUE(starts_with_ci("", ""));
    EXPECT_TRUE(starts_with_ci("SELECT", ""));
}

TEST(QA_GDB1219_StartsWithCi, EmptyInputWithNonEmptyPrefixNeverMatches) {
    EXPECT_FALSE(starts_with_ci("", "SELECT"));
    EXPECT_FALSE(starts_with_ci("", "S"));
}

TEST(QA_GDB1219_StartsWithCi, PrefixLongerThanInputNeverMatches) {
    EXPECT_FALSE(starts_with_ci("SEL", "SELECT "));
    EXPECT_FALSE(starts_with_ci("EXEC", "EXECUTE "));
}

TEST(QA_GDB1219_StartsWithCi, ExactLengthMatchIsInclusive) {
    // str.size() == prefix.size() must still be checked (not silently
    // rejected by an off-by-one in the relocated bounds check).
    EXPECT_TRUE(starts_with_ci("SELECT", "SELECT"));
    EXPECT_TRUE(starts_with_ci("select", "SELECT"));
    EXPECT_FALSE(starts_with_ci("SELECX", "SELECT"));
}

TEST(QA_GDB1219_StartsWithCi, HighBitNonAsciiBytesDoNotCrashOrInvokeUb) {
    // Bytes 0x80-0xFF passed through static_cast<unsigned char> before
    // std::tolower must not trigger UB (a naive `char` cast on a platform
    // where char is signed would be UB for negative values passed to
    // std::tolower). Verify survivability and that identical high-bit byte
    // sequences compare equal.
    std::string high_bytes;
    for (int c = 128; c < 256; ++c) {
        high_bytes.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    EXPECT_TRUE(starts_with_ci(high_bytes, high_bytes));

    std::string prefix_only_first_half(high_bytes.substr(0, 64));
    EXPECT_TRUE(starts_with_ci(high_bytes, prefix_only_first_half));
}

TEST(QA_GDB1219_StartsWithCi, HighBitBytesMixedWithAsciiCommandPrefix) {
    // A malformed/binary-garbage "SQL" string containing high-bit bytes
    // after a valid-looking ASCII command keyword must not crash the
    // dispatcher and must still correctly evaluate the prefix check.
    std::string sql = "SELECT ";
    sql.push_back(static_cast<char>(0xFF));
    sql.push_back(static_cast<char>(0x80));
    EXPECT_TRUE(starts_with_ci(sql, "SELECT "));
    EXPECT_TRUE(starts_with_ci(sql, "select "));
}

TEST(QA_GDB1219_StartsWithCi, NoMatchIsCorrectlyRejected) {
    EXPECT_FALSE(starts_with_ci("INSERT INTO t", "SELECT "));
    EXPECT_FALSE(starts_with_ci("DELETE FROM t", "SELECT "));
}

TEST(QA_GDB1219_StartsWithCi, WhitespaceIsNotCollapsedOrTrimmedByStartsWithCi) {
    // starts_with_ci is a pure byte-wise prefix check; it must not perform
    // any implicit trimming. A leading space before the keyword must NOT
    // match the keyword's prefix (trimming, if needed, is the caller's job
    // via a separate trim() call, and pg_protocol.cpp/session.cpp both
    // trim before dispatch -- but starts_with_ci itself must not).
    EXPECT_FALSE(starts_with_ci(" SELECT 1", "SELECT "));
}

// =============================================================================
// Shared-header identity: verify both consumers observe the exact same
// symbol (single definition, not two independently-compiled copies that
// could silently diverge again in the future).
// =============================================================================

TEST(QA_GDB1219_SharedDefinition, FunctionPointerIdentityAcrossTranslationUnits) {
    // Both session.cpp and pg_protocol.cpp now `#include
    // "sixseven/common/string_util.h"` rather than defining their own
    // anonymous-namespace copy. Since the function is `inline` in a shared
    // header, ODR guarantees a single definition. This test doesn't (and
    // can't, from a single TU) directly compare addresses across TUs, but it
    // does confirm the symbol used in this TU behaves per the documented
    // contract at every call site's actual usage pattern.
    using FnPtr = bool (*)(std::string_view, std::string_view);
    FnPtr fn = &starts_with_ci;
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn("SET x", "SET "));
}
