#include "giodb/vector/distance.h"

#include <cmath>
#include <cstddef>

// SIMD intrinsics headers based on platform.
#if defined(__AVX512F__)
#include <immintrin.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace giodb {

// =============================================================================
// Scalar fallback implementations
// =============================================================================

namespace {

[[maybe_unused]] float l2_squared_scalar(const float* a, const float* b, uint32_t dim) {
    float sum = 0.0F;
    for (uint32_t i = 0; i < dim; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

[[maybe_unused]] float dot_product_scalar(const float* a, const float* b, uint32_t dim) {
    float sum = 0.0F;
    for (uint32_t i = 0; i < dim; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

[[maybe_unused]] float cosine_distance_scalar(const float* a, const float* b, uint32_t dim) {
    float dot = 0.0F;
    float norm_a = 0.0F;
    float norm_b = 0.0F;
    for (uint32_t i = 0; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    float denom = std::sqrt(norm_a * norm_b);
    if (denom < 1e-30F) {
        return 1.0F; // Zero-vector edge case: treat as maximum distance.
    }
    float similarity = dot / denom;
    // Clamp to [-1, 1] to handle floating-point rounding.
    similarity = std::fmin(1.0F, std::fmax(-1.0F, similarity));
    return 1.0F - similarity;
}

// =============================================================================
// ARM NEON implementations
// =============================================================================

#if defined(__ARM_NEON)

float l2_squared_neon(const float* a, const float* b, uint32_t dim) {
    float32x4_t sum_vec = vdupq_n_f32(0.0F);

    uint32_t i = 0;
    // Process 16 floats per iteration (4 NEON registers).
    for (; i + 16 <= dim; i += 16) {
        float32x4_t a0 = vld1q_f32(a + i);
        float32x4_t b0 = vld1q_f32(b + i);
        float32x4_t d0 = vsubq_f32(a0, b0);
        sum_vec = vmlaq_f32(sum_vec, d0, d0);

        float32x4_t a1 = vld1q_f32(a + i + 4);
        float32x4_t b1 = vld1q_f32(b + i + 4);
        float32x4_t d1 = vsubq_f32(a1, b1);
        sum_vec = vmlaq_f32(sum_vec, d1, d1);

        float32x4_t a2 = vld1q_f32(a + i + 8);
        float32x4_t b2 = vld1q_f32(b + i + 8);
        float32x4_t d2 = vsubq_f32(a2, b2);
        sum_vec = vmlaq_f32(sum_vec, d2, d2);

        float32x4_t a3 = vld1q_f32(a + i + 12);
        float32x4_t b3 = vld1q_f32(b + i + 12);
        float32x4_t d3 = vsubq_f32(a3, b3);
        sum_vec = vmlaq_f32(sum_vec, d3, d3);
    }
    // Process 4 floats per iteration.
    for (; i + 4 <= dim; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        float32x4_t diff = vsubq_f32(va, vb);
        sum_vec = vmlaq_f32(sum_vec, diff, diff);
    }

    float sum = vaddvq_f32(sum_vec);
    // Scalar tail for remaining elements.
    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float dot_product_neon(const float* a, const float* b, uint32_t dim) {
    float32x4_t sum_vec = vdupq_n_f32(0.0F);

    uint32_t i = 0;
    for (; i + 16 <= dim; i += 16) {
        sum_vec = vmlaq_f32(sum_vec, vld1q_f32(a + i), vld1q_f32(b + i));
        sum_vec = vmlaq_f32(sum_vec, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        sum_vec = vmlaq_f32(sum_vec, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        sum_vec = vmlaq_f32(sum_vec, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    for (; i + 4 <= dim; i += 4) {
        sum_vec = vmlaq_f32(sum_vec, vld1q_f32(a + i), vld1q_f32(b + i));
    }

    float sum = vaddvq_f32(sum_vec);
    for (; i < dim; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

float cosine_distance_neon(const float* a, const float* b, uint32_t dim) {
    float32x4_t dot_vec = vdupq_n_f32(0.0F);
    float32x4_t norm_a_vec = vdupq_n_f32(0.0F);
    float32x4_t norm_b_vec = vdupq_n_f32(0.0F);

    uint32_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        dot_vec = vmlaq_f32(dot_vec, va, vb);
        norm_a_vec = vmlaq_f32(norm_a_vec, va, va);
        norm_b_vec = vmlaq_f32(norm_b_vec, vb, vb);
    }

    float dot = vaddvq_f32(dot_vec);
    float norm_a = vaddvq_f32(norm_a_vec);
    float norm_b = vaddvq_f32(norm_b_vec);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = std::sqrt(norm_a * norm_b);
    if (denom < 1e-30F) {
        return 1.0F;
    }
    float similarity = dot / denom;
    similarity = std::fmin(1.0F, std::fmax(-1.0F, similarity));
    return 1.0F - similarity;
}

#endif // __ARM_NEON

// =============================================================================
// x86 AVX2 implementations
// =============================================================================

#if defined(__AVX2__)

float l2_squared_avx2(const float* a, const float* b, uint32_t dim) {
    __m256 sum_vec = _mm256_setzero_ps();

    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        sum_vec = _mm256_fmadd_ps(diff, diff, sum_vec);
    }

    // Horizontal sum of 8 floats.
    __m128 hi = _mm256_extractf128_ps(sum_vec, 1);
    __m128 lo = _mm256_castps256_ps128(sum_vec);
    __m128 sum128 = _mm_add_ps(lo, hi);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    float sum = _mm_cvtss_f32(sum128);

    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float dot_product_avx2(const float* a, const float* b, uint32_t dim) {
    __m256 sum_vec = _mm256_setzero_ps();

    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum_vec = _mm256_fmadd_ps(va, vb, sum_vec);
    }

    __m128 hi = _mm256_extractf128_ps(sum_vec, 1);
    __m128 lo = _mm256_castps256_ps128(sum_vec);
    __m128 sum128 = _mm_add_ps(lo, hi);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    float sum = _mm_cvtss_f32(sum128);

    for (; i < dim; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

float cosine_distance_avx2(const float* a, const float* b, uint32_t dim) {
    __m256 dot_vec = _mm256_setzero_ps();
    __m256 norm_a_vec = _mm256_setzero_ps();
    __m256 norm_b_vec = _mm256_setzero_ps();

    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        dot_vec = _mm256_fmadd_ps(va, vb, dot_vec);
        norm_a_vec = _mm256_fmadd_ps(va, va, norm_a_vec);
        norm_b_vec = _mm256_fmadd_ps(vb, vb, norm_b_vec);
    }

    // Horizontal sums.
    auto hsum = [](__m256 v) -> float {
        __m128 hi = _mm256_extractf128_ps(v, 1);
        __m128 lo = _mm256_castps256_ps128(v);
        __m128 s = _mm_add_ps(lo, hi);
        s = _mm_hadd_ps(s, s);
        s = _mm_hadd_ps(s, s);
        return _mm_cvtss_f32(s);
    };

    float dot = hsum(dot_vec);
    float norm_a = hsum(norm_a_vec);
    float norm_b = hsum(norm_b_vec);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = std::sqrt(norm_a * norm_b);
    if (denom < 1e-30F) {
        return 1.0F;
    }
    float similarity = dot / denom;
    similarity = std::fmin(1.0F, std::fmax(-1.0F, similarity));
    return 1.0F - similarity;
}

#endif // __AVX2__

// =============================================================================
// x86 AVX-512 implementations
// =============================================================================

#if defined(__AVX512F__)

float l2_squared_avx512(const float* a, const float* b, uint32_t dim) {
    __m512 sum_vec = _mm512_setzero_ps();

    uint32_t i = 0;
    for (; i + 16 <= dim; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        __m512 diff = _mm512_sub_ps(va, vb);
        sum_vec = _mm512_fmadd_ps(diff, diff, sum_vec);
    }

    float sum = _mm512_reduce_add_ps(sum_vec);

    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float dot_product_avx512(const float* a, const float* b, uint32_t dim) {
    __m512 sum_vec = _mm512_setzero_ps();

    uint32_t i = 0;
    for (; i + 16 <= dim; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        sum_vec = _mm512_fmadd_ps(va, vb, sum_vec);
    }

    float sum = _mm512_reduce_add_ps(sum_vec);

    for (; i < dim; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

float cosine_distance_avx512(const float* a, const float* b, uint32_t dim) {
    __m512 dot_vec = _mm512_setzero_ps();
    __m512 norm_a_vec = _mm512_setzero_ps();
    __m512 norm_b_vec = _mm512_setzero_ps();

    uint32_t i = 0;
    for (; i + 16 <= dim; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        dot_vec = _mm512_fmadd_ps(va, vb, dot_vec);
        norm_a_vec = _mm512_fmadd_ps(va, va, norm_a_vec);
        norm_b_vec = _mm512_fmadd_ps(vb, vb, norm_b_vec);
    }

    float dot = _mm512_reduce_add_ps(dot_vec);
    float norm_a = _mm512_reduce_add_ps(norm_a_vec);
    float norm_b = _mm512_reduce_add_ps(norm_b_vec);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = std::sqrt(norm_a * norm_b);
    if (denom < 1e-30F) {
        return 1.0F;
    }
    float similarity = dot / denom;
    similarity = std::fmin(1.0F, std::fmax(-1.0F, similarity));
    return 1.0F - similarity;
}

#endif // __AVX512F__

// =============================================================================
// Dispatch: select the best available SIMD implementation at compile time.
// =============================================================================

using DistanceFn = float (*)(const float*, const float*, uint32_t);

struct DistanceDispatch {
    DistanceFn l2_fn;
    DistanceFn dot_fn;
    DistanceFn cosine_fn;
    SimdLevel level;
};

constexpr DistanceDispatch make_dispatch() {
#if defined(__AVX512F__)
    return {l2_squared_avx512, dot_product_avx512, cosine_distance_avx512, SimdLevel::AVX512};
#elif defined(__AVX2__)
    return {l2_squared_avx2, dot_product_avx2, cosine_distance_avx2, SimdLevel::AVX2};
#elif defined(__ARM_NEON)
    return {l2_squared_neon, dot_product_neon, cosine_distance_neon, SimdLevel::NEON};
#else
    return {l2_squared_scalar, dot_product_scalar, cosine_distance_scalar, SimdLevel::SCALAR};
#endif
}

// NOLINTNEXTLINE(cert-err58-cpp)
const DistanceDispatch dispatch = make_dispatch();

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

float compute_distance(DistanceMetric metric, std::span<const float> a, std::span<const float> b) {
    auto dim = static_cast<uint32_t>(std::min(a.size(), b.size()));
    const float* pa = a.data();
    const float* pb = b.data();

    switch (metric) {
    case DistanceMetric::L2:
        return dispatch.l2_fn(pa, pb, dim);
    case DistanceMetric::COSINE:
        return dispatch.cosine_fn(pa, pb, dim);
    case DistanceMetric::DOT_PRODUCT:
        return dispatch.dot_fn(pa, pb, dim);
    case DistanceMetric::INNER_PRODUCT:
        return -dispatch.dot_fn(pa, pb, dim);
    }
    return 0.0F;
}

void compute_distance_batch(DistanceMetric metric,
                            std::span<const float> query,
                            const float* const* candidates,
                            uint32_t count,
                            uint32_t dim,
                            float* out) {
    const float* q = query.data();
    uint32_t qdim = std::min(static_cast<uint32_t>(query.size()), dim);

    // Select the kernel once, then loop over all candidates.
    DistanceFn kernel = nullptr;
    bool negate = false;

    switch (metric) {
    case DistanceMetric::L2:
        kernel = dispatch.l2_fn;
        break;
    case DistanceMetric::COSINE:
        kernel = dispatch.cosine_fn;
        break;
    case DistanceMetric::DOT_PRODUCT:
        kernel = dispatch.dot_fn;
        break;
    case DistanceMetric::INNER_PRODUCT:
        kernel = dispatch.dot_fn;
        negate = true;
        break;
    }

    if (negate) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = -kernel(q, candidates[i], qdim);
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = kernel(q, candidates[i], qdim);
        }
    }
}

SimdLevel active_simd_level() {
    return dispatch.level;
}

const char* active_simd_name() {
    switch (dispatch.level) {
    case SimdLevel::SCALAR:
        return "scalar";
    case SimdLevel::NEON:
        return "NEON";
    case SimdLevel::AVX2:
        return "AVX2";
    case SimdLevel::AVX512:
        return "AVX-512";
    }
    return "unknown";
}

} // namespace giodb
