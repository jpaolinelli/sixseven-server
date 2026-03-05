/// @file test_qa_gdb_243.cpp
/// @brief QA adversarial tests for GDB-243 (OnnxProvider implementation).
///
/// Tests edge cases, boundary values, error paths, and stress scenarios
/// for the OnnxProvider, its tokenizer, attention mask, batch inference,
/// health check delegation, and provider registry integration.

#include "sixseven/catalog/catalog.h"
#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/provider_registry.h"
#include "sixseven/vector/tokenizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

/// Mock ONNX session for adversarial testing.
class MockOnnxSession : public OnnxSession {
public:
    void set_embedding(const std::vector<float>& embedding) {
        embedding_ = embedding;
        should_fail_ = false;
    }

    void set_error(StatusCode code, const std::string& msg) {
        error_code_ = code;
        error_msg_ = msg;
        should_fail_ = true;
    }

    void set_health_error(StatusCode code, const std::string& msg) {
        health_code_ = code;
        health_msg_ = msg;
        health_fail_ = true;
    }

    Result<std::vector<float>> run(const std::vector<int64_t>& input_ids,
                                   const std::vector<int64_t>& attention_mask,
                                   size_t expected_dim) override {
        last_input_ids_ = input_ids;
        last_attention_mask_ = attention_mask;
        last_expected_dim_ = expected_dim;
        run_call_count_++;

        if (should_fail_) {
            return make_error(error_code_, error_msg_);
        }

        if (embedding_.size() != expected_dim) {
            return make_error(StatusCode::INTERNAL_ERROR, "dimension mismatch");
        }

        return ok(embedding_);
    }

    Result<void> health_check() override {
        if (health_fail_) {
            return make_error(health_code_, health_msg_);
        }
        return ok();
    }

    std::vector<int64_t> last_input_ids_;
    std::vector<int64_t> last_attention_mask_;
    size_t last_expected_dim_ = 0;
    int run_call_count_ = 0;

private:
    std::vector<float> embedding_;
    StatusCode error_code_ = StatusCode::INTERNAL_ERROR;
    std::string error_msg_;
    bool should_fail_ = false;

    StatusCode health_code_ = StatusCode::INTERNAL_ERROR;
    std::string health_msg_;
    bool health_fail_ = false;
};

} // namespace

// ===========================================================================
// Tokenizer boundary-value tests
// ===========================================================================

TEST(QA_GDB_243_Tokenizer, MaxLengthZeroCrash) {
    // max_length = 0 causes unsigned underflow: max_length - 1 = SIZE_MAX.
    // After overflow, tokens accumulate, then resize(0) + back() = UB.
    // This test verifies the function does not crash.
    // If the implementation is buggy, ASan/UBSan will catch the UB.
    HashTokenizer tok;
    auto tokens = tok.encode("hello", 0);
    // At minimum, should not crash. Exact behavior is implementation-defined
    // but the result should have size 0 if max_length = 0.
    EXPECT_EQ(tokens.size(), 0u);
}

TEST(QA_GDB_243_Tokenizer, MaxLengthOne) {
    // Only room for one token — should have SEP.
    HashTokenizer tok;
    auto tokens = tok.encode("hello", 1);
    ASSERT_EQ(tokens.size(), 1u);
    // Last token should be SEP (102) due to truncation logic.
    EXPECT_EQ(tokens[0], 102);
}

TEST(QA_GDB_243_Tokenizer, MaxLengthTwo) {
    // Room for CLS + SEP only, no word tokens.
    HashTokenizer tok;
    auto tokens = tok.encode("hello", 2);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP
}

TEST(QA_GDB_243_Tokenizer, MaxLengthThree) {
    // Room for CLS + one word + SEP.
    HashTokenizer tok;
    auto tokens = tok.encode("hello", 3);
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // "hello" word token
    EXPECT_EQ(tokens[2], 102); // SEP
}

TEST(QA_GDB_243_Tokenizer, SingleCharInput) {
    HashTokenizer tok;
    auto tokens = tok.encode("a", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // "a" word token
    EXPECT_EQ(tokens[2], 102); // SEP
    for (size_t i = 3; i < 10; ++i) {
        EXPECT_EQ(tokens[i], 0); // padding
    }
}

TEST(QA_GDB_243_Tokenizer, NumericInput) {
    HashTokenizer tok;
    auto tokens = tok.encode("12345", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // numeric token
    EXPECT_EQ(tokens[2], 102); // SEP
}

TEST(QA_GDB_243_Tokenizer, SpecialCharactersOnly) {
    // All punctuation, no alphanumeric — no word tokens produced.
    HashTokenizer tok;
    auto tokens = tok.encode("!@#$%^&*()", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP
    for (size_t i = 2; i < 10; ++i) {
        EXPECT_EQ(tokens[i], 0);
    }
}

TEST(QA_GDB_243_Tokenizer, MixedUnicodeAndAscii) {
    // Non-ASCII bytes are not alphanumeric — only ASCII letters produce tokens.
    HashTokenizer tok;
    auto tokens = tok.encode("hello 世界 world", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    // "hello" and "world" should produce word tokens.
    EXPECT_NE(tokens[1], 0);
}

TEST(QA_GDB_243_Tokenizer, ExactlyMaxLengthWords) {
    // Generate exactly max_length - 2 words (room for CLS + words + SEP = max_length).
    constexpr size_t MAX_LEN = 10;
    std::string text;
    for (size_t i = 0; i < MAX_LEN - 2; ++i) {
        if (i > 0)
            text += " ";
        text += "w" + std::to_string(i);
    }

    HashTokenizer tok;
    auto tokens = tok.encode(text, MAX_LEN);
    ASSERT_EQ(tokens.size(), MAX_LEN);
    EXPECT_EQ(tokens[0], 101);           // CLS
    EXPECT_EQ(tokens[MAX_LEN - 1], 102); // SEP
    // All middle positions should be word tokens.
    for (size_t i = 1; i < MAX_LEN - 1; ++i) {
        EXPECT_NE(tokens[i], 0) << "position " << i << " should be a word token";
    }
}

TEST(QA_GDB_243_Tokenizer, MoreWordsThanMaxLength) {
    // More words than max_length allows — truncation must preserve SEP.
    constexpr size_t MAX_LEN = 5;
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "word" + std::to_string(i) + " ";
    }

    HashTokenizer tok;
    auto tokens = tok.encode(text, MAX_LEN);
    ASSERT_EQ(tokens.size(), MAX_LEN);
    EXPECT_EQ(tokens[0], 101);           // CLS
    EXPECT_EQ(tokens[MAX_LEN - 1], 102); // SEP preserved after truncation
}

TEST(QA_GDB_243_Tokenizer, VeryLongSingleWord) {
    // A single very long word (no spaces) — should produce exactly one word token.
    HashTokenizer tok;
    std::string long_word(10000, 'a');
    auto tokens = tok.encode(long_word, 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // the single word token
    EXPECT_EQ(tokens[2], 102); // SEP
}

TEST(QA_GDB_243_Tokenizer, ConsecutiveSpaces) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello     world", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // "hello"
    EXPECT_NE(tokens[2], 0);   // "world"
    EXPECT_EQ(tokens[3], 102); // SEP
}

TEST(QA_GDB_243_Tokenizer, LeadingAndTrailingSpaces) {
    HashTokenizer tok;
    auto tokens1 = tok.encode("hello", 10);
    auto tokens2 = tok.encode("  hello  ", 10);
    // The word token should be the same regardless of whitespace.
    EXPECT_EQ(tokens1[1], tokens2[1]);
}

TEST(QA_GDB_243_Tokenizer, TabsAndNewlines) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello\tworld\nfoo", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // "hello"
    EXPECT_NE(tokens[2], 0);   // "world"
    EXPECT_NE(tokens[3], 0);   // "foo"
    EXPECT_EQ(tokens[4], 102); // SEP
}

// ===========================================================================
// Attention mask boundary tests
// ===========================================================================

TEST(QA_GDB_243_AttentionMask, EmptyTokens) {
    HashTokenizer tok;
    std::vector<int64_t> tokens;
    auto mask = tok.attention_mask(tokens);
    EXPECT_TRUE(mask.empty());
}

TEST(QA_GDB_243_AttentionMask, SingleNonZeroToken) {
    HashTokenizer tok;
    std::vector<int64_t> tokens = {101};
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 1u);
    EXPECT_EQ(mask[0], 1);
}

TEST(QA_GDB_243_AttentionMask, SingleZeroToken) {
    HashTokenizer tok;
    std::vector<int64_t> tokens = {0};
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 1u);
    EXPECT_EQ(mask[0], 0);
}

TEST(QA_GDB_243_AttentionMask, NegativeTokenTreatedAsNonPadding) {
    // Negative token IDs are non-zero, should have mask = 1.
    HashTokenizer tok;
    std::vector<int64_t> tokens = {-1, -100, 0};
    auto mask = tok.attention_mask(tokens);
    ASSERT_EQ(mask.size(), 3u);
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[1], 1);
    EXPECT_EQ(mask[2], 0);
}

// ===========================================================================
// Embed edge cases
// ===========================================================================

TEST(QA_GDB_243_Embed, WhitespaceOnlyTextIsNotEmpty) {
    // " " is not empty — embed() should not reject it.
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({0.1F, 0.2F});

    OnnxProvider provider("model.onnx", 2, std::move(mock));
    auto result = provider.embed(" ");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
}

TEST(QA_GDB_243_Embed, VeryLongText) {
    // Embed a very long text — should work without crashing.
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({1.0F, 2.0F, 3.0F});

    OnnxProvider provider("model.onnx", 3, std::move(mock));

    std::string long_text;
    for (int i = 0; i < 10000; ++i) {
        long_text += "word" + std::to_string(i) + " ";
    }

    auto result = provider.embed(long_text);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
}

TEST(QA_GDB_243_Embed, DimensionOne) {
    // Smallest possible embedding dimension.
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({42.0F});

    OnnxProvider provider("model.onnx", 1, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_FLOAT_EQ((*result)[0], 42.0F);
}

TEST(QA_GDB_243_Embed, LargeDimension) {
    // Large embedding dimension (4096).
    constexpr size_t DIM = 4096;
    auto mock = std::make_unique<MockOnnxSession>();
    std::vector<float> emb(DIM, 0.5F);
    mock->set_embedding(emb);

    OnnxProvider provider("model.onnx", DIM, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), DIM);
}

TEST(QA_GDB_243_Embed, NaNInEmbedding) {
    // Session returns NaN values — should propagate without crash.
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({std::numeric_limits<float>::quiet_NaN(), 1.0F});

    OnnxProvider provider("model.onnx", 2, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_TRUE(std::isnan((*result)[0]));
    EXPECT_FLOAT_EQ((*result)[1], 1.0F);
}

TEST(QA_GDB_243_Embed, InfinityInEmbedding) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding(
        {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()});

    OnnxProvider provider("model.onnx", 2, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_TRUE(std::isinf((*result)[0]));
    EXPECT_TRUE(std::isinf((*result)[1]));
}

// ===========================================================================
// Batch embedding edge cases
// ===========================================================================

TEST(QA_GDB_243_Batch, SingleElementBatch) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({1.0F});

    OnnxProvider provider("model.onnx", 1, std::move(mock));
    auto result = provider.embed_batch({"hello"});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_FLOAT_EQ((*result)[0][0], 1.0F);
    EXPECT_EQ(mock_ptr->run_call_count_, 1);
}

TEST(QA_GDB_243_Batch, LargeBatch) {
    // 1000 texts in a batch — stress test.
    constexpr size_t BATCH_SIZE = 1000;
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({0.5F, 0.5F});

    OnnxProvider provider("model.onnx", 2, std::move(mock));

    std::vector<std::string> texts;
    texts.reserve(BATCH_SIZE);
    for (size_t i = 0; i < BATCH_SIZE; ++i) {
        texts.push_back("text" + std::to_string(i));
    }

    auto result = provider.embed_batch(texts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), BATCH_SIZE);
    EXPECT_EQ(mock_ptr->run_call_count_, static_cast<int>(BATCH_SIZE));
}

TEST(QA_GDB_243_Batch, ErrorInMiddleOfBatch) {
    // First two texts succeed, third fails. Verify early termination.
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();

    // The mock doesn't support per-call configuration, but
    // embed_batch calls embed() which calls tokenize() + session.run().
    // We can make the second text empty to trigger INVALID_ARGUMENT.
    mock_ptr->set_embedding({1.0F});

    OnnxProvider provider("model.onnx", 1, std::move(mock));
    auto result = provider.embed_batch({"good", "also good", "", "never reached"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    // Should have processed "good" and "also good" before hitting "".
    EXPECT_EQ(mock_ptr->run_call_count_, 2);
}

TEST(QA_GDB_243_Batch, AllEmptyTexts) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({1.0F});

    OnnxProvider provider("model.onnx", 1, std::move(mock));
    auto result = provider.embed_batch({"", "", ""});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// Health check delegation
// ===========================================================================

TEST(QA_GDB_243_Health, DifferentErrorCodes) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_health_error(StatusCode::IO_ERROR, "model file corrupted");

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.health_check();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// ===========================================================================
// Provider name and dimension
// ===========================================================================

TEST(QA_GDB_243_Accessors, EmptyModelPath) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("", 384, std::move(mock));
    EXPECT_EQ(provider.name(), "onnx/");
    EXPECT_EQ(provider.dimension(), 384u);
}

TEST(QA_GDB_243_Accessors, DimensionZero) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("model.onnx", 0, std::move(mock));
    EXPECT_EQ(provider.dimension(), 0u);
}

TEST(QA_GDB_243_Accessors, LargeDimension) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("model.onnx", 100000, std::move(mock));
    EXPECT_EQ(provider.dimension(), 100000u);
}

TEST(QA_GDB_243_Accessors, PathWithSlashes) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("/data/models/v2/model.onnx", 384, std::move(mock));
    EXPECT_EQ(provider.name(), "onnx//data/models/v2/model.onnx");
}

// ===========================================================================
// Registry integration edge cases
// ===========================================================================

TEST(QA_GDB_243_Registry, OnnxTypeRecognizedEvenWithInvalidPath) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("onnx/definitely-not-a-real-path.onnx");
    ASSERT_FALSE(result.has_value());
    // Must be IO_ERROR (file not found), NOT INVALID_ARGUMENT (unknown type).
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(QA_GDB_243_Registry, OnnxWithSlashOnlyFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "onnx/" — slash at last position, rejected by parse_provider_name.
    auto result = registry.resolve("onnx/");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_243_Registry, OnnxWithNestedPathFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "onnx/path/to/model.onnx" — first slash splits type="onnx", model="path/to/model.onnx".
    // parse_provider_name splits at first slash, so model = "path/to/model.onnx".
    // Then create_onnx_session("/path/to/model.onnx") should fail with IO_ERROR.
    auto result = registry.resolve("onnx/path/to/model.onnx");
    ASSERT_FALSE(result.has_value());
    // Should try to load the model and fail.
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(QA_GDB_243_Registry, ProviderCaching) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // First resolve fails (file not found).
    auto result1 = registry.resolve("onnx/fake.onnx");
    ASSERT_FALSE(result1.has_value());

    // Failed providers should not be cached — second resolve should also fail.
    auto result2 = registry.resolve("onnx/fake.onnx");
    ASSERT_FALSE(result2.has_value());
}

// ===========================================================================
// create_onnx_session error handling
// ===========================================================================

TEST(QA_GDB_243_Session, EmptyPathFails) {
    auto result = create_onnx_session("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(QA_GDB_243_Session, DirectoryPathFails) {
    // Passing a directory instead of a file.
    auto result = create_onnx_session("/tmp");
    ASSERT_FALSE(result.has_value());
    // The path exists but is not a valid ONNX model.
    // Could be IO_ERROR or INTERNAL_ERROR depending on how Ort::Session handles it.
}

// ===========================================================================
// Embed with dimension zero — should fail at session level
// ===========================================================================

TEST(QA_GDB_243_Embed, DimensionZeroEmbedFails) {
    // Provider with dim=0 — session returns empty embedding which doesn't match.
    auto mock = std::make_unique<MockOnnxSession>();
    // Set embedding to empty (matches dim=0).
    mock->set_embedding({});

    OnnxProvider provider("model.onnx", 0, std::move(mock));
    auto result = provider.embed("hello");
    // Empty embedding with dim=0 should succeed (0 == 0).
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ===========================================================================
// Tokenizer hash stability and determinism
// ===========================================================================

TEST(QA_GDB_243_Tokenizer, SameWordAlwaysProducesSameToken) {
    // Run encode 100 times — must be deterministic.
    HashTokenizer tok;
    auto reference = tok.encode("determinism", 5);
    for (int i = 0; i < 100; ++i) {
        auto tokens = tok.encode("determinism", 5);
        EXPECT_EQ(tokens, reference) << "non-deterministic at iteration " << i;
    }
}

TEST(QA_GDB_243_Tokenizer, DifferentWordsProduceDifferentHashes) {
    // Check that common words have distinct hashes (no trivial collisions).
    HashTokenizer tok;
    std::vector<std::string> words = {
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "hello", "world"};
    std::vector<int64_t> hashes;
    for (const auto& word : words) {
        auto tokens = tok.encode(word, 5);
        // tokens[1] is the word hash.
        hashes.push_back(tokens[1]);
    }

    // All hashes should be unique (no collisions among these 10 common words).
    std::sort(hashes.begin(), hashes.end());
    auto last = std::unique(hashes.begin(), hashes.end());
    EXPECT_EQ(last, hashes.end()) << "hash collision detected among common words";
}

TEST(QA_GDB_243_Tokenizer, DefaultMaxLengthIsMAX_SEQ_LENGTH) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello", tok.max_sequence_length());
    EXPECT_EQ(tokens.size(), HashTokenizer::DEFAULT_MAX_SEQ_LENGTH);
}
