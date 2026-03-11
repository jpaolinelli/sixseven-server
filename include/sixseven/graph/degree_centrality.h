#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the degree centrality algorithm.
/// Output schema: (node_id INT64, in_degree INT64, out_degree INT64,
///                 degree INT64, normalized_degree FLOAT64).
/// Parameters: direction STRING (default "all", also accepts "in" or "out").
[[nodiscard]] AlgorithmDef make_degree_centrality_def();

/// Execute function for the degree centrality algorithm.
/// Computes in-degree, out-degree, and total degree for each node in O(V+E).
[[nodiscard]] Result<std::vector<AlgorithmRow>>
degree_centrality_execute(const AlgorithmContext& ctx);

} // namespace sixseven
