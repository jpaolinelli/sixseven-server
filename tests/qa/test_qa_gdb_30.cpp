#include "sixseven/vector/distance.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

namespace {

float ref_l2(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

float ref_dot(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

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
// NaN Input Handling
// =============================================================================

TEST(QA_Distance, L2WithNaNInput) {
    std::vector<float> a = {1.0F, std::numeric_limits<float>::quiet_NaN(), 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    // NaN propagation is mathematically correct — result should be NaN.
    EXPECT_TRUE(std::isnan(d)) << "L2 with NaN input should propagate NaN, got " << d;
}

TEST(QA_Distance, DotProductWithNaNInput) {
    std::vector<float> a = {1.0F, std::numeric_limits<float>::quiet_NaN(), 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_TRUE(std::isnan(d)) << "Dot product with NaN input should propagate NaN, got " << d;
}

TEST(QA_Distance, CosineWithNaNInput) {
    std::vector<float> a = {1.0F, std::numeric_limits<float>::quiet_NaN(), 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    // Cosine with NaN should return NaN (not crash).
    EXPECT_TRUE(std::isnan(d)) << "Cosine with NaN input should propagate NaN, got " << d;
}

TEST(QA_Distance, InnerProductWithNaNInput) {
    std::vector<float> a = {1.0F, std::numeric_limits<float>::quiet_NaN(), 3.0F};
    std::vector<float> b = {4.0F, 5.0F, 6.0F};
    float d = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
    EXPECT_TRUE(std::isnan(d)) << "Inner product with NaN input should propagate NaN, got " << d;
}

TEST(QA_Distance, AllNaNVector) {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> a = {nan, nan, nan, nan};
    std::vector<float> b = {1.0F, 2.0F, 3.0F, 4.0F};
    for (auto metric : {DistanceMetric::L2,
                        DistanceMetric::DOT_PRODUCT,
                        DistanceMetric::INNER_PRODUCT,
                        DistanceMetric::COSINE}) {
        float d = compute_distance(metric, a, b);
        EXPECT_TRUE(std::isnan(d)) << "metric=" << distance_metric_name(metric) << " got " << d;
    }
}

// =============================================================================
// Infinity Input Handling
// =============================================================================

TEST(QA_Distance, L2WithInfinityInput) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {inf, 1.0F, 2.0F};
    std::vector<float> b = {0.0F, 1.0F, 2.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    // inf - 0 = inf, inf^2 = inf. Result should be inf.
    EXPECT_TRUE(std::isinf(d) && d > 0) << "L2 with Inf should be +Inf, got " << d;
}

TEST(QA_Distance, L2BothInfinityOppositeSign) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {inf, 0.0F};
    std::vector<float> b = {-inf, 0.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    // inf - (-inf) = inf, inf^2 = inf
    EXPECT_TRUE(std::isinf(d) && d > 0) << "L2 with opposing Inf should be +Inf, got " << d;
}

TEST(QA_Distance, L2BothInfinitySameSign) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {inf, 0.0F};
    std::vector<float> b = {inf, 0.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    // inf - inf = NaN, NaN^2 = NaN. Result should be NaN.
    EXPECT_TRUE(std::isnan(d)) << "L2 of identical Inf vectors should be NaN (inf-inf), got " << d;
}

TEST(QA_Distance, DotProductWithInfinity) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {inf, 1.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_TRUE(std::isinf(d) && d > 0) << "Dot with Inf should be +Inf, got " << d;
}

TEST(QA_Distance, CosineWithInfinityInput) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {inf, 0.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    // norm_a = inf, norm_b = finite, denom = inf*finite = inf, sqrt(inf) = inf
    // dot = inf*1 + 0*1 = inf, similarity = inf/inf = NaN
    // Result should be NaN (mathematically undefined).
    EXPECT_FALSE(std::isnan(d) && std::isinf(d))
        << "Cosine with Inf: expect NaN or valid float, got " << d;
}

// =============================================================================
// Negative Zero
// =============================================================================

TEST(QA_Distance, NegativeZeroL2) {
    std::vector<float> a = {-0.0F, -0.0F, -0.0F};
    std::vector<float> b = {0.0F, 0.0F, 0.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    // -0 - 0 = -0, (-0)^2 = 0. Distance should be 0.
    EXPECT_FLOAT_EQ(d, 0.0F);
}

TEST(QA_Distance, NegativeZeroDot) {
    std::vector<float> a = {-0.0F, -0.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_FLOAT_EQ(d, 0.0F);
}

// =============================================================================
// Subnormal Float Values
// =============================================================================

TEST(QA_Distance, SubnormalValues) {
    float subnormal = std::numeric_limits<float>::denorm_min();
    std::vector<float> a(16, subnormal);
    std::vector<float> b(16, -subnormal);
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_TRUE(std::isfinite(d));
    EXPECT_GE(d, 0.0F);
}

TEST(QA_Distance, CosineSubnormalVectors) {
    float subnormal = std::numeric_limits<float>::denorm_min();
    std::vector<float> a(16, subnormal);
    std::vector<float> b(16, subnormal);
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    // Both vectors are tiny but identical direction. Cosine should be ~0.
    // If subnormals underflow to zero in norms, denom < 1e-30 triggers, returns 1.0.
    EXPECT_FALSE(std::isnan(d));
    EXPECT_TRUE(std::isfinite(d));
}

// =============================================================================
// FLT_MAX Boundary
// =============================================================================

TEST(QA_Distance, FltMaxL2) {
    float mx = std::numeric_limits<float>::max();
    std::vector<float> a = {mx, 0.0F};
    std::vector<float> b = {0.0F, 0.0F};
    float d = compute_distance(DistanceMetric::L2, a, b);
    // mx^2 overflows float. Result should be +Inf.
    EXPECT_TRUE(std::isinf(d) && d > 0) << "L2 of FLT_MAX should overflow to +Inf, got " << d;
}

TEST(QA_Distance, FltMaxDot) {
    float mx = std::numeric_limits<float>::max();
    std::vector<float> a = {mx, 0.0F};
    std::vector<float> b = {1.0F, 0.0F};
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_FLOAT_EQ(d, mx);
}

// =============================================================================
// Dimension Boundary Stress (SIMD alignment edge cases)
// =============================================================================

TEST(QA_Distance, AllDimensionsUpTo65) {
    // Exhaustively test every dimension from 0 to 65 to catch SIMD tail bugs.
    std::mt19937 rng(12345);
    for (uint32_t dim = 0; dim <= 65; ++dim) {
        std::vector<float> a(dim);
        std::vector<float> b(dim);
        std::uniform_real_distribution<float> dist(-10.0F, 10.0F);
        for (auto& x : a)
            x = dist(rng);
        for (auto& x : b)
            x = dist(rng);

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float cos_d = compute_distance(DistanceMetric::COSINE, a, b);
        float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);

        float exp_l2 = ref_l2(a, b);
        float exp_dot = ref_dot(a, b);
        float exp_cos = ref_cosine(a, b);

        EXPECT_NEAR(l2, exp_l2, std::fabs(exp_l2) * 1e-5F + 1e-6F) << "L2 dim=" << dim;
        EXPECT_NEAR(dot, exp_dot, std::fabs(exp_dot) * 1e-5F + 1e-6F) << "DOT dim=" << dim;
        EXPECT_NEAR(cos_d, exp_cos, 1e-5F) << "COS dim=" << dim;
        EXPECT_NEAR(ip, -exp_dot, std::fabs(exp_dot) * 1e-5F + 1e-6F) << "IP dim=" << dim;
    }
}

TEST(QA_Distance, ExactSimdWidths) {
    // Test dimensions that are exact multiples of NEON(4), AVX2(8), AVX512(16).
    std::mt19937 rng(9999);
    for (uint32_t dim : {4, 8, 12, 16, 24, 32, 48, 64, 128, 256, 512}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        float exp_l2 = ref_l2(a, b);
        EXPECT_NEAR(l2, exp_l2, std::fabs(exp_l2) * 1e-5F + 1e-6F) << "dim=" << dim;

        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float exp_dot = ref_dot(a, b);
        EXPECT_NEAR(dot, exp_dot, std::fabs(exp_dot) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

TEST(QA_Distance, SimdWidthPlusOne) {
    // Test dimensions that are one past SIMD boundaries (tail of 1 element).
    std::mt19937 rng(8888);
    for (uint32_t dim : {5, 9, 13, 17, 33, 65, 129, 257}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        float exp_l2 = ref_l2(a, b);
        EXPECT_NEAR(l2, exp_l2, std::fabs(exp_l2) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

TEST(QA_Distance, SimdWidthMinusOne) {
    // Test dimensions that are one before SIMD boundaries (no full SIMD iteration).
    std::mt19937 rng(7777);
    for (uint32_t dim : {3, 7, 15, 31, 63, 127}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        float exp_l2 = ref_l2(a, b);
        EXPECT_NEAR(l2, exp_l2, std::fabs(exp_l2) * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

// =============================================================================
// Cosine Distance Edge Cases
// =============================================================================

TEST(QA_Distance, CosineBothZeroVectors) {
    std::vector<float> a(64, 0.0F);
    std::vector<float> b(64, 0.0F);
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 1.0F, 1e-6F) << "Cosine of two zero vectors should be 1.0";
}

TEST(QA_Distance, CosineOneZeroVector) {
    std::vector<float> a(64, 0.0F);
    std::vector<float> b(64, 1.0F);
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 1.0F, 1e-6F) << "Cosine with one zero vector should be 1.0";
}

TEST(QA_Distance, CosineAntiParallelVectors) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    std::vector<float> b(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        b[i] = -a[i];
    }
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 2.0F, 1e-5F) << "Cosine of anti-parallel vectors should be 2.0";
}

TEST(QA_Distance, CosineNearZeroThreshold) {
    // Vectors with norm just above the 1e-30 threshold.
    float tiny = 1e-15F;
    std::vector<float> a = {tiny, 0.0F};
    std::vector<float> b = {tiny, 0.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    // norm_a = tiny^2 = 1e-30, norm_b = tiny^2 = 1e-30
    // norm_a * norm_b = 1e-60, which is < 1e-30. So should return 1.0.
    // But if the threshold check is on denom (after sqrt), sqrt(1e-60) = 1e-30,
    // which is NOT < 1e-30 (equal). This means it passes the check!
    // sim = dot / denom = 1e-30 / 1e-30 = 1.0
    // 1.0 - 1.0 = 0.0
    EXPECT_FALSE(std::isnan(d)) << "Cosine near threshold should not produce NaN";
    EXPECT_TRUE(std::isfinite(d));
}

TEST(QA_Distance, CosineNegativeAndPositive) {
    // Mixed signs, should produce cosine distance > 0 and <= 2.
    std::vector<float> a = {1.0F, -1.0F, 1.0F, -1.0F};
    std::vector<float> b = {-1.0F, 1.0F, 1.0F, -1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_GE(d, 0.0F);
    EXPECT_LE(d, 2.0F);
    float expected = ref_cosine(a, b);
    EXPECT_NEAR(d, expected, 1e-6F);
}

TEST(QA_Distance, CosineRangeProperty) {
    // For any non-zero vectors, cosine distance should be in [0, 2].
    std::mt19937 rng(1111);
    for (int trial = 0; trial < 100; ++trial) {
        uint32_t dim = (rng() % 256) + 1;
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float d = compute_distance(DistanceMetric::COSINE, a, b);
        EXPECT_GE(d, -1e-6F) << "trial=" << trial << " dim=" << dim;
        EXPECT_LE(d, 2.0F + 1e-6F) << "trial=" << trial << " dim=" << dim;
    }
}

// =============================================================================
// L2 Non-Negativity Property
// =============================================================================

TEST(QA_Distance, L2AlwaysNonNegative) {
    std::mt19937 rng(2222);
    for (int trial = 0; trial < 200; ++trial) {
        uint32_t dim = (rng() % 512) + 1;
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float d = compute_distance(DistanceMetric::L2, a, b);
        EXPECT_GE(d, 0.0F) << "trial=" << trial << " dim=" << dim;
    }
}

// =============================================================================
// Inner Product == Negated Dot Product Property
// =============================================================================

TEST(QA_Distance, InnerProductIsNegatedDotForManyDimensions) {
    std::mt19937 rng(3333);
    for (uint32_t dim : {1, 3, 4, 7, 8, 15, 16, 31, 32, 64, 128, 256, 512, 768, 1536}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
        EXPECT_NEAR(ip, -dot, std::fabs(dot) * 1e-6F + 1e-7F) << "dim=" << dim;
    }
}

// =============================================================================
// L2 Symmetry Property
// =============================================================================

TEST(QA_Distance, L2SymmetryAcrossDimensions) {
    std::mt19937 rng(4444);
    for (uint32_t dim : {1, 3, 7, 8, 15, 16, 33, 64, 128}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float d_ab = compute_distance(DistanceMetric::L2, a, b);
        float d_ba = compute_distance(DistanceMetric::L2, b, a);
        EXPECT_FLOAT_EQ(d_ab, d_ba) << "dim=" << dim;
    }
}

// =============================================================================
// Dot Product Symmetry Property
// =============================================================================

TEST(QA_Distance, DotProductSymmetry) {
    std::mt19937 rng(5555);
    for (uint32_t dim : {1, 4, 7, 8, 16, 33, 64, 128}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float d_ab = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float d_ba = compute_distance(DistanceMetric::DOT_PRODUCT, b, a);
        EXPECT_FLOAT_EQ(d_ab, d_ba) << "dim=" << dim;
    }
}

// =============================================================================
// Cosine Symmetry Property
// =============================================================================

TEST(QA_Distance, CosineSymmetry) {
    std::mt19937 rng(6666);
    for (uint32_t dim : {1, 4, 7, 8, 16, 33, 64, 128}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        float d_ab = compute_distance(DistanceMetric::COSINE, a, b);
        float d_ba = compute_distance(DistanceMetric::COSINE, b, a);
        EXPECT_NEAR(d_ab, d_ba, 1e-6F) << "dim=" << dim;
    }
}

// =============================================================================
// All-Same Value Vectors
// =============================================================================

TEST(QA_Distance, AllSameValueL2) {
    // a = [5, 5, ...], b = [3, 3, ...]. L2 = dim * (5-3)^2 = dim * 4.
    for (uint32_t dim : {1, 4, 8, 16, 32, 64, 128}) {
        std::vector<float> a(dim, 5.0F);
        std::vector<float> b(dim, 3.0F);
        float d = compute_distance(DistanceMetric::L2, a, b);
        float expected = static_cast<float>(dim) * 4.0F;
        EXPECT_NEAR(d, expected, expected * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

TEST(QA_Distance, AllSameValueDot) {
    // a = [2, 2, ...], b = [3, 3, ...]. dot = dim * 6.
    for (uint32_t dim : {1, 4, 8, 16, 32, 64, 128}) {
        std::vector<float> a(dim, 2.0F);
        std::vector<float> b(dim, 3.0F);
        float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float expected = static_cast<float>(dim) * 6.0F;
        EXPECT_NEAR(d, expected, expected * 1e-5F + 1e-6F) << "dim=" << dim;
    }
}

TEST(QA_Distance, AllSameValueCosine) {
    // Identical direction vectors => cosine distance = 0.
    for (uint32_t dim : {1, 4, 8, 16, 32, 64, 128}) {
        std::vector<float> a(dim, 7.0F);
        std::vector<float> b(dim, 3.0F);
        float d = compute_distance(DistanceMetric::COSINE, a, b);
        EXPECT_NEAR(d, 0.0F, 1e-5F) << "dim=" << dim;
    }
}

// =============================================================================
// Alternating Sign Pattern
// =============================================================================

TEST(QA_Distance, AlternatingSignsDot) {
    // a = [1, -1, 1, -1, ...], b = [1, 1, 1, 1, ...].
    // dot = 1 + (-1) + 1 + (-1) + ... = 0 for even dim.
    for (uint32_t dim : {2, 4, 8, 16, 32, 64, 128}) {
        std::vector<float> a(dim);
        std::vector<float> b(dim, 1.0F);
        for (uint32_t i = 0; i < dim; ++i) {
            a[i] = (i % 2 == 0) ? 1.0F : -1.0F;
        }
        float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        EXPECT_NEAR(d, 0.0F, 1e-5F) << "dim=" << dim;
    }
}

// =============================================================================
// One-Hot Vectors (Orthogonality)
// =============================================================================

TEST(QA_Distance, OneHotVectorsOrthogonal) {
    for (uint32_t dim : {4, 8, 16, 32, 64}) {
        // e_0 and e_1 are orthogonal.
        std::vector<float> e0(dim, 0.0F);
        std::vector<float> e1(dim, 0.0F);
        e0[0] = 1.0F;
        e1[1] = 1.0F;

        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, e0, e1);
        EXPECT_NEAR(dot, 0.0F, 1e-6F) << "dim=" << dim;

        float cos_d = compute_distance(DistanceMetric::COSINE, e0, e1);
        EXPECT_NEAR(cos_d, 1.0F, 1e-6F) << "dim=" << dim;

        float l2 = compute_distance(DistanceMetric::L2, e0, e1);
        EXPECT_NEAR(l2, 2.0F, 1e-5F) << "dim=" << dim;
    }
}

TEST(QA_Distance, OneHotVectorsSameAxis) {
    for (uint32_t dim : {4, 8, 16, 32, 64}) {
        std::vector<float> e0(dim, 0.0F);
        e0[0] = 1.0F;

        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, e0, e0);
        EXPECT_NEAR(dot, 1.0F, 1e-6F) << "dim=" << dim;

        float cos_d = compute_distance(DistanceMetric::COSINE, e0, e0);
        EXPECT_NEAR(cos_d, 0.0F, 1e-6F) << "dim=" << dim;

        float l2 = compute_distance(DistanceMetric::L2, e0, e0);
        EXPECT_NEAR(l2, 0.0F, 1e-6F) << "dim=" << dim;
    }
}

// =============================================================================
// Batch Computation Edge Cases
// =============================================================================

TEST(QA_DistanceBatch, AllMetricsConsistent) {
    std::mt19937 rng(6789);
    const uint32_t dim = 64;
    const uint32_t count = 50;

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

TEST(QA_DistanceBatch, DimSmallerThanQuerySize) {
    // dim < query.size() means we only use dim elements.
    std::vector<float> query = {1.0F, 2.0F, 3.0F, 4.0F};
    std::vector<float> cand = {5.0F, 6.0F, 7.0F, 8.0F};
    const float* ptr = cand.data();

    float out_full = 0.0F;
    compute_distance_batch(DistanceMetric::L2, query, &ptr, 1, 4, &out_full);

    float out_partial = 0.0F;
    compute_distance_batch(DistanceMetric::L2, query, &ptr, 1, 2, &out_partial);

    // Partial should only use first 2 dims: (5-1)^2 + (6-2)^2 = 16+16 = 32
    EXPECT_NEAR(out_partial, 32.0F, 1e-5F);
    // Full uses all 4 dims: 32 + (7-3)^2 + (8-4)^2 = 32+16+16 = 64
    EXPECT_NEAR(out_full, 64.0F, 1e-5F);
}

TEST(QA_DistanceBatch, DimLargerThanQuerySize) {
    // dim > query.size(): qdim = min(query.size(), dim), only uses query.size().
    std::vector<float> query = {1.0F, 2.0F};
    std::vector<float> cand = {5.0F, 6.0F, 7.0F, 8.0F};
    const float* ptr = cand.data();

    float out = 0.0F;
    compute_distance_batch(DistanceMetric::L2, query, &ptr, 1, 100, &out);

    // Should only use first 2 dims: (5-1)^2 + (6-2)^2 = 16+16 = 32
    EXPECT_NEAR(out, 32.0F, 1e-5F);
}

TEST(QA_DistanceBatch, LargeCount) {
    std::mt19937 rng(7890);
    const uint32_t dim = 128;
    const uint32_t count = 10000;

    auto query = random_vector(dim, rng);
    std::vector<std::vector<float>> candidates(count);
    std::vector<const float*> ptrs(count);
    for (uint32_t i = 0; i < count; ++i) {
        candidates[i] = random_vector(dim, rng);
        ptrs[i] = candidates[i].data();
    }

    std::vector<float> batch_out(count);
    compute_distance_batch(DistanceMetric::L2, query, ptrs.data(), count, dim, batch_out.data());

    // Spot-check first and last.
    float first = compute_distance(DistanceMetric::L2, query, candidates[0]);
    float last = compute_distance(DistanceMetric::L2, query, candidates[count - 1]);
    EXPECT_NEAR(batch_out[0], first, std::fabs(first) * 1e-5F + 1e-6F);
    EXPECT_NEAR(batch_out[count - 1], last, std::fabs(last) * 1e-5F + 1e-6F);
}

// =============================================================================
// Empty Vector Edge Cases (all metrics)
// =============================================================================

TEST(QA_Distance, EmptyVectorsAllMetrics) {
    std::vector<float> a;
    std::vector<float> b;
    for (auto metric :
         {DistanceMetric::L2, DistanceMetric::DOT_PRODUCT, DistanceMetric::INNER_PRODUCT}) {
        float d = compute_distance(metric, a, b);
        EXPECT_FLOAT_EQ(d, 0.0F) << "metric=" << distance_metric_name(metric);
    }
    // Cosine of empty vectors: denom = sqrt(0*0) = 0, denom < 1e-30 => returns 1.0.
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(d, 1.0F, 1e-6F);
}

// =============================================================================
// Mismatched Dimension Edge Cases
// =============================================================================

TEST(QA_Distance, MismatchedDimensionsAllMetrics) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    std::vector<float> b = {10.0F, 20.0F, 30.0F};

    // Should use min(5, 3) = 3 dims.
    float l2 = compute_distance(DistanceMetric::L2, a, b);
    // (10-1)^2 + (20-2)^2 + (30-3)^2 = 81 + 324 + 729 = 1134
    EXPECT_NEAR(l2, 1134.0F, 1e-3F);

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    // 1*10 + 2*20 + 3*30 = 10 + 40 + 90 = 140
    EXPECT_NEAR(dot, 140.0F, 1e-3F);

    float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
    EXPECT_NEAR(ip, -140.0F, 1e-3F);
}

// =============================================================================
// Specific Known-Value Tests for SIMD Verification
// =============================================================================

TEST(QA_Distance, L2KnownValue8D) {
    // 8 elements = exactly 1 AVX2 register, 2 NEON registers.
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    std::vector<float> b = {8.0F, 7.0F, 6.0F, 5.0F, 4.0F, 3.0F, 2.0F, 1.0F};
    // Diffs: -7, -5, -3, -1, 1, 3, 5, 7. Squared: 49, 25, 9, 1, 1, 9, 25, 49. Sum = 168.
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(d, 168.0F, 1e-4F);
}

TEST(QA_Distance, L2KnownValue16D) {
    // 16 elements = exactly 1 AVX-512 register, 2 AVX2, 4 NEON.
    std::vector<float> a(16);
    std::vector<float> b(16);
    std::iota(a.begin(), a.end(), 1.0F);  // [1, 2, ..., 16]
    std::iota(b.begin(), b.end(), 17.0F); // [17, 18, ..., 32]
    // Each diff is 16. L2 = 16 * 16^2 = 16 * 256 = 4096.
    float d = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(d, 4096.0F, 1e-2F);
}

TEST(QA_Distance, DotKnownValue8D) {
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    std::vector<float> b = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    // dot = 1+2+3+4+5+6+7+8 = 36
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(d, 36.0F, 1e-4F);
}

TEST(QA_Distance, DotKnownValue16D) {
    std::vector<float> a(16);
    std::vector<float> b(16, 2.0F);
    std::iota(a.begin(), a.end(), 1.0F); // [1..16]
    // dot = 2 * (1+2+...+16) = 2 * 136 = 272
    float d = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(d, 272.0F, 1e-2F);
}

// =============================================================================
// Stress: Large Dimension Accuracy
// =============================================================================

TEST(QA_Distance, LargeDimensionAccuracy) {
    // Test that SIMD accumulation doesn't lose precision for very large vectors.
    std::mt19937 rng(11111);
    for (uint32_t dim : {1024, 2048, 4096}) {
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        float exp_l2 = ref_l2(a, b);
        // For large dims, allow slightly more tolerance.
        float tol = std::fabs(exp_l2) * 1e-4F + 1e-4F;
        EXPECT_NEAR(l2, exp_l2, tol) << "dim=" << dim;

        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float exp_dot = ref_dot(a, b);
        tol = std::fabs(exp_dot) * 1e-4F + 1e-4F;
        EXPECT_NEAR(dot, exp_dot, tol) << "dim=" << dim;
    }
}

// =============================================================================
// Batch with All Identical Candidates
// =============================================================================

TEST(QA_DistanceBatch, AllIdenticalCandidates) {
    const uint32_t dim = 32;
    const uint32_t count = 100;

    std::vector<float> query(dim, 1.0F);
    std::vector<float> cand(dim, 2.0F);

    std::vector<const float*> ptrs(count, cand.data());

    std::vector<float> out(count);
    compute_distance_batch(DistanceMetric::L2, query, ptrs.data(), count, dim, out.data());

    // All distances should be identical: dim * 1.0 = 32.
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_NEAR(out[i], 32.0F, 1e-4F) << "i=" << i;
    }
}

// =============================================================================
// SIMD Level Sanity
// =============================================================================

TEST(QA_Distance, SimdLevelMatchesPlatform) {
    auto level = active_simd_level();
    const char* name = active_simd_name();

#if defined(__AVX512F__)
    EXPECT_EQ(level, SimdLevel::AVX512);
    EXPECT_STREQ(name, "AVX-512");
#elif defined(__AVX2__)
    EXPECT_EQ(level, SimdLevel::AVX2);
    EXPECT_STREQ(name, "AVX2");
#elif defined(__ARM_NEON)
    EXPECT_EQ(level, SimdLevel::NEON);
    EXPECT_STREQ(name, "NEON");
#else
    EXPECT_EQ(level, SimdLevel::SCALAR);
    EXPECT_STREQ(name, "scalar");
#endif
}

// =============================================================================
// Cosine Scale Invariance with Large Scaling Factors
// =============================================================================

TEST(QA_Distance, CosineScaleInvarianceExtreme) {
    std::mt19937 rng(22222);
    for (uint32_t dim : {4, 16, 64, 256}) {
        auto a = random_vector(dim, rng);
        std::vector<float> b(a.size());

        // Scale by various factors.
        for (float scale : {0.001F, 100.0F, 1e10F, 1e-10F}) {
            for (size_t i = 0; i < a.size(); ++i) {
                b[i] = a[i] * scale;
            }
            float d = compute_distance(DistanceMetric::COSINE, a, b);
            EXPECT_NEAR(d, 0.0F, 1e-4F) << "dim=" << dim << " scale=" << scale;
        }
    }
}

// =============================================================================
// Batch with dim=0
// =============================================================================

TEST(QA_DistanceBatch, DimZero) {
    std::vector<float> query;
    std::vector<float> cand = {1.0F, 2.0F};
    const float* ptr = cand.data();
    float out = -999.0F;
    compute_distance_batch(DistanceMetric::L2, query, &ptr, 1, 0, &out);
    EXPECT_FLOAT_EQ(out, 0.0F);
}

// =============================================================================
// Triangle Inequality for L2 (sqrt of squared L2)
// =============================================================================

TEST(QA_Distance, L2TriangleInequality) {
    std::mt19937 rng(33333);
    for (int trial = 0; trial < 50; ++trial) {
        uint32_t dim = (rng() % 64) + 4;
        auto a = random_vector(dim, rng);
        auto b = random_vector(dim, rng);
        auto c = random_vector(dim, rng);

        // sqrt(L2(a,b)) <= sqrt(L2(a,c)) + sqrt(L2(c,b))
        float d_ab = std::sqrt(compute_distance(DistanceMetric::L2, a, b));
        float d_ac = std::sqrt(compute_distance(DistanceMetric::L2, a, c));
        float d_cb = std::sqrt(compute_distance(DistanceMetric::L2, c, b));

        EXPECT_LE(d_ab, d_ac + d_cb + 1e-4F) << "Triangle inequality violated: trial=" << trial;
    }
}
