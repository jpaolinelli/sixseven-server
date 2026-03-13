#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the eigenvector centrality algorithm.
/// Output schema: (node_id INT64, score FLOAT64, iterations_used INT64).
/// Parameters: iterations INT64 (default 100), tolerance FLOAT64 (default 1e-6).
[[nodiscard]] AlgorithmDef make_eigenvector_centrality_def();

/// Execute function for the eigenvector centrality algorithm.
/// Computes the principal eigenvector of the adjacency matrix using power
/// iteration: x(t+1) = A * x(t) / ||A * x(t)||.
///
/// Convergence check: ||x(t+1) - x(t)|| < tolerance.
///
/// Disconnected components are handled by running power iteration
/// independently per weakly connected component, then normalising the
/// full vector so the maximum score across all components is 1.0.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
eigenvector_centrality_execute(const AlgorithmContext& ctx);

} // namespace sixseven
