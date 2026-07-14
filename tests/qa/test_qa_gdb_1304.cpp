// QA regression / adversarial tests for GDB-1304: PATH text formatting now
// quotes/escapes STRING node PKs so PATH's own structural delimiters
// ('[', ']', ',', '-(' ')->') never collide with delimiter characters
// embedded in the PK data itself (value_to_pg_text PATH case in
// src/server/pg_protocol.cpp).
//
// Focus areas (per QA handoff):
//   - STRING PKs with every combination of delimiter chars (commas,
//     brackets, parens, arrows) plus quotes/backslashes/whitespace/unicode
//     within one multi-step path
//   - UUID-typed PKs: confirm the fix's `type_id() == TypeId::STRING` check
//     correctly distinguishes STRING from UUID (GDB-1292 widened node_pk to
//     Value for both, so a naive string-content check could misfire)
//   - value_to_pg_binary for PATH: verify it reuses value_to_pg_text (single
//     code path) rather than having its own, potentially unfixed, encoding

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/server/pg_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace sixseven;

namespace {

Uuid make_uuid(uint8_t fill) {
    Uuid u{};
    for (auto& b : u) b = fill;
    return u;
}

} // namespace

// =============================================================================
// UUID node PKs must never be quoted (only STRING PKs are quoted). UUID text
// contains literal '-' characters; verify these are never confused with
// STRING-delimiter content and that UUID stays unquoted/unescaped.
// =============================================================================

TEST(QA_GDB1304, PathUuidPkStaysUnquoted) {
    Path path;
    path.steps.emplace_back(Value(make_uuid(0x01)), -1);
    auto text = value_to_pg_text(Value(path));
    // Must not be wrapped in quotes -- UUID is not STRING.
    EXPECT_EQ(text.find('"'), std::string::npos);
    EXPECT_EQ(text, "[01010101-0101-0101-0101-010101010101]");
}

TEST(QA_GDB1304, PathUuidPkWithEdgeStepUnambiguous) {
    Path path;
    path.steps.emplace_back(Value(make_uuid(0xab)), 42);
    path.steps.emplace_back(Value(make_uuid(0xcd)), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, "[abababab-abab-abab-abab-abababababab-(42)->,cdcdcdcd-cdcd-cdcd-cdcd-cdcdcdcdcdcd]");
    // No quote characters should appear anywhere for UUID-only paths.
    EXPECT_EQ(text.find('"'), std::string::npos);
}

TEST(QA_GDB1304, PathMixedUuidAndStringPks) {
    // A STRING PK next to a UUID PK: only the STRING one should be quoted.
    Path path;
    path.steps.emplace_back(Value(make_uuid(0x0f)), 7);
    path.steps.emplace_back(Value(std::string("has,comma")), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, R"([0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f-(7)->,"has,comma"])");
}

// =============================================================================
// STRING PKs containing every combination of structural delimiter chars in a
// single multi-step path.
// =============================================================================

TEST(QA_GDB1304, PathStringPkWithParensAndArrow) {
    // A PK that itself looks like an edge-step delimiter sequence.
    Path path;
    path.steps.emplace_back(Value(std::string("a-(1)->b")), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, R"(["a-(1)->b"])");
}

TEST(QA_GDB1304, PathStringPkWithAllDelimitersCombined) {
    Path path;
    path.steps.emplace_back(Value(std::string("[a,b-(c)->d]")), 5);
    path.steps.emplace_back(Value(std::string("plain")), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, R"(["[a,b-(c)->d]"-(5)->,"plain"])");
}

TEST(QA_GDB1304, PathStringPkQuoteAndBackslashAdjacent) {
    // Backslash immediately followed by a quote -- escaping must not merge
    // or misorder the two escape sequences.
    Path path;
    path.steps.emplace_back(Value(std::string("a\\\"b")), -1); // contains: a \ " b
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, R"(["a\\\"b"])");
}

TEST(QA_GDB1304, PathStringPkOnlyBackslashes) {
    Path path;
    path.steps.emplace_back(Value(std::string("\\\\\\")), -1); // three backslashes
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, R"(["\\\\\\"])");
}

TEST(QA_GDB1304, PathStringPkUnicodeContent) {
    // Multi-byte UTF-8 content must pass through untouched (no accidental
    // byte-level escaping of continuation bytes that happen to match ASCII
    // delimiter values).
    Path path;
    path.steps.emplace_back(Value(std::string("caf\xc3\xa9,\xe6\x97\xa5\xe6\x9c\xac")), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, "[\"caf\xc3\xa9,\xe6\x97\xa5\xe6\x9c\xac\"]");
}

TEST(QA_GDB1304, PathManyStepsWithMixedDelimiterContent) {
    // Stress: 5 steps, alternating plain / delimiter-laden STRING PKs, each
    // separated by edges, must remain distinguishable step-by-step.
    Path path;
    path.steps.emplace_back(Value(std::string("n0")), 1);
    path.steps.emplace_back(Value(std::string("n1,x")), 2);
    path.steps.emplace_back(Value(std::string("[n2]")), 3);
    path.steps.emplace_back(Value(std::string("n3\"q")), 4);
    path.steps.emplace_back(Value(std::string("n4")), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text,
              R"(["n0"-(1)->,"n1,x"-(2)->,"[n2]"-(3)->,"n3\"q"-(4)->,"n4"])");
}

TEST(QA_GDB1304, PathStringPkWithOnlyDelimiterCharacter) {
    // A PK that is a single structural delimiter character on its own.
    Path path;
    path.steps.emplace_back(Value(std::string(",")), -1);
    auto text = value_to_pg_text(Value(path));
    EXPECT_EQ(text, R"([","])");
}

// =============================================================================
// value_to_pg_binary reachability: PATH's binary encoding must reuse the
// (now-fixed) text encoding rather than having a separate, unfixed path.
// =============================================================================

TEST(QA_GDB1304, PathBinaryEncodingMatchesFixedTextEncoding) {
    Path path;
    path.steps.emplace_back(Value(std::string("a,b")), -1);
    auto text = value_to_pg_text(Value(path));
    auto binary = value_to_pg_binary(Value(path));
    std::string binary_str(binary.begin(), binary.end());
    EXPECT_EQ(binary_str, text);
    // Confirm the quoting actually happened in the binary payload too (i.e.
    // this isn't accidentally hitting a separate unescaped encoder).
    EXPECT_EQ(binary_str, R"(["a,b"])");
}

TEST(QA_GDB1304, PathBinaryEncodingWithAllDelimiterCombo) {
    Path path;
    path.steps.emplace_back(Value(std::string("[x,y]-(z)->w")), 9);
    path.steps.emplace_back(Value(static_cast<int64_t>(42)), -1);
    auto binary = value_to_pg_binary(Value(path));
    std::string binary_str(binary.begin(), binary.end());
    EXPECT_EQ(binary_str, R"(["[x,y]-(z)->w"-(9)->,42])");
}

// =============================================================================
// Boundary / degenerate paths.
// =============================================================================

TEST(QA_GDB1304, PathSingleStepNoEdgeStringPk) {
    Path path;
    path.steps.emplace_back(Value(std::string("solo")), -1);
    EXPECT_EQ(value_to_pg_text(Value(path)), R"(["solo"])");
}

TEST(QA_GDB1304, PathEmptyPathProducesEmptyBrackets) {
    Path path;
    EXPECT_EQ(value_to_pg_text(Value(path)), "[]");
}

TEST(QA_GDB1304, PathStringPkVeryLongWithEmbeddedDelimiters) {
    std::string long_pk;
    for (int i = 0; i < 200; ++i) {
        long_pk += (i % 7 == 0) ? ',' : (i % 11 == 0 ? '"' : 'x');
    }
    Path path;
    path.steps.emplace_back(Value(long_pk), -1);
    auto text = value_to_pg_text(Value(path));
    // Must start/end with a quote and be strictly longer than the raw PK
    // (accounting for the 2 wrapping quotes plus escapes).
    ASSERT_GE(text.size(), long_pk.size() + 2);
    EXPECT_EQ(text.front(), '[');
    EXPECT_EQ(text[1], '"');
    EXPECT_EQ(text.back(), ']');
    EXPECT_EQ(text[text.size() - 2], '"');
}
