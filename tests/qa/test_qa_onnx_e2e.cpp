/// @file test_qa_onnx_e2e.cpp
/// @brief GDB-326: End-to-end ONNX embedding quality test.
///
/// Loads the real all-MiniLM-L6-v2 ONNX model + tokenizer, generates
/// embeddings, and validates against Python reference vectors.
/// Skips automatically when the model fixture is not present.

#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
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

/// Cosine similarity between two equal-length vectors.
double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

/// Load reference embeddings from JSON fixture.
json load_reference_embeddings() {
    std::ifstream f(reference_json_path());
    return json::parse(f);
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class OnnxE2E : public ::testing::Test {
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// AC: Generates embeddings for test sentences and compares against
///     Python reference vectors (cosine similarity > 0.99).
TEST_F(OnnxE2E, GDB326_ReferenceVectorComparison) {
    auto ref = load_reference_embeddings();

    std::vector<std::string> sentences = {
        "The quick brown fox jumps over the lazy dog.",
        "king",
        "queen",
        "bicycle",
        "machine learning is a subset of artificial intelligence",
    };

    for (const auto& sent : sentences) {
        SCOPED_TRACE(sent);
        auto result = provider_->embed(sent);
        ASSERT_TRUE(result.has_value()) << result.error().message;

        const auto& emb = *result;
        ASSERT_EQ(emb.size(), kDim);

        // Load reference full vector from JSON.
        auto ref_vec_json = ref.at(sent).at("full");
        std::vector<float> ref_vec;
        ref_vec.reserve(kDim);
        for (const auto& v : ref_vec_json) {
            ref_vec.push_back(v.get<float>());
        }
        ASSERT_EQ(ref_vec.size(), kDim);

        double sim = cosine_similarity(emb, ref_vec);
        EXPECT_GT(sim, 0.99) << "Cosine similarity for \"" << sent << "\" = " << sim
                             << " (expected > 0.99)";
    }
}

/// AC: Semantic similarity — cosine("king","queen") > cosine("king","bicycle").
TEST_F(OnnxE2E, GDB326_SemanticSimilarity) {
    auto king = provider_->embed("king");
    auto queen = provider_->embed("queen");
    auto bicycle = provider_->embed("bicycle");

    ASSERT_TRUE(king.has_value()) << king.error().message;
    ASSERT_TRUE(queen.has_value()) << queen.error().message;
    ASSERT_TRUE(bicycle.has_value()) << bicycle.error().message;

    double sim_king_queen = cosine_similarity(*king, *queen);
    double sim_king_bicycle = cosine_similarity(*king, *bicycle);

    EXPECT_GT(sim_king_queen, sim_king_bicycle)
        << "cosine(king, queen) = " << sim_king_queen
        << " should be > cosine(king, bicycle) = " << sim_king_bicycle;

    // Sanity: king-queen similarity should be meaningfully positive.
    EXPECT_GT(sim_king_queen, 0.5);
}

/// AC: Determinism — same input always produces identical output.
TEST_F(OnnxE2E, GDB326_Determinism) {
    const std::string text = "The quick brown fox jumps over the lazy dog.";

    auto first = provider_->embed(text);
    ASSERT_TRUE(first.has_value()) << first.error().message;

    // Run multiple times and compare bit-for-bit.
    for (int i = 0; i < 5; ++i) {
        SCOPED_TRACE("iteration " + std::to_string(i));
        auto again = provider_->embed(text);
        ASSERT_TRUE(again.has_value()) << again.error().message;
        ASSERT_EQ(first->size(), again->size());

        for (size_t j = 0; j < first->size(); ++j) {
            EXPECT_EQ((*first)[j], (*again)[j])
                << "Mismatch at index " << j << " on iteration " << i;
        }
    }
}

/// AC: Batch embedding produces same results as individual calls.
TEST_F(OnnxE2E, GDB326_BatchConsistency) {
    std::vector<std::string> texts = {"king", "queen", "bicycle"};

    auto batch = provider_->embed_batch(texts);
    ASSERT_TRUE(batch.has_value()) << batch.error().message;
    ASSERT_EQ(batch->size(), texts.size());

    for (size_t i = 0; i < texts.size(); ++i) {
        SCOPED_TRACE(texts[i]);
        auto single = provider_->embed(texts[i]);
        ASSERT_TRUE(single.has_value()) << single.error().message;

        double sim = cosine_similarity((*batch)[i], *single);
        EXPECT_GT(sim, 0.9999) << "Batch vs single mismatch for \"" << texts[i]
                               << "\": cosine = " << sim;
    }
}

/// AC: Health check passes on a loaded model.
TEST_F(OnnxE2E, GDB326_HealthCheck) {
    auto result = provider_->health_check();
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

/// AC: Output dimension is correct.
TEST_F(OnnxE2E, GDB326_OutputDimension) {
    EXPECT_EQ(provider_->dimension(), kDim);

    auto result = provider_->embed("test");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), kDim);
}

} // namespace
} // namespace sixseven
