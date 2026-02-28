/// @file test_qa_gdb_126.cpp
/// @brief QA adversarial tests for GDB-126: SIMD-accelerated distance functions.
///
/// Targets edge cases not covered by existing test_distance.cpp:
/// exact SIMD lane boundaries, subnormal inputs, mixed special values,
/// large batch operations, INNER_PRODUCT consistency, and COSINE precision.

#include "giodb/vector/distance.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using namespace giodb;

namespace {

/// Scalar reference for L2 squared distance.
float ref_l2(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

/// Scalar reference for dot product.
float ref_dot(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

/// Scalar reference for cosine distance.
float ref_cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0.0F;
    float na = 0.0F;
    float nb = 0.0F;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    if (denom < 1e-30F) {
        return 1.0F;
    }
    float sim = dot / denom;
    sim = std::fmax(-1.0F, std::fmin(1.0F, sim));
    return 1.0F - sim;
}

float tolerance(float expected) {
    return std::fabs(expected) * 1e-5F + 1e-6F;
}

} // namespace

// ---------------------------------------------------------------------------
// Exact SIMD lane boundary dimensions
// These exercise the exact widths where SIMD lanes switch to scalar remainder.
// NEON: 4, AVX2: 8, AVX512: 16
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_Boundaries, ExactNeonWidth) {
    // Dim=4 — exactly 1 NEON iteration, no remainder.
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F};
    std::vector<float> b = {5.0F, 6.0F, 7.0F, 8.0F};

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(l2, ref_l2(a, b), tolerance(ref_l2(a, b)));

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(dot, ref_dot(a, b), tolerance(ref_dot(a, b)));

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(cos, ref_cosine(a, b), 1e-5F);
}

TEST(QA_GDB_126_Boundaries, ExactAvx2Width) {
    // Dim=8 — exactly 1 AVX2 iteration.
    std::vector<float> a(8);
    std::vector<float> b(8);
    for (int i = 0; i < 8; ++i) {
        a[i] = static_cast<float>(i + 1);
        b[i] = static_cast<float>(i + 9);
    }

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(l2, ref_l2(a, b), tolerance(ref_l2(a, b)));

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(dot, ref_dot(a, b), tolerance(ref_dot(a, b)));
}

TEST(QA_GDB_126_Boundaries, ExactAvx512Width) {
    // Dim=16 — exactly 1 AVX-512 iteration.
    std::vector<float> a(16);
    std::vector<float> b(16);
    for (int i = 0; i < 16; ++i) {
        a[i] = static_cast<float>(i + 1);
        b[i] = static_cast<float>(i + 17);
    }

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(l2, ref_l2(a, b), tolerance(ref_l2(a, b)));

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(dot, ref_dot(a, b), tolerance(ref_dot(a, b)));
}

TEST(QA_GDB_126_Boundaries, NeonWidthPlusOne) {
    // Dim=5 — 1 NEON iteration + 1 scalar.
    std::vector<float> a = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    std::vector<float> b = {6.0F, 7.0F, 8.0F, 9.0F, 10.0F};

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(l2, ref_l2(a, b), tolerance(ref_l2(a, b)));
}

TEST(QA_GDB_126_Boundaries, Avx2WidthPlusOne) {
    // Dim=9 — 1 AVX2 iteration + 1 scalar.
    std::vector<float> a(9);
    std::vector<float> b(9);
    for (int i = 0; i < 9; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = static_cast<float>(i * 2);
    }

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_NEAR(dot, ref_dot(a, b), tolerance(ref_dot(a, b)));
}

TEST(QA_GDB_126_Boundaries, Avx2WidthMinusOne) {
    // Dim=7 — NEON remainder only, no full AVX2 iteration.
    std::vector<float> a(7, 1.0F);
    std::vector<float> b(7, 2.0F);

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(l2, 7.0F, tolerance(7.0F)); // Each (2-1)^2 = 1, sum = 7.
}

// ---------------------------------------------------------------------------
// Special float values
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_Special, SubnormalValues) {
    // Subnormals are tiny but valid floats.
    float sub = std::numeric_limits<float>::denorm_min();
    std::vector<float> a(16, sub);
    std::vector<float> b(16, sub);

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_TRUE(std::isfinite(l2));
    EXPECT_NEAR(l2, 0.0F, 1e-30F); // Identical vectors.

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_TRUE(std::isfinite(dot));
    EXPECT_GE(dot, 0.0F); // Dot of identical vectors >= 0.
}

TEST(QA_GDB_126_Special, InfinityInput) {
    float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {inf, 0.0F, 0.0F, 0.0F};
    std::vector<float> b = {0.0F, 0.0F, 0.0F, 0.0F};

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_TRUE(std::isinf(l2) || l2 > 1e30F); // Should be infinity.

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    // inf * 0 = NaN in IEEE 754, but only first element is inf, rest are 0.
    // dot = inf*0 + 0*0 + 0*0 + 0*0 = NaN + 0 + 0 + 0 = NaN.
    // Actually, on many platforms this might be NaN.
    // Just check it doesn't crash.
    (void)dot;
}

TEST(QA_GDB_126_Special, NegativeInfinity) {
    float ninf = -std::numeric_limits<float>::infinity();
    std::vector<float> a = {ninf, 1.0F, 2.0F, 3.0F};
    std::vector<float> b = {1.0F, 1.0F, 2.0F, 3.0F};

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    // (-inf - 1)^2 = inf, so result should be inf.
    EXPECT_TRUE(std::isinf(l2));
}

TEST(QA_GDB_126_Special, MaxFloatValues) {
    float mx = std::numeric_limits<float>::max();
    std::vector<float> a = {mx, 0.0F, 0.0F, 0.0F};
    std::vector<float> b = {0.0F, 0.0F, 0.0F, 0.0F};

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    // max^2 overflows to inf.
    EXPECT_TRUE(std::isinf(l2) || l2 == std::numeric_limits<float>::max());
}

TEST(QA_GDB_126_Special, NegativeZero) {
    std::vector<float> a = {0.0F, 0.0F, 0.0F, 0.0F};
    std::vector<float> b = {-0.0F, -0.0F, -0.0F, -0.0F};

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_NEAR(l2, 0.0F, 1e-10F); // +0 and -0 are equal.

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(cos, 1.0F, 1e-5F); // Zero vectors -> cosine distance 1.
}

// ---------------------------------------------------------------------------
// INNER_PRODUCT consistency
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_InnerProduct, ConsistentWithDotNegation) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    for (uint32_t dim : {4, 8, 16, 32, 64, 128}) {
        std::vector<float> a(dim);
        std::vector<float> b(dim);
        for (auto& v : a) {
            v = dist(rng);
        }
        for (auto& v : b) {
            v = dist(rng);
        }

        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
        EXPECT_NEAR(ip, -dot, tolerance(dot))
            << "INNER_PRODUCT != -DOT_PRODUCT at dim=" << dim;
    }
}

// ---------------------------------------------------------------------------
// COSINE precision
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_Cosine, NearlyParallelVectors) {
    // Two very similar vectors — cosine distance should be near 0.
    std::vector<float> a(64, 1.0F);
    std::vector<float> b(64, 1.0F);
    b[0] = 1.001F; // Tiny perturbation.

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isfinite(cos));
    EXPECT_LT(cos, 0.001F); // Should be very close to 0.
    EXPECT_GE(cos, 0.0F);   // Should not be negative.
}

TEST(QA_GDB_126_Cosine, AntiparallelVectors) {
    // Exactly opposite vectors — cosine distance should be 2.
    std::vector<float> a = {1.0F, 0.0F, 0.0F, 0.0F};
    std::vector<float> b = {-1.0F, 0.0F, 0.0F, 0.0F};

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(cos, 2.0F, 1e-5F);
}

TEST(QA_GDB_126_Cosine, UnitVectorsOrthogonal) {
    // Orthogonal unit vectors — cosine distance should be 1.
    std::vector<float> a = {1.0F, 0.0F, 0.0F, 0.0F};
    std::vector<float> b = {0.0F, 1.0F, 0.0F, 0.0F};

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(cos, 1.0F, 1e-5F);
}

TEST(QA_GDB_126_Cosine, ScaledIdentical) {
    // Same direction, different magnitudes — cosine distance should be 0.
    std::vector<float> a = {3.0F, 4.0F};
    std::vector<float> b = {30.0F, 40.0F};

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_NEAR(cos, 0.0F, 1e-5F);
}

// ---------------------------------------------------------------------------
// Batch operations
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_Batch, LargeBatch) {
    const uint32_t dim = 64;
    const uint32_t count = 500;
    std::mt19937 rng(123);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    std::vector<float> query(dim);
    for (auto& v : query) {
        v = dist(rng);
    }

    std::vector<std::vector<float>> candidates(count, std::vector<float>(dim));
    std::vector<const float*> ptrs(count);
    for (uint32_t i = 0; i < count; ++i) {
        for (auto& v : candidates[i]) {
            v = dist(rng);
        }
        ptrs[i] = candidates[i].data();
    }

    std::vector<float> batch_out(count);
    compute_distance_batch(
        DistanceMetric::L2, query, ptrs.data(), count, dim, batch_out.data());

    // Verify each result against individual computation.
    for (uint32_t i = 0; i < count; ++i) {
        float single = compute_distance(DistanceMetric::L2, query, candidates[i]);
        EXPECT_NEAR(batch_out[i], single, tolerance(single))
            << "Mismatch at candidate " << i;
    }
}

TEST(QA_GDB_126_Batch, AllMetricsConsistent) {
    const uint32_t dim = 32;
    const uint32_t count = 10;
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    std::vector<float> query(dim);
    for (auto& v : query) {
        v = dist(rng);
    }

    std::vector<std::vector<float>> candidates(count, std::vector<float>(dim));
    std::vector<const float*> ptrs(count);
    for (uint32_t i = 0; i < count; ++i) {
        for (auto& v : candidates[i]) {
            v = dist(rng);
        }
        ptrs[i] = candidates[i].data();
    }

    for (auto metric : {DistanceMetric::L2, DistanceMetric::COSINE,
                        DistanceMetric::DOT_PRODUCT, DistanceMetric::INNER_PRODUCT}) {
        std::vector<float> batch_out(count);
        compute_distance_batch(metric, query, ptrs.data(), count, dim, batch_out.data());

        for (uint32_t i = 0; i < count; ++i) {
            float single = compute_distance(metric, query, candidates[i]);
            float tol = (metric == DistanceMetric::COSINE) ? 1e-5F : tolerance(single);
            EXPECT_NEAR(batch_out[i], single, tol)
                << "Metric=" << distance_metric_name(metric) << " candidate=" << i;
        }
    }
}

TEST(QA_GDB_126_Batch, BatchDim1) {
    // Edge case: single-dimension vectors.
    const uint32_t dim = 1;
    const uint32_t count = 5;
    std::vector<float> query = {3.0F};

    std::vector<std::vector<float>> candidates = {{1.0F}, {2.0F}, {3.0F}, {4.0F}, {5.0F}};
    std::vector<const float*> ptrs(count);
    for (uint32_t i = 0; i < count; ++i) {
        ptrs[i] = candidates[i].data();
    }

    std::vector<float> out(count);
    compute_distance_batch(DistanceMetric::L2, query, ptrs.data(), count, dim, out.data());

    EXPECT_NEAR(out[0], 4.0F, 1e-6F);  // (3-1)^2 = 4
    EXPECT_NEAR(out[1], 1.0F, 1e-6F);  // (3-2)^2 = 1
    EXPECT_NEAR(out[2], 0.0F, 1e-6F);  // (3-3)^2 = 0
    EXPECT_NEAR(out[3], 1.0F, 1e-6F);  // (3-4)^2 = 1
    EXPECT_NEAR(out[4], 4.0F, 1e-6F);  // (3-5)^2 = 4
}

// ---------------------------------------------------------------------------
// All metrics on same data
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_AllMetrics, IdenticalVectorsAllMetrics) {
    std::vector<float> v = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};

    float l2 = compute_distance(DistanceMetric::L2, v, v);
    EXPECT_NEAR(l2, 0.0F, 1e-6F);

    float cos = compute_distance(DistanceMetric::COSINE, v, v);
    EXPECT_NEAR(cos, 0.0F, 1e-5F);

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, v, v);
    EXPECT_GT(dot, 0.0F); // dot(v,v) = sum(v_i^2) > 0.

    float ip = compute_distance(DistanceMetric::INNER_PRODUCT, v, v);
    EXPECT_NEAR(ip, -dot, 1e-6F);
}

// ---------------------------------------------------------------------------
// Metric names
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_Names, AllMetricNamesNonNull) {
    EXPECT_NE(distance_metric_name(DistanceMetric::L2), nullptr);
    EXPECT_NE(distance_metric_name(DistanceMetric::COSINE), nullptr);
    EXPECT_NE(distance_metric_name(DistanceMetric::DOT_PRODUCT), nullptr);
    EXPECT_NE(distance_metric_name(DistanceMetric::INNER_PRODUCT), nullptr);

    // SIMD level name should also be non-null.
    EXPECT_NE(active_simd_name(), nullptr);
}

// ---------------------------------------------------------------------------
// Large dimension stress
// ---------------------------------------------------------------------------

TEST(QA_GDB_126_Stress, Dim4096AllMetrics) {
    const uint32_t dim = 4096;
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    std::vector<float> a(dim);
    std::vector<float> b(dim);
    for (auto& v : a) {
        v = dist(rng);
    }
    for (auto& v : b) {
        v = dist(rng);
    }

    float l2 = compute_distance(DistanceMetric::L2, a, b);
    EXPECT_TRUE(std::isfinite(l2));
    EXPECT_GE(l2, 0.0F);

    float cos = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isfinite(cos));
    EXPECT_GE(cos, -1e-6F);
    EXPECT_LE(cos, 2.0F + 1e-6F);

    float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
    EXPECT_TRUE(std::isfinite(dot));

    float ip = compute_distance(DistanceMetric::INNER_PRODUCT, a, b);
    EXPECT_NEAR(ip, -dot, tolerance(dot));
}

TEST(QA_GDB_126_Stress, ManyRandomDimensions) {
    // Test dimensions that are NOT powers of 2 or SIMD-aligned.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    for (uint32_t dim : {3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 19, 23, 33, 63, 65, 127, 129}) {
        std::vector<float> a(dim);
        std::vector<float> b(dim);
        for (auto& v : a) {
            v = dist(rng);
        }
        for (auto& v : b) {
            v = dist(rng);
        }

        float l2 = compute_distance(DistanceMetric::L2, a, b);
        EXPECT_NEAR(l2, ref_l2(a, b), tolerance(ref_l2(a, b))) << "L2 mismatch at dim=" << dim;

        float dot = compute_distance(DistanceMetric::DOT_PRODUCT, a, b);
        EXPECT_NEAR(dot, ref_dot(a, b), tolerance(ref_dot(a, b)))
            << "DOT mismatch at dim=" << dim;

        float cos = compute_distance(DistanceMetric::COSINE, a, b);
        EXPECT_NEAR(cos, ref_cosine(a, b), 1e-4F) << "COSINE mismatch at dim=" << dim;
    }
}
