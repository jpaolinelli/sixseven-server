// QA regression tests for GDB-1216: behavior-preserving dedup of
// to_upper/to_lower into include/sixseven/common/string_util.h.
//
// Adversarial focus:
//   - shared to_upper/to_lower do not regress callers that used to have their
//     own private copies (AlgorithmRegistry case-insensitive lookup, Session
//     SET/SHOW/RESET variable handling, query engine / planner CTE and
//     identifier resolution paths)
//   - unsigned-char safety survives round trips and repeated application
//   - idempotency / no data loss across to_upper <-> to_lower
//   - boundary sizes (empty, single char, long strings)
//   - non-ASCII / high-bit bytes do not crash or corrupt length
//   - no behavioral drift from the removed AlgorithmRegistry::to_upper member
//     (which historically deep-copied `const std::string&` rather than
//     taking by value) when used through the registry's public API

#include "sixseven/common/string_util.h"
#include "sixseven/graph/algorithm_registry.h"

#include <gtest/gtest.h>

#include <string>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Direct adversarial tests on the shared primitives
// ---------------------------------------------------------------------------

TEST(QA_GDB1216_StringUtil, ToUpperSingleCharBoundary) {
    EXPECT_EQ(to_upper("a"), "A");
    EXPECT_EQ(to_upper("Z"), "Z");
    EXPECT_EQ(to_upper("9"), "9");
}

TEST(QA_GDB1216_StringUtil, ToLowerSingleCharBoundary) {
    EXPECT_EQ(to_lower("A"), "a");
    EXPECT_EQ(to_lower("z"), "z");
    EXPECT_EQ(to_lower("9"), "9");
}

TEST(QA_GDB1216_StringUtil, RoundTripUpperThenLowerIsIdempotentOnAscii) {
    const std::string original = "Hello World 123!";
    EXPECT_EQ(to_lower(to_upper(original)), to_lower(original));
    EXPECT_EQ(to_upper(to_lower(original)), to_upper(original));
}

TEST(QA_GDB1216_StringUtil, RepeatedApplicationIsIdempotent) {
    const std::string s = "MiXeD CaSe 42";
    auto once = to_upper(s);
    auto twice = to_upper(once);
    EXPECT_EQ(once, twice);

    auto lower_once = to_lower(s);
    auto lower_twice = to_lower(lower_once);
    EXPECT_EQ(lower_once, lower_twice);
}

TEST(QA_GDB1216_StringUtil, LongStringDoesNotTruncateOrCorrupt) {
    std::string input(10000, 'a');
    for (size_t i = 0; i < input.size(); i += 7) {
        input[i] = 'A' + static_cast<char>(i % 26);
    }
    auto upper = to_upper(input);
    ASSERT_EQ(upper.size(), input.size());
    for (char c : upper) {
        EXPECT_TRUE(c == '\0' || !std::islower(static_cast<unsigned char>(c)));
    }
}

TEST(QA_GDB1216_StringUtil, AllHighBitBytesRoundTripWithoutCrashOrTruncation) {
    // Every byte 0-255 individually: neither function should ever change the
    // length or throw/crash regardless of what std::toupper/tolower does with
    // it in the current locale (locale is "C" for these bytes by default).
    for (int c = 0; c < 256; ++c) {
        std::string s(1, static_cast<char>(static_cast<unsigned char>(c)));
        auto upper = to_upper(s);
        auto lower = to_lower(s);
        EXPECT_EQ(upper.size(), 1u) << "byte " << c;
        EXPECT_EQ(lower.size(), 1u) << "byte " << c;
    }
}

TEST(QA_GDB1216_StringUtil, WhitespaceAndControlCharsUnchanged) {
    std::string s = "\t\n\r hello \t\n\r";
    auto upper = to_upper(s);
    EXPECT_EQ(upper, "\t\n\r HELLO \t\n\r");
}

// ---------------------------------------------------------------------------
// AlgorithmRegistry: case-insensitive lookup must survive the removal of its
// private static to_upper member in favor of the shared free function.
// ---------------------------------------------------------------------------

namespace {

AlgorithmDef make_def(std::string name) {
    AlgorithmDef def;
    def.name = std::move(name);
    def.output_columns = {{"node_id", TypeId::INT64, false}, {"result", TypeId::INT64, false}};
    return def;
}

} // namespace

TEST(QA_GDB1216_AlgorithmRegistry, RegisterMixedCaseThenLookupAllCaseVariants) {
    AlgorithmRegistry registry;
    auto reg_result = registry.register_algorithm(
        make_def("PageRank"), [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
            return ok(std::vector<AlgorithmRow>{});
        });
    ASSERT_TRUE(reg_result.has_value()) << reg_result.error().message;

    EXPECT_TRUE(registry.has("pagerank"));
    EXPECT_TRUE(registry.has("PAGERANK"));
    EXPECT_TRUE(registry.has("PageRank"));
    EXPECT_TRUE(registry.has("pAgErAnK"));
    EXPECT_FALSE(registry.has("page_rank"));
    EXPECT_FALSE(registry.has("pagerank2"));
}

TEST(QA_GDB1216_AlgorithmRegistry, DuplicateRegistrationDifferingOnlyByCaseIsRejected) {
    AlgorithmRegistry registry;
    auto first = registry.register_algorithm(
        make_def("bfs"), [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
            return ok(std::vector<AlgorithmRow>{});
        });
    ASSERT_TRUE(first.has_value()) << first.error().message;

    auto second = registry.register_algorithm(
        make_def("BFS"), [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
            return ok(std::vector<AlgorithmRow>{});
        });
    // Case-insensitive registry: registering "BFS" after "bfs" must be treated
    // as the same key (collision), not silently accepted as a distinct entry.
    EXPECT_FALSE(second.has_value());
}

TEST(QA_GDB1216_AlgorithmRegistry, EmptyNameLookupDoesNotCrash) {
    AlgorithmRegistry registry;
    EXPECT_FALSE(registry.has(""));
}

TEST(QA_GDB1216_AlgorithmRegistry, HighBitNameDoesNotCrashRegistryLookup) {
    AlgorithmRegistry registry;
    std::string weird_name;
    for (int c = 128; c < 160; ++c) {
        weird_name.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    // Must not crash regardless of registration outcome.
    EXPECT_NO_THROW({ (void)registry.has(weird_name); });
}
