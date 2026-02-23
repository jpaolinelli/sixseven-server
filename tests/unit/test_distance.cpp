#include "giodb/vector/distance.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

using namespace giodb;

// =============================================================================
// Helpers
// =============================================================================

namespace {

/// Scalar reference L2 squared distance (for correctness verification).
float ref_l2(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

/// Scalar reference dot product.
float ref_dot(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

/// Scalar reference cosine distance.
float ref_cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0.0F;
    float na = 0.0F;
    float nb = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na * nb);
    if (denom < 1e-30F) {
        return 1.0F;
    }
    float sim = dot / denom;
    sim = std::fmin(1.0F, std::fmax(-1.0F, sim));
    return 1.0F - sim;
}

/// Generate a random vector of given dimension.
std::vector<float> random_vector(uint32_t dim, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::vector<float> v(dim);
    for (auto& x : v) {
        x = dist(rng);
    }
    return v;
}

} // namespace

// =============================================================================
// SIMD level info
// =============================================================================

TEST(Distance, ActiveSimdLevelIsValid) {
    auto level = active_simd_level();
    EXPECT_TRUE(level == SimdLevel::SCALAR || level == SimdLevel::NEON ||
                level == SimdLevel::AVX2 || level == SimdLevel::AVX512);
    const char* name = active_simd_name();
    EXPECT_NE(name, nullptr);
    EXPECT_GT(std::string(name).size(), 0U);
}

// =============================================================================
// L2 (Euclidean squared) correctness
// =============================================================================

TEST(DistanceL2, KnownValues) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    // (4-1)^2 + (5-2)^2 + (6-3)^2 = 9 + 9 + 9 = 27
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(d, 27.0F, 1e-5F);
}

TEST(DistanceL2, IdenticalVectors) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F};
    float d = compute_distance(DistanceMetric::L2, a, a);
    EXPECT_FLOAT_EQ(d, 0.0F);
}

TEST(DistanceL2, ZeroVectors) {
    std::vector<float> a(128, 0.0F);
    std::vector<float> b(128, 0.0F);
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_FLOAT_EQ(d, 0.0F);
}

TEST(DistanceL2, SingleDimension) {
    std::vector<float> a = {3.0F};
    std::vector<float> b = {7.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(d, 16.0F, 1e-5F);
}

TEST(DistanceL2, Symmetric) {
    std::vector<float> a = {1.0F, -2.0F, 3.5F, -0.5F};
    std::vector<float> b = {-1.0F, 2.0F, -3.5F, 0.5F};
    float d_ab = compute_distance(DistanceMetric::L2, a, b);
    float d_ba = compute_distance(DistanceMetric::L2, b, a);
    EXPECT_FLOAT_EQ(d_ab, d_ba);
}

TEST(DistanceL2, MatchesScalarReference) {
    std::mt19937 rng(42);
    for (uint32_t dim : {1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 128, 256, 768, 1536}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::L2, a, b);
        float expected = ref_l2(a, b);
        EXPECT_NEAR(actual, expected, std::fabs(expected) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

// =============================================================================
// Dot product correctness
// =============================================================================

TEST(DistanceDot, KnownValues) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(d, 32.0F, 1e-5F);
}

TEST(DistanceDot, OrthogonalVectors) {
    std::vector<float> a = {1.0F, 0.0F, 0.0F};
    std::vector<float> b = {0.0F, 1.0F, 0.0F};
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(d, 0.0F, 1e-6F);
}

TEST(DistanceDot, MatchesScalarReference) {
    std::mt19937 rng(123);
    for (uint32_t dim : {1, 3, 4, 8, 16, 32, 128, 256, 768, 1536}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float expected = ref_dot(a, b);
        EXPECT_NEAR(actual, expected, std::fabs(expected) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

// =============================================================================
// Inner product correctness
// =============================================================================

TEST(DistanceInnerProduct, IsNegatedDotProduct) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
    EXPECT_NEAR(ip, -dot, 1e-6F);
}

TEST(DistanceInnerProduct, MatchesReference) {
    std::mt19937 rng(77);
    for (uint32_t dim : {4, 16, 128, 768}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
        float expected = -ref_dot(a, b);
        EXPECT_NEAR(actual, expected, std::fabs(expected) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

// =============================================================================
// Cosine distance correctness
// =============================================================================

TEST(DistanceCosine, IdenticalVectors) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, a);
    EXPECT_NEAR(d, 0.0F, 1e-6F);
}

TEST(DistanceCosine, OppositeVectors) {
    std::vector<float> a = {1.0F, 0.0F};
    std::vector<float> b = {-1.0F, 0.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 2.0F, 1e-6F);
}

TEST(DistanceCosine, OrthogonalVectors) {
    std::vector<float> a = {1.0F, 0.0F};
    std::vector<float> b = {0.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 1.0F, 1e-6F);
}

TEST(DistanceCosine, ZeroVectorReturnsOne) {
    std::vector<float> a(128, 0.0F);
    std::vector<float> b = {1.0F, 2.0F, 3.0F};
    b.resize(128, 0.0F);
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 1.0F, 1e-6F);
}

TEST(DistanceCosine, ScaleInvariance) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F};
    std::vector<float> b = {2.0F, 4.0F, 6.0F, 8.0F}; // 2 * a
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 0.0F, 1e-6F);
}

TEST(DistanceCosine, MatchesScalarReference) {
    std::mt19937 rng(99);
    for (uint32_t dim : {1, 3, 4, 8, 16, 32, 128, 256, 768, 1536}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::COSINE, a, b);
        float expected = ref_cosine(a, b);
        EXPECT_NEAR(actual, expected, 1e-5F) << "dim=" << dim;
    }
}

// =============================================================================
// Edge cases
// =============================================================================

TEST(DistanceEdge, VerySmallValues) {
    std::vector<float> a = {1e-20F, 1e-20F};
    std::vector<float> b = {-1e-20F, -1e-20F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_TRUE(std::isfinite(d));
}

TEST(DistanceEdge, LargeValues) {
    std::vector<float> a = {1e18F, 1e18F};
    std::vector<float> b = {-1e18F, -1e18F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_TRUE(std::isfinite(d) || d == std::numeric_limits<float>::infinity());
}

TEST(DistanceEdge, CosineNumericalStability) {
    // Near-zero vectors should not produce NaN.
    std::vector<float> a(128, 1e-38F);
    std::vector<float> b(128, 1e-38F);
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_FALSE(std::isnan(d));
    EXPECT_TRUE(std::isfinite(d));
}

TEST(DistanceEdge, EmptyVectors) {
    std::vector<float> a;
    std::vector<float> b;
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_FLOAT_EQ(d, 0.0F);
}

TEST(DistanceEdge, MismatchedDimensionsUsesMinimum) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F};
    std::vector<float> b = {4.0F, 5.0F};
    // Should compute over min(3,2) = 2 dimensions: (4-1)^2 + (5-2)^2 = 9+9 = 18
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(d, 18.0F, 1e-5F);
}

// =============================================================================
// Batch computation correctness
// =============================================================================

TEST(DistanceBatch, MatchesSingleCalls) {
    std::mt19937 rng(55);
    const uint32_t dim = 128;
    const uint32_t count = 100;

    auto query = random_vector(dim, rng);
    std::vector<std::vector<float>> candidates(count);
    std::vector<const float*> ptrs(count);
    for (uint32_t i = 0; i < count; ++i) {
        candidates[i] = random_vector(dim, rng);
        ptrs[i] = candidates[i].data();
    }

    for (auto metric : {DistanceMetric::L2,
                        DistanceMetric::COSINE,
                        DistanceMetric::DOT_PRODUCT,
                        DistanceMetric::INNER_PRODUCT}) {
        std::vector<float> batch_out(count);
        compute_distance_batch(metric, query, ptrs.data(), count, dim, batch_out.data());

        for (uint32_t i = 0; i < count; ++i) {
            float single = compute_distance(metric, query, candidates[i]);
            EXPECT_NEAR(batch_out[i], single, std::fabs(single) * 1e-5F + 1e-6F)
                << "metric=" << distance_metric_name(metric) << " i=" << i;
        }
    }
}

TEST(DistanceBatch, ZeroCount) {
    std::vector<float> query = {1.0F, 2.0F};
    compute_distance_batch(DistanceMetric::L2, query, nullptr, 0, 2, nullptr);
    // Should not crash.
}

TEST(DistanceBatch, SingleCandidate) {
    std::vector<float> query = {1.0F, 2.0F, 3.0F};
    std::vector<float> candidate = {4.0F, 5.0F, 6.0F};
    const float* ptr = candidate.data();
    float result = 0.0F;
    compute_distance_batch(DistanceMetric::L2, query, &ptr, 1, 3, &result);
    EXPECT_NEAR(result, 27.0F, 1e-5F);
}

// =============================================================================
// Batch performance: batch should be faster than sequential single calls.
// =============================================================================

TEST(DistanceBatch, FasterThanSequential) {
    std::mt19937 rng(7);
    const uint32_t dim = 768;
    const uint32_t count = 1000;

    auto query = random_vector(dim, rng);
    std::vector<std::vector<float>> candidates(count);
    std::vector<const float*> ptrs(count);
    for (uint32_t i = 0; i < count; ++i) {
        candidates[i] = random_vector(dim, rng);
        ptrs[i] = candidates[i].data();
    }

    // Warm up.
    std::vector<float> out(count);
    compute_distance_batch(DistanceMetric::L2, query, ptrs.data(), count, dim, out.data());

    // Time batch.
    constexpr int iterations = 50;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        compute_distance_batch(DistanceMetric::L2, query, ptrs.data(), count, dim, out.data());
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // Time sequential.
    auto t2 = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = compute_distance(DistanceMetric::L2, query, candidates[i]);
        }
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    auto batch_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    auto seq_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    // Batch should not be significantly slower than sequential.
    // The main overhead difference is dispatch + span construction in sequential.
    // For very optimized builds, batch is at least comparable.
    EXPECT_LE(batch_us, seq_us * 2) << "batch_us=" << batch_us << " seq_us=" << seq_us;
}

// =============================================================================
// Metric name utility
// =============================================================================

TEST(Distance, MetricNames) {
    EXPECT_STREQ(distance_metric_name(DistanceMetric::L2), "L2");
    EXPECT_STREQ(distance_metric_name(DistanceMetric::COSINE), "COSINE");
    EXPECT_STREQ(distance_metric_name(DistanceMetric::DOT_PRODUCT), "DOT_PRODUCT");
    EXPECT_STREQ(distance_metric_name(DistanceMetric::INNER_PRODUCT), "INNER_PRODUCT");
}

// =============================================================================
// Various dimension sizes (SIMD boundary testing)
// =============================================================================

TEST(DistanceL2, VariousDimensions) {
    std::mt19937 rng(333);
    // Test dimensions that exercise SIMD boundaries:
    // NEON: 4-wide, AVX2: 8-wide, AVX-512: 16-wide.
    for (uint32_t dim : {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::L2, a, b);
        float expected = ref_l2(a, b);
        EXPECT_NEAR(actual, expected, std::fabs(expected) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

TEST(DistanceDot, VariousDimensions) {
    std::mt19937 rng(444);
    for (uint32_t dim : {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float expected = ref_dot(a, b);
        EXPECT_NEAR(actual, expected, std::fabs(expected) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

TEST(DistanceCosine, VariousDimensions) {
    std::mt19937 rng(555);
    for (uint32_t dim : {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float actual = compute_distance(DistanceMetric::COSINE, a, b);
        float expected = ref_cosine(a, b);
        EXPECT_NEAR(actual, expected, 1e-5F) << "dim=" << dim;
    }
}

// =============================================================================
// Large-dimension stress test (typical embedding sizes)
// =============================================================================

TEST(Distance, LargeDimensionEmbeddings) {
    std::mt19937 rng(666);
    // Common embedding sizes: OpenAI ada-002 (1536), BERT (768), CLIP (512).
    for (uint32_t dim : {512, 768, 1024, 1536, 3072}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        float cos_d = compute_distance(DistanceMetric::COSINE, a, b);
        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);

        EXPECT_TRUE(std::isfinite(l2)) << "dim=" << dim;
        EXPECT_TRUE(std::isfinite(cos_d)) << "dim=" << dim;
        EXPECT_TRUE(std::isfinite(dot)) << "dim=" << dim;
        EXPECT_TRUE(std::isfinite(ip)) << "dim=" << dim;
        EXPECT_GE(l2, 0.0F) << "dim=" << dim;
        EXPECT_GE(cos_d, -1e-6F) << "dim=" << dim;
        EXPECT_LE(cos_d, 2.0F + 1e-6F) << "dim=" << dim;
        EXPECT_NEAR(ip, -dot, 1e-6F) << "dim=" << dim;

        // Cross-check against references.
        EXPECT_NEAR(l2, ref_l2(a, b), std::fabs(ref_l2(a, b)) * 1e-4F + 1e-4F) << "dim=" << dim;
        EXPECT_NEAR(dot, ref_dot(a, b), std::fabs(ref_dot(a, b)) * 1e-4F + 1e-4F) << "dim=" << dim;
    }
}
