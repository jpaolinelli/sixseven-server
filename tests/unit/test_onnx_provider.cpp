/// @file test_onnx_provider.cpp
/// @brief Unit tests for OnnxProvider (GDB-243, GDB-324).
///
/// Tests the OnnxProvider implementation using a mock ONNX session,
/// covering: single/batch embedding, tokenizer, attention mask,
/// error handling, health check, registry integration, auto-discovery,
/// and tokenizer integration.

#include "sixseven/catalog/catalog.h"
#include "sixseven/vector/bpe_tokenizer.h"
#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/provider_registry.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"
#include "sixseven/vector/wordpiece_tokenizer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

/// Mock ONNX session that returns configurable embedding vectors.
class MockOnnxSession : public OnnxSession {
public:
    /// Set the embedding that run() should return.
    void set_embedding(const std::vector<float>& embedding) {
        embedding_ = embedding;
        should_fail_ = false;
    }

    /// Make run() return an error.
    void set_error(StatusCode code, const std::string& msg) {
        error_code_ = code;
        error_msg_ = msg;
        should_fail_ = true;
    }

    /// Make health_check() return an error.
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
            return make_error(StatusCode::INTERNAL_ERROR,
                              "ONNX dimension mismatch: expected " + std::to_string(expected_dim) +
                                  ", got " + std::to_string(embedding_.size()));
        }

        return ok(embedding_);
    }

    Result<void> health_check() override {
        if (health_fail_) {
            return make_error(health_code_, health_msg_);
        }
        return ok();
    }

    // Captured state for assertions.
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

// ---------------------------------------------------------------------------
// Basic interface and accessors
// ---------------------------------------------------------------------------

TEST(OnnxProvider, NameIncludesModelPath) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("/path/to/model.onnx", 384, std::move(mock));
    EXPECT_EQ(provider.name(), "onnx//path/to/model.onnx");
}

TEST(OnnxProvider, DimensionMatchesConstructor) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("model.onnx", 768, std::move(mock));
    EXPECT_EQ(provider.dimension(), 768u);
}

// ---------------------------------------------------------------------------
// Single embedding
// ---------------------------------------------------------------------------

TEST(OnnxProvider, EmbedSingleText) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({0.1F, 0.2F, 0.3F});

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.embed("hello world");

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[1], 0.2F);
    EXPECT_FLOAT_EQ((*result)[2], 0.3F);

    // Verify session was called with correct expected dimension.
    EXPECT_EQ(mock_ptr->last_expected_dim_, 3u);
    EXPECT_EQ(mock_ptr->run_call_count_, 1);
}

TEST(OnnxProvider, EmbedEmptyTextFails) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({0.1F, 0.2F, 0.3F});

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.embed("");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(OnnxProvider, EmbedPropagatesSessionError) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_error(StatusCode::INTERNAL_ERROR, "inference failed");

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.embed("hello");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

// ---------------------------------------------------------------------------
// Batch embedding
// ---------------------------------------------------------------------------

TEST(OnnxProvider, EmbedBatchMultipleTexts) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({1.0F, 2.0F});

    OnnxProvider provider("model.onnx", 2, std::move(mock));
    auto result = provider.embed_batch({"hello", "world", "foo"});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    // Each text should produce the same mock embedding.
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_EQ((*result)[i].size(), 2u);
        EXPECT_FLOAT_EQ((*result)[i][0], 1.0F);
        EXPECT_FLOAT_EQ((*result)[i][1], 2.0F);
    }
    EXPECT_EQ(mock_ptr->run_call_count_, 3);
}

TEST(OnnxProvider, EmbedBatchEmpty) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("model.onnx", 3, std::move(mock));

    auto result = provider.embed_batch({});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST(OnnxProvider, EmbedBatchStopsOnFirstError) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    // First call succeeds, but set_error makes all calls fail.
    mock_ptr->set_error(StatusCode::INTERNAL_ERROR, "broken model");

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.embed_batch({"a", "b", "c"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
    // Should have stopped after first failure.
    EXPECT_EQ(mock_ptr->run_call_count_, 1);
}

TEST(OnnxProvider, EmbedBatchFailsOnEmptyText) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_embedding({0.1F, 0.2F});

    OnnxProvider provider("model.onnx", 2, std::move(mock));
    // Second text is empty — should fail.
    auto result = provider.embed_batch({"hello", "", "world"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Batched dispatch — verifies OnnxProvider::embed_batch fans all N texts into
// a single OnnxSession::run_batch call rather than looping per-text. Prior to
// the real batching fix, embed_batch called OnnxSession::run N times; now it
// tokenizes all N texts and invokes run_batch exactly once, and the
// RealOnnxSession override turns that into a single Ort::Session::Run over a
// [N, max_len] tensor. Tests here use a mock that overrides run_batch so we
// can assert the dispatch happens through that code path.
// ---------------------------------------------------------------------------

namespace {

/// Mock session that overrides run_batch directly so tests can distinguish
/// between "embed_batch called run N times" (default fallback) and
/// "embed_batch called run_batch once with all N rows" (real batched dispatch).
class BatchCountingMockSession : public OnnxSession {
public:
    BatchCountingMockSession() = default;

    Result<std::vector<float>> run(const std::vector<int64_t>& /*input_ids*/,
                                   const std::vector<int64_t>& /*attention_mask*/,
                                   size_t /*expected_dim*/) override {
        run_call_count_++;
        return make_error(StatusCode::INTERNAL_ERROR,
                          "BatchCountingMockSession::run should never be called; "
                          "embed_batch must dispatch via run_batch");
    }

    Result<std::vector<std::vector<float>>>
    run_batch(const std::vector<std::vector<int64_t>>& input_ids,
              const std::vector<std::vector<int64_t>>& attention_mask,
              size_t expected_dim) override {
        batch_call_count_++;
        last_batch_size_ = input_ids.size();
        last_attention_mask_ = attention_mask;
        last_expected_dim_ = expected_dim;

        // Return one deterministic per-row embedding: row i's vector is all (i+1)*0.1F.
        std::vector<std::vector<float>> out;
        out.reserve(input_ids.size());
        for (size_t i = 0; i < input_ids.size(); ++i) {
            out.emplace_back(expected_dim, static_cast<float>(i + 1) * 0.1F);
        }
        return ok(std::move(out));
    }

    Result<void> health_check() override { return ok(); }

    int run_call_count_ = 0;
    int batch_call_count_ = 0;
    size_t last_batch_size_ = 0;
    size_t last_expected_dim_ = 0;
    std::vector<std::vector<int64_t>> last_attention_mask_;
};

} // namespace

TEST(OnnxProvider, EmbedBatchDispatchesViaRunBatch) {
    auto mock = std::make_unique<BatchCountingMockSession>();
    auto* mock_ptr = mock.get();

    OnnxProvider provider("model.onnx", 4, std::move(mock));
    auto result = provider.embed_batch({"first", "second", "third"});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);

    // The whole point of this test: exactly one run_batch call and zero run() calls.
    EXPECT_EQ(mock_ptr->batch_call_count_, 1);
    EXPECT_EQ(mock_ptr->run_call_count_, 0);
    EXPECT_EQ(mock_ptr->last_batch_size_, 3u);
    EXPECT_EQ(mock_ptr->last_expected_dim_, 4u);

    // Per-row outputs should come back in order.
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_EQ((*result)[i].size(), 4u);
        for (float v : (*result)[i]) {
            EXPECT_FLOAT_EQ(v, static_cast<float>(i + 1) * 0.1F);
        }
    }
}

TEST(OnnxProvider, EmbedBatchPassesPerRowTokenizedInputs) {
    auto mock = std::make_unique<BatchCountingMockSession>();
    auto* mock_ptr = mock.get();

    OnnxProvider provider("model.onnx", 2, std::move(mock));
    auto result = provider.embed_batch({"short text", "a longer piece of text"});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Each row must have its own attention mask (default HashTokenizer produces a
    // mask matching the tokenized length). We cannot assert exact lengths without
    // coupling to HashTokenizer internals, but we can assert that the mock received
    // two rows with independent, non-empty masks.
    ASSERT_EQ(mock_ptr->last_attention_mask_.size(), 2u);
    EXPECT_FALSE(mock_ptr->last_attention_mask_[0].empty());
    EXPECT_FALSE(mock_ptr->last_attention_mask_[1].empty());
}

TEST(OnnxProvider, EmbedBatchEmptyShortCircuitsRunBatch) {
    auto mock = std::make_unique<BatchCountingMockSession>();
    auto* mock_ptr = mock.get();

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.embed_batch({});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());

    // Empty batch should not reach the session at all.
    EXPECT_EQ(mock_ptr->batch_call_count_, 0);
    EXPECT_EQ(mock_ptr->run_call_count_, 0);
}

TEST(OnnxProvider, EmbedBatchFailsFastOnEmptyTextBeforeDispatch) {
    auto mock = std::make_unique<BatchCountingMockSession>();
    auto* mock_ptr = mock.get();

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    // Second text is empty — should fail before ever reaching run_batch.
    auto result = provider.embed_batch({"hello", "", "world"});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(mock_ptr->batch_call_count_, 0);
    EXPECT_EQ(mock_ptr->run_call_count_, 0);
}

// ---------------------------------------------------------------------------
// Health check
// ---------------------------------------------------------------------------

TEST(OnnxProvider, HealthCheckSuccess) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("model.onnx", 3, std::move(mock));

    auto result = provider.health_check();
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(OnnxProvider, HealthCheckFailure) {
    auto mock = std::make_unique<MockOnnxSession>();
    mock->set_health_error(StatusCode::INTERNAL_ERROR, "model not loaded");

    OnnxProvider provider("model.onnx", 3, std::move(mock));
    auto result = provider.health_check();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

// ---------------------------------------------------------------------------
// Tokenizer (HashTokenizer via Tokenizer interface)
// ---------------------------------------------------------------------------

TEST(OnnxProvider, TokenizeBasicText) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello world", 10);

    ASSERT_EQ(tokens.size(), 10u);
    // CLS at start.
    EXPECT_EQ(tokens[0], 101);
    // Two word tokens.
    EXPECT_NE(tokens[1], 0);
    EXPECT_NE(tokens[2], 0);
    // SEP after words.
    EXPECT_EQ(tokens[3], 102);
    // Padding fills the rest.
    for (size_t i = 4; i < 10; ++i) {
        EXPECT_EQ(tokens[i], 0) << "position " << i << " should be padding";
    }
}

TEST(OnnxProvider, TokenizeDeterministic) {
    HashTokenizer tok;
    auto tokens1 = tok.encode("hello world", 10);
    auto tokens2 = tok.encode("hello world", 10);
    EXPECT_EQ(tokens1, tokens2);
}

TEST(OnnxProvider, TokenizeDifferentTextsDifferentTokens) {
    HashTokenizer tok;
    auto tokens1 = tok.encode("hello", 10);
    auto tokens2 = tok.encode("goodbye", 10);
    // Word tokens should differ (CLS/SEP/padding are the same).
    EXPECT_NE(tokens1[1], tokens2[1]);
}

TEST(OnnxProvider, TokenizeLongTextTruncated) {
    // Build a very long text that exceeds max_length.
    std::string long_text;
    for (int i = 0; i < 200; ++i) {
        long_text += "word" + std::to_string(i) + " ";
    }

    HashTokenizer tok;
    auto tokens = tok.encode(long_text, 10);
    ASSERT_EQ(tokens.size(), 10u);
    // Should have CLS at start.
    EXPECT_EQ(tokens[0], 101);
    // Should end with SEP (truncation preserves SEP).
    EXPECT_EQ(tokens[9], 102);
}

TEST(OnnxProvider, TokenizePunctuationSplitsWords) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello,world", 10);
    ASSERT_EQ(tokens.size(), 10u);
    // "hello" and "world" should produce two separate word tokens.
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_NE(tokens[1], 0);   // "hello"
    EXPECT_NE(tokens[2], 0);   // "world"
    EXPECT_EQ(tokens[3], 102); // SEP
}

TEST(OnnxProvider, TokenizeCaseInsensitive) {
    HashTokenizer tok;
    auto tokens_lower = tok.encode("hello", 10);
    auto tokens_upper = tok.encode("HELLO", 10);
    // Both should produce the same word token.
    EXPECT_EQ(tokens_lower[1], tokens_upper[1]);
}

TEST(OnnxProvider, TokenizeEmptyTextOnlySpecialTokens) {
    HashTokenizer tok;
    auto tokens = tok.encode("", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP
    for (size_t i = 2; i < 10; ++i) {
        EXPECT_EQ(tokens[i], 0); // padding
    }
}

TEST(OnnxProvider, TokenizeWhitespaceOnlyTextOnlySpecialTokens) {
    HashTokenizer tok;
    auto tokens = tok.encode("   \t\n  ", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 102); // SEP
}

TEST(OnnxProvider, TokenizeWordTokensInValidRange) {
    HashTokenizer tok;
    auto tokens = tok.encode("the quick brown fox", 10);
    // Word tokens should be in [104, 30103].
    for (size_t i = 1; i <= 4; ++i) {
        EXPECT_GE(tokens[i], 104) << "token at " << i << " below range";
        EXPECT_LE(tokens[i], 30103) << "token at " << i << " above range";
    }
}

// ---------------------------------------------------------------------------
// Attention mask
// ---------------------------------------------------------------------------

TEST(OnnxProvider, AttentionMaskNonPaddingIsOne) {
    HashTokenizer tok;
    auto tokens = tok.encode("hello world", 8);
    auto mask = tok.attention_mask(tokens);

    ASSERT_EQ(mask.size(), 8u);
    // CLS=101, "hello", "world", SEP=102 → all non-zero → mask = 1.
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[1], 1);
    EXPECT_EQ(mask[2], 1);
    EXPECT_EQ(mask[3], 1);
    // Padding → mask = 0.
    EXPECT_EQ(mask[4], 0);
    EXPECT_EQ(mask[5], 0);
    EXPECT_EQ(mask[6], 0);
    EXPECT_EQ(mask[7], 0);
}

TEST(OnnxProvider, AttentionMaskAllPadding) {
    HashTokenizer tok;
    std::vector<int64_t> tokens(10, 0);
    auto mask = tok.attention_mask(tokens);

    ASSERT_EQ(mask.size(), 10u);
    for (auto m : mask) {
        EXPECT_EQ(m, 0);
    }
}

// ---------------------------------------------------------------------------
// Input/output validation via mock session
// ---------------------------------------------------------------------------

TEST(OnnxProvider, SessionReceivesCorrectInputLength) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(384, 0.5F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider("model.onnx", 384, std::move(mock));
    auto result = provider.embed("test input");

    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Input IDs and attention mask should be MAX_SEQ_LENGTH.
    EXPECT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
    EXPECT_EQ(mock_ptr->last_attention_mask_.size(), OnnxProvider::MAX_SEQ_LENGTH);
}

TEST(OnnxProvider, SessionReceivesMatchingDimension) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(512, 0.1F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider("model.onnx", 512, std::move(mock));
    auto result = provider.embed("hello");

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(mock_ptr->last_expected_dim_, 512u);
}

TEST(OnnxProvider, DimensionMismatchFromSession) {
    auto mock = std::make_unique<MockOnnxSession>();
    // Session returns dim=3 but provider expects dim=5.
    mock->set_embedding({0.1F, 0.2F, 0.3F});

    OnnxProvider provider("model.onnx", 5, std::move(mock));
    auto result = provider.embed("hello");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

// ---------------------------------------------------------------------------
// Registry integration
// ---------------------------------------------------------------------------

TEST(OnnxProvider, RegistryRecognizesOnnxType) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "onnx/nonexistent-model.onnx" — type is recognized, file not found.
    auto result = registry.resolve("onnx/nonexistent-model.onnx");
    ASSERT_FALSE(result.has_value());
    // Should be IO_ERROR (file not found), NOT INVALID_ARGUMENT (unknown type).
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(OnnxProvider, RegistryOnnxWithoutModelPathFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "onnx/" — empty model path.
    auto result = registry.resolve("onnx/");
    ASSERT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// create_onnx_session error handling
// ---------------------------------------------------------------------------

TEST(OnnxProvider, CreateSessionFileNotFound) {
    auto result = create_onnx_session("/nonexistent/path/model.onnx");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(OnnxProvider, CreateSessionInvalidModel) {
    // Create a temp file that isn't a valid ONNX model.
    auto temp_dir = std::filesystem::temp_directory_path() / "sixseven_test_onnx";
    std::filesystem::create_directories(temp_dir);
    auto bad_model = temp_dir / "bad_model.onnx";

    // Write garbage data.
    {
        std::ofstream out(bad_model, std::ios::binary);
        out << "this is not an onnx model";
    }

    auto result = create_onnx_session(bad_model.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);

    // Cleanup.
    std::filesystem::remove_all(temp_dir);
}

// ---------------------------------------------------------------------------
// Model directory auto-discovery (GDB-343)
// ---------------------------------------------------------------------------

class OnnxAutoDiscovery : public ::testing::Test {
protected:
    void SetUp() override {
        base_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_autodiscovery";
        std::filesystem::create_directories(base_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(base_dir_); }

    void write_file(const std::filesystem::path& path, const std::string& content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << content;
    }

    std::filesystem::path base_dir_;
};

TEST_F(OnnxAutoDiscovery, DirectoryWithModelOnnx) {
    auto model_dir = base_dir_ / "my_model";
    write_file(model_dir / "model.onnx", "fake_model_data");

    auto result = resolve_onnx_model_paths(model_dir.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (model_dir / "model.onnx").string());
    EXPECT_TRUE(result->tokenizer_path.empty());
}

TEST_F(OnnxAutoDiscovery, DirectoryWithModelOnnxAndTokenizer) {
    auto model_dir = base_dir_ / "my_model";
    write_file(model_dir / "model.onnx", "fake_model_data");
    write_file(model_dir / "tokenizer.json", "{}");

    auto result = resolve_onnx_model_paths(model_dir.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (model_dir / "model.onnx").string());
    EXPECT_EQ(result->tokenizer_path, (model_dir / "tokenizer.json").string());
}

TEST_F(OnnxAutoDiscovery, DirectoryWithOnnxSubdirectory) {
    auto model_dir = base_dir_ / "my_model";
    write_file(model_dir / "onnx" / "model.onnx", "fake_model_data");
    write_file(model_dir / "tokenizer.json", "{}");

    auto result = resolve_onnx_model_paths(model_dir.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (model_dir / "onnx" / "model.onnx").string());
    EXPECT_EQ(result->tokenizer_path, (model_dir / "tokenizer.json").string());
}

TEST_F(OnnxAutoDiscovery, DirectoryWithoutModelOnnxFails) {
    auto model_dir = base_dir_ / "empty_model";
    std::filesystem::create_directories(model_dir);

    auto result = resolve_onnx_model_paths(model_dir.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(OnnxAutoDiscovery, DirectOnnxFilePath) {
    auto model_file = base_dir_ / "model.onnx";
    write_file(model_file, "fake_model_data");

    auto result = resolve_onnx_model_paths(model_file.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, model_file.string());
    EXPECT_TRUE(result->tokenizer_path.empty());
}

TEST_F(OnnxAutoDiscovery, DirectOnnxFileWithTokenizerAlongside) {
    write_file(base_dir_ / "model.onnx", "fake_model_data");
    write_file(base_dir_ / "tokenizer.json", "{}");

    auto result = resolve_onnx_model_paths((base_dir_ / "model.onnx").string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (base_dir_ / "model.onnx").string());
    EXPECT_EQ(result->tokenizer_path, (base_dir_ / "tokenizer.json").string());
}

TEST_F(OnnxAutoDiscovery, NonexistentPathFails) {
    auto result = resolve_onnx_model_paths("/nonexistent/path/model");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// ---------------------------------------------------------------------------
// Tokenizer wiring (GDB-344)
// ---------------------------------------------------------------------------

TEST(OnnxProvider, ConstructorWithCustomTokenizer) {
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({0.1F, 0.2F, 0.3F});

    // Build a minimal WordPiece config.
    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"hello", 1000}, {"world", 1001}, {"##lo", 1002}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;

    auto tokenizer = std::make_unique<WordPieceTokenizer>(config);

    OnnxProvider provider("model.onnx", 3, std::move(mock), std::move(tokenizer));
    auto result = provider.embed("hello world");

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(mock_ptr->run_call_count_, 1);

    // Verify the tokenizer is the WordPiece one (vocab_size = config vocab size).
    EXPECT_EQ(provider.tokenizer().vocab_size(), 3u);
}

TEST(OnnxProvider, DefaultConstructorUsesHashTokenizer) {
    auto mock = std::make_unique<MockOnnxSession>();
    OnnxProvider provider("model.onnx", 3, std::move(mock));

    // HashTokenizer has VOCAB_SIZE = 30000.
    EXPECT_EQ(provider.tokenizer().vocab_size(), HashTokenizer::VOCAB_SIZE);
}

// ---------------------------------------------------------------------------
// Provider with model directory containing tokenizer.json (GDB-324 AC)
// ---------------------------------------------------------------------------

namespace {

/// Write a minimal but valid WordPiece tokenizer.json file.
void write_wordpiece_tokenizer_json(const std::filesystem::path& path) {
    nlohmann::json doc;

    // Model section.
    doc["model"]["type"] = "WordPiece";
    doc["model"]["continuing_subword_prefix"] = "##";
    doc["model"]["vocab"] = {{"[PAD]", 0},
                             {"[UNK]", 100},
                             {"[CLS]", 101},
                             {"[SEP]", 102},
                             {"[MASK]", 103},
                             {"hello", 104},
                             {"world", 105},
                             {"##ing", 106},
                             {"test", 107},
                             {"##ed", 108}};

    // Added tokens.
    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}},
                               {{"id", 100}, {"content", "[UNK]"}, {"special", true}},
                               {{"id", 101}, {"content", "[CLS]"}, {"special", true}},
                               {{"id", 102}, {"content", "[SEP]"}, {"special", true}},
                               {{"id", 103}, {"content", "[MASK]"}, {"special", true}}});

    // Normalizer.
    doc["normalizer"]["type"] = "BertNormalizer";
    doc["normalizer"]["lowercase"] = true;
    doc["normalizer"]["strip_accents"] = false;

    // Pre-tokenizer.
    doc["pre_tokenizer"]["type"] = "BertPreTokenizer";

    std::ofstream out(path);
    out << doc.dump(2);
}

/// Write a minimal but valid BPE tokenizer.json file.
void write_bpe_tokenizer_json(const std::filesystem::path& path) {
    nlohmann::json doc;

    doc["model"]["type"] = "BPE";
    doc["model"]["vocab"] = {{"[PAD]", 0},
                             {"[UNK]", 100},
                             {"[CLS]", 101},
                             {"[SEP]", 102},
                             {"h", 200},
                             {"e", 201},
                             {"l", 202},
                             {"o", 203},
                             {"he", 204},
                             {"ll", 205},
                             {"hello", 206}};
    doc["model"]["merges"] = {"h e", "l l", "he ll", "hell o"};

    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}},
                               {{"id", 100}, {"content", "[UNK]"}, {"special", true}},
                               {{"id", 101}, {"content", "[CLS]"}, {"special", true}},
                               {{"id", 102}, {"content", "[SEP]"}, {"special", true}}});

    doc["normalizer"]["type"] = "Lowercase";
    doc["normalizer"]["lowercase"] = true;

    doc["pre_tokenizer"]["type"] = "Whitespace";

    std::ofstream out(path);
    out << doc.dump(2);
}

} // namespace

TEST_F(OnnxAutoDiscovery, ProviderWithWordPieceTokenizerDirectory) {
    // Set up a model directory with model.onnx and a WordPiece tokenizer.json.
    auto model_dir = base_dir_ / "minilm";
    write_file(model_dir / "model.onnx", "fake_model_data");
    write_wordpiece_tokenizer_json(model_dir / "tokenizer.json");

    // Resolve paths and verify tokenizer is detected.
    auto paths = resolve_onnx_model_paths(model_dir.string());
    ASSERT_TRUE(paths.has_value()) << paths.error().message;
    EXPECT_FALSE(paths->tokenizer_path.empty());

    // Load the tokenizer config and verify type.
    auto config = load_tokenizer_config(paths->tokenizer_path);
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->model_type, TokenizerModelType::WORDPIECE);

    // Create a WordPiece tokenizer from the config and use it with the provider.
    auto tokenizer = std::make_unique<WordPieceTokenizer>(*config);
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(384, 0.5F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider(model_dir.string(), 384, std::move(mock), std::move(tokenizer));
    auto result = provider.embed("hello world");

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(mock_ptr->run_call_count_, 1);
    // Verify the real tokenizer was used (vocab_size matches config).
    EXPECT_EQ(provider.tokenizer().vocab_size(), 10u);
}

TEST_F(OnnxAutoDiscovery, ProviderWithBPETokenizerDirectory) {
    auto model_dir = base_dir_ / "nomic";
    write_file(model_dir / "model.onnx", "fake_model_data");
    write_bpe_tokenizer_json(model_dir / "tokenizer.json");

    auto paths = resolve_onnx_model_paths(model_dir.string());
    ASSERT_TRUE(paths.has_value()) << paths.error().message;

    auto config = load_tokenizer_config(paths->tokenizer_path);
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->model_type, TokenizerModelType::BPE);

    auto tokenizer = std::make_unique<BPETokenizer>(*config);
    auto mock = std::make_unique<MockOnnxSession>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(384, 0.5F);
    mock_ptr->set_embedding(emb);

    OnnxProvider provider(model_dir.string(), 384, std::move(mock), std::move(tokenizer));
    auto result = provider.embed("hello");

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(mock_ptr->run_call_count_, 1);
    EXPECT_EQ(provider.tokenizer().vocab_size(), 11u);
}
