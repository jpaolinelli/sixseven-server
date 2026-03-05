/// @file tests/qa/test_qa_gdb_352.cpp
/// QA adversarial tests for GDB-352: normalizer enum inconsistent when
/// normalizer JSON object has no "type" field.
///
/// Bug: When normalizer object exists without a "type" field, the normalizer
/// enum stayed at the struct default (LOWERCASE) regardless of the actual
/// lowercase flag value, creating an inconsistent state.
///
/// Fix: When no "type" field is present, infer normalizer enum from the
/// lowercase flag: true → LOWERCASE, false → NONE.

#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/vector/text_normalizer.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace sixseven {
namespace {

/// Write a temporary JSON file and return its path.
class TempJson352 {
public:
    explicit TempJson352(const std::string& content, const std::string& suffix = "")
        : path_(std::filesystem::temp_directory_path() / ("sixseven_qa_352_" + suffix + ".json")) {
        std::ofstream out(path_);
        out << content;
    }

    ~TempJson352() { std::filesystem::remove(path_); }

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// ===================================================================
// Core fix: no type field + lowercase=false → NONE (not LOWERCASE)
// ===================================================================

TEST(QA_GDB352, NoTypeFieldLowercaseFalseGivesNone) {
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":false}
    })",
                     "core_false");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
    EXPECT_FALSE(result->normalizer_lowercase);
}

TEST(QA_GDB352, NoTypeFieldLowercaseTrueGivesLowercase) {
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":true}
    })",
                     "core_true");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

// ===================================================================
// Edge: no type field AND no lowercase field (defaults to true)
// ===================================================================

TEST(QA_GDB352, NoTypeFieldNoLowercaseFieldDefaultsToLowercase) {
    // normalizer object with only strip_accents — no type, no lowercase.
    // lowercase defaults to true in the parser → enum should be LOWERCASE.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"strip_accents":true}
    })",
                     "no_lc_field");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
    EXPECT_TRUE(result->normalizer_strip_accents);
}

TEST(QA_GDB352, EmptyNormalizerObjectDefaultsToLowercase) {
    // Empty normalizer object: no type, no lowercase (defaults true) → LOWERCASE.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{}
    })",
                     "empty_norm");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

// ===================================================================
// Edge: type field present but wrong type (not string)
// ===================================================================

TEST(QA_GDB352, TypeFieldIsInteger) {
    // type field is an integer, not a string — should fall to "no type" branch.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":42,"lowercase":false}
    })",
                     "type_int");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
    EXPECT_FALSE(result->normalizer_lowercase);
}

TEST(QA_GDB352, TypeFieldIsNull) {
    // type field is null — should fall to "no type" branch.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":null,"lowercase":true}
    })",
                     "type_null");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

TEST(QA_GDB352, TypeFieldIsBoolean) {
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":true,"lowercase":false}
    })",
                     "type_bool");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
    EXPECT_FALSE(result->normalizer_lowercase);
}

// ===================================================================
// Consistency: enum and flags must agree
// ===================================================================

TEST(QA_GDB352, ConsistencyEnumMatchesFlags) {
    // When type IS present as "Lowercase", and lowercase=false → NONE.
    // This tests that the existing type-present path is also consistent.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"Lowercase","lowercase":false}
    })",
                     "consistency");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::NONE);
    EXPECT_FALSE(result->normalizer_lowercase);
}

TEST(QA_GDB352, ConsistencyLowercaseTypeWithTrueFlag) {
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"type":"Lowercase","lowercase":true}
    })",
                     "lc_true");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
    EXPECT_TRUE(result->normalizer_lowercase);
}

// ===================================================================
// Integration: factory creates correct normalizer from inferred type
// ===================================================================

TEST(QA_GDB352, FactoryNoneProducesNullNormalizer) {
    // When normalizer is inferred as NONE, create_normalizer should return
    // a NullNormalizer (pass-through).
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":false}
    })",
                     "factory_none");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto norm = create_normalizer(*result);
    ASSERT_NE(norm, nullptr);
    // NullNormalizer: everything passes through unchanged.
    EXPECT_EQ(norm->normalize("HELLO World"), "HELLO World");
    EXPECT_EQ(norm->normalize("  spaces  "), "  spaces  ");
}

TEST(QA_GDB352, FactoryLowercaseFromInferredType) {
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":true}
    })",
                     "factory_lc");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto norm = create_normalizer(*result);
    ASSERT_NE(norm, nullptr);
    // LowercaseNormalizer: only lowercases, preserves whitespace.
    EXPECT_EQ(norm->normalize("HELLO"), "hello");
    EXPECT_EQ(norm->normalize("  A  "), "  a  ");
}

// ===================================================================
// Edge: lowercase field is non-boolean types
// ===================================================================

TEST(QA_GDB352, LowercaseFieldIsStringIgnored) {
    // lowercase: "false" (string, not boolean) → is_boolean() = false → default true.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":"false"}
    })",
                     "lc_string");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // "false" as string is not boolean → defaults to true.
    EXPECT_TRUE(result->normalizer_lowercase);
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
}

TEST(QA_GDB352, LowercaseFieldIsZeroIgnored) {
    // lowercase: 0 (integer, not boolean) → is_boolean() = false → default true.
    TempJson352 file(R"({
        "model":{"type":"WordPiece","vocab":{"a":0}},
        "normalizer":{"lowercase":0}
    })",
                     "lc_zero");
    auto result = load_tokenizer_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->normalizer_lowercase);
    EXPECT_EQ(result->normalizer, NormalizerType::LOWERCASE);
}

} // namespace
} // namespace sixseven
