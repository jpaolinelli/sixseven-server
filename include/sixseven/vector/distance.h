#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sixseven {

/// Distance metrics for vector similarity search.
enum class DistanceMetric : uint8_t {
    /// Squared Euclidean distance: sum((a[i] - b[i])^2).
    /// Lower values indicate more similar vectors.
    L2,

    /// Cosine distance: 1 - cosine_similarity.
    /// Range [0, 2]. Lower values indicate more similar vectors.
    COSINE,

    /// Raw dot product: sum(a[i] * b[i]).
    /// Higher values indicate more similar vectors.
    DOT_PRODUCT,

    /// Negative dot product: -sum(a[i] * b[i]).
    /// Lower values indicate more similar vectors (suitable for HNSW).
    INNER_PRODUCT,
};

/// Return a human-readable name for a DistanceMetric.
constexpr const char* distance_metric_name(DistanceMetric metric) {
    switch (metric) {
    case DistanceMetric::L2:
        return "L2";
    case DistanceMetric::COSINE:
        return "COSINE";
    case DistanceMetric::DOT_PRODUCT:
        return "DOT_PRODUCT";
    case DistanceMetric::INNER_PRODUCT:
        return "INNER_PRODUCT";
    }
    return "UNKNOWN";
}

/// Compute the distance between two vectors using the given metric.
/// Both vectors must have the same size. Behavior is undefined if sizes differ.
/// @param metric The distance metric to use.
/// @param a First vector.
/// @param b Second vector.
/// @return The computed distance value.
[[nodiscard]] float
compute_distance(DistanceMetric metric, std::span<const float> a, std::span<const float> b);

/// Compute distances from a query vector to multiple candidate vectors.
///
/// This is more efficient than calling compute_distance in a loop because
/// it amortizes dispatch overhead and enables prefetching.
///
/// @param metric The distance metric to use.
/// @param query The query vector (dim floats).
/// @param candidates Array of pointers to candidate vectors (each dim floats).
/// @param count Number of candidate vectors.
/// @param dim Dimension of each vector (must match query.size()).
/// @param out Output array for distances (must have space for count floats).
void compute_distance_batch(DistanceMetric metric,
                            std::span<const float> query,
                            const float* const* candidates,
                            uint32_t count,
                            uint32_t dim,
                            float* out);

// -- SIMD capability info -----------------------------------------------------

/// Identifiers for SIMD instruction set levels.
enum class SimdLevel : uint8_t {
    SCALAR,
    NEON,
    AVX2,
    AVX512,
};

/// Return the SIMD level selected at compile time for this build.
[[nodiscard]] SimdLevel active_simd_level();

/// Return a human-readable name for the active SIMD level.
[[nodiscard]] const char* active_simd_name();

} // namespace sixseven
