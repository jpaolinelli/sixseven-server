#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the closeness centrality algorithm.
/// Output schema: (node_id INT64, closeness FLOAT64,
///                 sum_farness INT64, reachable_count INT64).
/// No additional parameters beyond the required edge_type.
[[nodiscard]] AlgorithmDef make_closeness_centrality_def();

/// Execute function for the closeness centrality algorithm.
/// Computes closeness centrality via BFS from every node in O(V*(V+E)) time.
/// Formula: C(v) = (reachable_count - 1) / sum_farness.
/// Isolated nodes (reachable_count <= 1) get closeness = 0.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
closeness_centrality_execute(const AlgorithmContext& ctx);

} // namespace sixseven
