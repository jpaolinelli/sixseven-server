#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the closeness centrality algorithm.
/// Output schema: (node_id INT64, closeness FLOAT64, sum_farness INT64,
///                 reachable_count INT64, component_size INT64,
///                 normalized_closeness FLOAT64).
/// Parameter: variant STRING (default "standard", also "wasserman_faust").
[[nodiscard]] AlgorithmDef make_closeness_centrality_def();

/// Execute function for the closeness centrality algorithm.
/// Computes closeness centrality via BFS from every node in O(V*(V+E)) time.
///
/// Standard variant:
///   C(v) = (reachable_count - 1) / sum_farness
///
/// Wasserman-Faust variant (for disconnected graphs):
///   C_WF(v) = [(n_c - 1)/(N - 1)] * [(n_c - 1) / sum_farness]
///   where n_c = component size, N = total nodes in graph.
///   Scales closeness by component size so larger-component nodes rank higher.
///
/// Isolated nodes (reachable_count <= 1) get closeness = 0.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
closeness_centrality_execute(const AlgorithmContext& ctx);

} // namespace sixseven
