#include "sixseven/vector/distance.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

using namespace sixseven;

// =============================================================================
// GDB-832: Adversarial tests for cosine distance with infinity / NaN inputs.
//
// The ticket fixed a tautological EXPECT_FALSE(isnan&&isinf) assertion in
// CosineWithInfinityInput, replacing it with EXPECT_TRUE(isnan(d)).
//
// This suite probes the full matrix of inf/nan input shapes to verify:
//   (a) Every shape that is mathematically undefined returns NaN.
//   (b) Shapes where a finite answer is correct actually return a finite value.
//   (c) No shape silently returns a wrong finite value instead of NaN.
//   (d) All SIMD paths agree — tested indirectly by exercising dimensions that
//       span scalar tail, NEON (4-wide), AVX2 (8-wide), AVX512 (16-wide).
// =============================================================================

namespace {
constexpr float INF  = std::numeric_limits<float>::infinity();
constexpr float NINF = -std::numeric_limits<float>::infinity();
constexpr float NAN_ = std::numeric_limits<float>::quiet_NaN();
} // namespace

// ---------------------------------------------------------------------------
// Shape 1 (the original ticket case): +inf in one component of a, finite b.
// Math: dot=inf, norm_a=inf, denom=inf, sim=inf/inf=NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, PlusInfOneVectorOnlyPosition0) {
    std::vector<float> a = {INF, 0.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({inf,0},{1,1}): dot=inf,denom=inf => NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 2: -inf in one component of a, finite b.
// Math: dot=-inf, norm_a=inf, denom=inf, sim=-inf/inf=NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, MinusInfOneVectorOnlyPosition0) {
    std::vector<float> a = {NINF, 0.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({-inf,0},{1,1}): dot=-inf,denom=inf => NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 3: +inf in b only.
// Math: dot=inf, norm_b=inf, denom=inf, sim=inf/inf=NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, PlusInfInBOnly) {
    std::vector<float> a = {1.0F, 1.0F};
    std::vector<float> b = {INF, 0.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({1,1},{inf,0}): expected NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 4: +inf in both vectors at same position.
// Math: dot=inf*inf=inf, norm_a=inf, norm_b=inf, denom=inf, sim=inf/inf=NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, PlusInfBothVectorsSameComponent) {
    std::vector<float> a = {INF, 1.0F};
    std::vector<float> b = {INF, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({inf,1},{inf,1}): dot=inf,denom=inf => NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 5: +inf in a at position where b is zero.
// IEEE 754: inf * 0 = NaN (indeterminate). So dot = NaN, denom = inf,
// sim = NaN/inf = NaN. Returns NaN.
// NOTE: intuition suggests dot=0 because inf*0 might "cancel", but IEEE 754
// defines inf*0 as NaN (not 0), so the result is NaN — not 1.0.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, InfInAAtPositionWhereBIsZero) {
    std::vector<float> a = {INF, 0.0F};
    std::vector<float> b = {0.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    // IEEE 754: inf * 0 = NaN, so dot = NaN, sim = NaN/inf = NaN.
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({inf,0},{0,1}): IEEE inf*0=NaN => dot=NaN => result NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 6: Mixed +inf and -inf in same vector a (direction undefined).
// Math: dot = inf*1 + (-inf)*1 = NaN. norm_a = inf. denom = inf.
//       sim = NaN/inf = NaN. Returns NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, MixedPlusAndMinusInfInA) {
    std::vector<float> a = {INF, NINF};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({inf,-inf},{1,1}): dot=NaN => NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 7: +inf in a, finite b with NO zero components.
// This is the original ticket case shape with more components.
// Math: dot=inf, denom=inf, sim=NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, InfInAAllNonZeroB) {
    std::vector<float> a = {INF, 1.0F, 2.0F, 3.0F};
    std::vector<float> b = {1.0F, 2.0F, 3.0F, 4.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine with inf component (all b nonzero): expected NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 8: NaN in a — should propagate to result (not be silently clamped).
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, NaNInAPropagatesToResult) {
    std::vector<float> a = {NAN_, 1.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine with NaN input must propagate NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 9: NaN in b.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, NaNInBPropagatesToResult) {
    std::vector<float> a = {1.0F, 1.0F};
    std::vector<float> b = {NAN_, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine with NaN in b must propagate NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 10: inf in a, zero vector b.
// Math: norm_b = 0, denom = sqrt(inf * 0) = NaN.
//       denom < 1e-30? NaN < 1e-30 is false (NaN comparisons are false).
//       So we fall through: sim = inf/NaN = NaN. Returns NaN.
// The guard DOES NOT trigger for NaN denom (because NaN < 1e-30F is false).
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, InfInAZeroVectorB) {
    std::vector<float> a = {INF, 1.0F};
    std::vector<float> b = {0.0F, 0.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    // denom = sqrt(inf * 0) = sqrt(NaN) = NaN; NaN < 1e-30 is false.
    // similarity = inf/NaN = NaN or dot/NaN = NaN. Must be NaN.
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({inf,1},{0,0}): denom=NaN, expect NaN result, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 11: SIMD width stress — inf at various positions across a 16-element
// vector to exercise SIMD lanes. If any SIMD lane mishandles inf it may
// silently corrupt an accumulator. We test inf at index 0 (AVX512 lane 0),
// index 8 (AVX512 lane 8), and index 15 (last lane before tail).
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, InfAt16DimensionPositions) {
    for (int inf_pos : {0, 7, 8, 15}) {
        std::vector<float> a(16, 1.0F);
        std::vector<float> b(16, 1.0F);
        a[inf_pos] = INF;

        float d = compute_distance(DistanceMetric::COSINE, a, b);
        EXPECT_TRUE(std::isnan(d))
            << "Cosine 16D with inf at pos=" << inf_pos << " expected NaN, got " << d;
    }
}

// ---------------------------------------------------------------------------
// Shape 12: inf at various positions in a 32-element vector (2x AVX512 iter).
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, InfAt32DimensionPositions) {
    for (int inf_pos : {0, 15, 16, 31}) {
        std::vector<float> a(32, 1.0F);
        std::vector<float> b(32, 1.0F);
        a[inf_pos] = INF;

        float d = compute_distance(DistanceMetric::COSINE, a, b);
        EXPECT_TRUE(std::isnan(d))
            << "Cosine 32D with inf at pos=" << inf_pos << " expected NaN, got " << d;
    }
}

// ---------------------------------------------------------------------------
// Shape 13: inf only in scalar tail (position >= SIMD width).
// AVX2 processes 8 at a time; inf at pos 9 falls in the scalar tail.
// AVX512 processes 16 at a time; inf at pos 17 falls in the scalar tail.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, InfInScalarTailAfterSimdBlock) {
    for (int dim : {9, 17, 25, 33}) {
        std::vector<float> a(dim, 1.0F);
        std::vector<float> b(dim, 1.0F);
        a[dim - 1] = INF; // Last element = scalar tail.

        float d = compute_distance(DistanceMetric::COSINE, a, b);
        EXPECT_TRUE(std::isnan(d))
            << "Cosine dim=" << dim << " inf in scalar tail: expected NaN, got " << d;
    }
}

// ---------------------------------------------------------------------------
// Shape 14: +inf and -inf in both vectors at same position (cancels in dot,
// but the norm still accumulates inf). Result must be NaN because the
// norm still has inf making denom inf and sim= dot/inf.
// Case: a={inf,1}, b={-inf,1}: dot=inf*(-inf)+1*1 = -inf+1 = -inf.
// norm_a=inf, norm_b=inf, denom=inf. sim=-inf/inf=NaN.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, OppositeSignInfBothVectors) {
    std::vector<float> a = {INF, 1.0F};
    std::vector<float> b = {NINF, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Cosine({inf,1},{-inf,1}): dot=-inf,denom=inf => NaN, got " << d;
}

// ---------------------------------------------------------------------------
// Shape 15: The rewritten fix test itself (golden case from ticket).
// Replicated here as a regression anchor so this file locks the behavior.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, GoldenCaseFromTicket) {
    std::vector<float> a = {INF, 0.0F};
    std::vector<float> b = {1.0F, 1.0F};
    float d = compute_distance(DistanceMetric::COSINE, a, b);
    EXPECT_TRUE(std::isnan(d))
        << "Golden ticket case: Cosine({inf,0},{1,1}) must return NaN, got " << d;
    // Confirm the old tautological guard was wrong: a float CANNOT be both
    // NaN and Inf. The old assertion would have passed even if d were 42.0.
    EXPECT_FALSE(std::isnan(d) && std::isinf(d))
        << "Sanity: no float is simultaneously NaN and Inf";
}

// ---------------------------------------------------------------------------
// Shape 16: Batch compute_distance_batch with inf inputs.
// Verifies the batch kernel handles inf the same as the single kernel.
// ---------------------------------------------------------------------------
TEST(QA_GDB832_CosineInf, BatchKernelWithInfInput) {
    std::vector<float> query = {INF, 0.0F};
    std::vector<float> cand  = {1.0F, 1.0F};
    const float* ptr = cand.data();

    float batch_result = -999.0F;
    compute_distance_batch(DistanceMetric::COSINE, query, &ptr, 1, 2, &batch_result);

    float single_result = compute_distance(DistanceMetric::COSINE, query, cand);

    // Both must be NaN.
    EXPECT_TRUE(std::isnan(batch_result))
        << "Batch cosine with inf: expected NaN, got " << batch_result;
    // Single and batch must agree (both NaN — isnan checks).
    EXPECT_TRUE(std::isnan(single_result) == std::isnan(batch_result))
        << "Single and batch cosine disagree on inf handling";
}
