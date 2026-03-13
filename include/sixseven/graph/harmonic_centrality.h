#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the harmonic centrality algorithm.
/// Output schema: (node_id INT64, harmonic FLOAT64, normalized_harmonic FLOAT64).
/// No parameters — naturally handles disconnected graphs.
[[nodiscard]] AlgorithmDef make_harmonic_centrality_def();

/// Execute function for the harmonic centrality algorithm.
/// Computes harmonic centrality via BFS from every node in O(V*(V+E)) time.
///
/// Formula: H(v) = sum of 1/d(v,u) for all u != v where d(v,u) is the
/// shortest-path distance. Unreachable nodes contribute 0 (1/infinity = 0).
///
/// Normalized: H_norm(v) = H(v) / (N - 1).
///
/// Naturally handles disconnected graphs without special parameters.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
harmonic_centrality_execute(const AlgorithmContext& ctx);

} // namespace sixseven
