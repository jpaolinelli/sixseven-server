#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the betweenness centrality algorithm.
/// Output schema: (node_id INT64, centrality FLOAT64).
/// Parameters: normalized BOOL (default true).
[[nodiscard]] AlgorithmDef make_betweenness_centrality_def();

/// Execute function for the betweenness centrality algorithm.
/// Uses Brandes' algorithm for efficient computation in O(V*E) time.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
betweenness_centrality_execute(const AlgorithmContext& ctx);

} // namespace sixseven
