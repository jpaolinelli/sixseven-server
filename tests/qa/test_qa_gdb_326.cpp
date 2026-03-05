/// @file test_qa_gdb_326.cpp
/// @brief QA adversarial tests for GDB-326: End-to-End ONNX Embeddings.
///
/// Tests edge cases, boundary conditions, numerical stability,
/// and additional semantic validation for the ONNX embedding pipeline.
/// Skips automatically when the model fixture is not present.

#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace sixseven {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

fs::path fixtures_dir() {
    return fs::path(__FILE__).parent_path().parent_path() / "fixtures";
}

fs::path model_dir() {
    return fixtures_dir() / "models" / "all-MiniLM-L6-v2";
}

fs::path reference_json_path() {
    return fixtures_dir() / "reference_embeddings.json";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    double denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    if (denom == 0.0) {
        return 0.0;
    }
    return dot / denom;
}

double l2_norm(const std::vector<float>& v) {
    double sum = 0.0;
    for (auto x : v) {
        sum += static_cast<double>(x) * static_cast<double>(x);
    }
    return std::sqrt(sum);
}

json load_reference_embeddings() {
    auto path = reference_json_path();
    std::ifstream f(path);
    if (!f.is_open()) {
        return json{};
    }
    return json::parse(f);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB326 : public ::testing::Test {
protected:
    void SetUp() override {
        auto dir = model_dir();
        if (!fs::exists(dir / "model.onnx") || !fs::exists(dir / "tokenizer.json")) {
            GTEST_SKIP() << "Model fixture not present. Run: "
                            "./tests/fixtures/download_models.sh";
        }
        auto result = create_onnx_provider(dir.string(), kDim);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        provider_ = std::move(*result);
    }

    static constexpr size_t kDim = 384;
    std::unique_ptr<OnnxProvider> provider_;
};

// ===========================================================================
// Edge case text inputs
// ===========================================================================

/// Single character should produce a valid embedding.
TEST_F(QA_GDB326, GDB326_SingleCharInput) {
    auto result = provider_->embed("a");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
    EXPECT_GT(l2_norm(*result), 0.0) << "Embedding should be non-zero";
}

/// Numeric-only text should produce a valid embedding.
TEST_F(QA_GDB326, GDB326_NumericOnlyText) {
    auto result = provider_->embed("12345");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
    EXPECT_GT(l2_norm(*result), 0.0);
}

/// Punctuation-heavy text should produce a valid embedding.
TEST_F(QA_GDB326, GDB326_PunctuationHeavyText) {
    auto result = provider_->embed("!@#$%^&*()");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

/// Unicode text should not crash the pipeline.
TEST_F(QA_GDB326, GDB326_UnicodeText) {
    auto result = provider_->embed("café résumé naïve");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

/// CJK characters should produce a valid embedding.
TEST_F(QA_GDB326, GDB326_CJKText) {
    auto result = provider_->embed("你好世界");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

/// Very long text should be handled (truncation expected).
TEST_F(QA_GDB326, GDB326_VeryLongText) {
    std::string long_text;
    for (int i = 0; i < 500; ++i) {
        long_text += "word" + std::to_string(i) + " ";
    }
    auto result = provider_->embed(long_text);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

/// Whitespace-only text should be handled gracefully.
/// The provider rejects empty text, but whitespace-only is not empty.
TEST_F(QA_GDB326, GDB326_WhitespaceOnlyText) {
    auto result = provider_->embed("   \t\n   ");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

/// Text with embedded null-like characters should not crash.
TEST_F(QA_GDB326, GDB326_SpecialCharacterText) {
    auto result = provider_->embed("hello\x01world");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

/// Repeated words should produce a valid embedding distinct from single occurrence.
TEST_F(QA_GDB326, GDB326_RepeatedWords) {
    auto single = provider_->embed("hello");
    auto repeated = provider_->embed("hello hello hello hello hello");
    ASSERT_TRUE(single.has_value()) << single.error().message;
    ASSERT_TRUE(repeated.has_value()) << repeated.error().message;

    // Should be related but not identical (context differs).
    double sim = cosine_similarity(*single, *repeated);
    EXPECT_GT(sim, 0.5) << "Repeated word should still be similar to single";
    EXPECT_LT(sim, 1.0) << "Repeated word should not be identical to single";
}

// ===========================================================================
// Numerical stability and vector properties
// ===========================================================================

/// All embeddings should have finite (non-NaN, non-Inf) components.
TEST_F(QA_GDB326, GDB326_EmbeddingComponentsAreFinite) {
    std::vector<std::string> texts = {
        "hello",
        "The quick brown fox jumps over the lazy dog.",
        "machine learning",
        "!@#$%",
        "12345",
    };
    for (const auto& text : texts) {
        SCOPED_TRACE(text);
        auto result = provider_->embed(text);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        for (size_t i = 0; i < result->size(); ++i) {
            EXPECT_TRUE(std::isfinite((*result)[i]))
                << "Component " << i << " is not finite: " << (*result)[i];
        }
    }
}

/// L2 norm of embeddings should be reasonable (not zero, not extremely large).
TEST_F(QA_GDB326, GDB326_EmbeddingNormReasonable) {
    auto result = provider_->embed("The quick brown fox jumps over the lazy dog.");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    double norm = l2_norm(*result);
    EXPECT_GT(norm, 0.1) << "L2 norm should be positive";
    EXPECT_LT(norm, 100.0) << "L2 norm should not be enormous";
}

/// Cross-check L2 norm against reference data.
TEST_F(QA_GDB326, GDB326_L2NormMatchesReference) {
    auto ref = load_reference_embeddings();
    if (ref.empty()) {
        GTEST_SKIP() << "Reference embeddings file not loaded";
    }

    const std::string text = "The quick brown fox jumps over the lazy dog.";
    auto result = provider_->embed(text);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    double computed_norm = l2_norm(*result);
    double ref_norm = ref.at(text).at("l2_norm").get<double>();

    // Allow some tolerance for floating-point differences.
    EXPECT_NEAR(computed_norm, ref_norm, 0.5) << "L2 norm diverges significantly from reference";
}

// ===========================================================================
// Additional semantic similarity tests
// ===========================================================================

/// Semantically related domain terms should cluster together.
TEST_F(QA_GDB326, GDB326_DomainSemanticClustering) {
    auto dog = provider_->embed("dog");
    auto cat = provider_->embed("cat");
    auto car = provider_->embed("car");
    ASSERT_TRUE(dog.has_value()) << dog.error().message;
    ASSERT_TRUE(cat.has_value()) << cat.error().message;
    ASSERT_TRUE(car.has_value()) << car.error().message;

    double sim_dog_cat = cosine_similarity(*dog, *cat);
    double sim_dog_car = cosine_similarity(*dog, *car);

    EXPECT_GT(sim_dog_cat, sim_dog_car)
        << "dog-cat (" << sim_dog_cat << ") should be > dog-car (" << sim_dog_car << ")";
}

/// Sentence-level semantic similarity should work.
TEST_F(QA_GDB326, GDB326_SentenceSemanticSimilarity) {
    auto sent1 = provider_->embed("The weather is beautiful today");
    auto sent2 = provider_->embed("It is a lovely day outside");
    auto sent3 = provider_->embed("I need to fix the database bug");
    ASSERT_TRUE(sent1.has_value()) << sent1.error().message;
    ASSERT_TRUE(sent2.has_value()) << sent2.error().message;
    ASSERT_TRUE(sent3.has_value()) << sent3.error().message;

    double sim_related = cosine_similarity(*sent1, *sent2);
    double sim_unrelated = cosine_similarity(*sent1, *sent3);

    EXPECT_GT(sim_related, sim_unrelated)
        << "Similar sentences (" << sim_related << ") should score higher than unrelated ("
        << sim_unrelated << ")";
}

/// Antonyms should be less similar than synonyms.
TEST_F(QA_GDB326, GDB326_AntonymVsSynonym) {
    auto happy = provider_->embed("happy");
    auto joyful = provider_->embed("joyful");
    auto sad = provider_->embed("sad");
    ASSERT_TRUE(happy.has_value()) << happy.error().message;
    ASSERT_TRUE(joyful.has_value()) << joyful.error().message;
    ASSERT_TRUE(sad.has_value()) << sad.error().message;

    double sim_synonym = cosine_similarity(*happy, *joyful);
    double sim_antonym = cosine_similarity(*happy, *sad);

    EXPECT_GT(sim_synonym, sim_antonym)
        << "happy-joyful (" << sim_synonym << ") should be > happy-sad (" << sim_antonym << ")";
}

// ===========================================================================
// Determinism stress
// ===========================================================================

/// Determinism across many iterations with varied inputs.
TEST_F(QA_GDB326, GDB326_DeterminismStress) {
    std::vector<std::string> texts = {
        "short",
        "a somewhat longer sentence with multiple words",
        "machine learning is a subset of artificial intelligence",
    };

    for (const auto& text : texts) {
        SCOPED_TRACE(text);
        auto baseline = provider_->embed(text);
        ASSERT_TRUE(baseline.has_value()) << baseline.error().message;

        for (int i = 0; i < 3; ++i) {
            auto again = provider_->embed(text);
            ASSERT_TRUE(again.has_value()) << again.error().message;
            ASSERT_EQ(baseline->size(), again->size());
            for (size_t j = 0; j < baseline->size(); ++j) {
                EXPECT_EQ((*baseline)[j], (*again)[j])
                    << "Mismatch at index " << j << " for \"" << text << "\" iter " << i;
            }
        }
    }
}

// ===========================================================================
// Batch embedding edge cases
// ===========================================================================

/// Batch with single element should match individual call.
TEST_F(QA_GDB326, GDB326_BatchSingleElement) {
    auto single = provider_->embed("hello world");
    ASSERT_TRUE(single.has_value()) << single.error().message;

    auto batch = provider_->embed_batch({"hello world"});
    ASSERT_TRUE(batch.has_value()) << batch.error().message;
    ASSERT_EQ(batch->size(), 1u);

    double sim = cosine_similarity(*single, (*batch)[0]);
    EXPECT_GT(sim, 0.9999) << "Single vs batch[0] mismatch: cosine = " << sim;
}

/// Batch with duplicate texts should return identical embeddings.
TEST_F(QA_GDB326, GDB326_BatchDuplicates) {
    auto batch = provider_->embed_batch({"test", "test", "test"});
    ASSERT_TRUE(batch.has_value()) << batch.error().message;
    ASSERT_EQ(batch->size(), 3u);

    for (size_t i = 1; i < batch->size(); ++i) {
        ASSERT_EQ((*batch)[0].size(), (*batch)[i].size());
        for (size_t j = 0; j < (*batch)[0].size(); ++j) {
            EXPECT_EQ((*batch)[0][j], (*batch)[i][j])
                << "Duplicate mismatch at batch[" << i << "][" << j << "]";
        }
    }
}

/// Empty batch should return empty result.
TEST_F(QA_GDB326, GDB326_BatchEmpty) {
    auto batch = provider_->embed_batch({});
    ASSERT_TRUE(batch.has_value()) << batch.error().message;
    EXPECT_TRUE(batch->empty());
}

/// Batch with empty string should fail.
TEST_F(QA_GDB326, GDB326_BatchWithEmptyString) {
    auto batch = provider_->embed_batch({"hello", "", "world"});
    ASSERT_FALSE(batch.has_value());
    EXPECT_EQ(batch.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// Error path verification
// ===========================================================================

/// Embedding empty text should return INVALID_ARGUMENT.
TEST_F(QA_GDB326, GDB326_EmptyTextError) {
    auto result = provider_->embed("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

/// Health check should pass after successful embedding calls.
TEST_F(QA_GDB326, GDB326_HealthCheckAfterUsage) {
    auto emb = provider_->embed("test");
    ASSERT_TRUE(emb.has_value()) << emb.error().message;

    auto health = provider_->health_check();
    EXPECT_TRUE(health.has_value()) << health.error().message;
}

// ===========================================================================
// Reference embedding integrity
// ===========================================================================

/// All reference sentences should exist in the JSON and have correct dimension.
TEST_F(QA_GDB326, GDB326_ReferenceDataIntegrity) {
    auto ref = load_reference_embeddings();
    if (ref.empty()) {
        GTEST_SKIP() << "Reference embeddings file not loaded";
    }

    std::vector<std::string> expected_keys = {
        "The quick brown fox jumps over the lazy dog.",
        "king",
        "queen",
        "bicycle",
        "machine learning is a subset of artificial intelligence",
    };

    for (const auto& key : expected_keys) {
        SCOPED_TRACE(key);
        ASSERT_TRUE(ref.contains(key)) << "Missing reference key: " << key;
        ASSERT_TRUE(ref.at(key).contains("full")) << "Missing 'full' vector for: " << key;

        auto& full = ref.at(key).at("full");
        EXPECT_EQ(full.size(), kDim) << "Reference vector dimension mismatch for: " << key;
    }
}

/// Reference first_20 values should match the first 20 of the full vector.
TEST_F(QA_GDB326, GDB326_ReferenceFirst20Consistency) {
    auto ref = load_reference_embeddings();
    if (ref.empty()) {
        GTEST_SKIP() << "Reference embeddings file not loaded";
    }

    for (auto& [key, val] : ref.items()) {
        SCOPED_TRACE(key);
        if (!val.contains("first_20") || !val.contains("full")) {
            continue;
        }
        auto& first_20 = val.at("first_20");
        auto& full = val.at("full");

        ASSERT_GE(full.size(), 20u);
        ASSERT_EQ(first_20.size(), 20u);

        for (size_t i = 0; i < 20; ++i) {
            EXPECT_FLOAT_EQ(first_20[i].get<float>(), full[i].get<float>())
                << "first_20[" << i << "] != full[" << i << "] for key: " << key;
        }
    }
}

// ===========================================================================
// Provider properties
// ===========================================================================

/// Dimension accessor should match expected dim.
TEST_F(QA_GDB326, GDB326_DimensionAccessor) {
    EXPECT_EQ(provider_->dimension(), kDim);
}

/// Provider name should contain "onnx".
TEST_F(QA_GDB326, GDB326_ProviderNameContainsOnnx) {
    std::string name = provider_->name();
    EXPECT_NE(name.find("onnx"), std::string::npos)
        << "Provider name should contain 'onnx', got: " << name;
}

// ===========================================================================
// Stress test — many sequential embeddings
// ===========================================================================

/// Run many embeddings sequentially without error or degradation.
TEST_F(QA_GDB326, GDB326_SequentialStress) {
    const int iterations = 50;
    auto baseline = provider_->embed("stress test sentence");
    ASSERT_TRUE(baseline.has_value()) << baseline.error().message;

    for (int i = 0; i < iterations; ++i) {
        auto result = provider_->embed("stress test sentence " + std::to_string(i));
        ASSERT_TRUE(result.has_value())
            << "Failed at iteration " << i << ": " << result.error().message;
        EXPECT_EQ(result->size(), kDim) << "Wrong dim at iteration " << i;
    }
}

// ===========================================================================
// create_onnx_provider error paths
// ===========================================================================

/// Creating provider with non-existent path should fail.
TEST(QA_GDB326_NoFixture, GDB326_CreateProviderBadPath) {
    auto result = create_onnx_provider("/nonexistent/path/to/model", 384);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

/// Creating provider with wrong dimension should still create successfully
/// (dimension mismatch is caught at inference time, not creation time).
TEST_F(QA_GDB326, GDB326_WrongDimensionAtCreation) {
    auto dir = model_dir();
    auto result = create_onnx_provider(dir.string(), 999);
    // Provider creation should succeed — dimension mismatch is detected at embed time.
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // But embedding should fail with dimension mismatch.
    auto emb = (*result)->embed("test");
    ASSERT_FALSE(emb.has_value());
    EXPECT_EQ(emb.error().code, StatusCode::INTERNAL_ERROR);
}

} // namespace
} // namespace sixseven
