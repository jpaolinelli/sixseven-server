#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the strongly_connected_components algorithm.
/// Output schema: (node_id INT64, component_id INT64, component_size INT64).
/// No parameters.
[[nodiscard]] AlgorithmDef make_strongly_connected_components_def();

/// Execute function for the strongly_connected_components algorithm.
/// Identifies maximal sets of nodes where every node is reachable from every
/// other node in directed graphs using Tarjan's algorithm.
///
/// Single DFS pass, O(V + E).
/// component_id is the smallest node_id in each component (stable, deterministic).
/// For undirected graphs (both directions present): equivalent to connected components.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
strongly_connected_components_execute(const AlgorithmContext& ctx);

} // namespace sixseven
