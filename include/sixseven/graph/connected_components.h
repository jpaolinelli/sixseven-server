#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the connected_components algorithm.
/// Output schema: (node_id INT64, component_id INT64).
[[nodiscard]] AlgorithmDef make_connected_components_def();

/// Execute function for the connected_components algorithm.
/// Uses Union-Find with path compression and union by rank.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
connected_components_execute(const AlgorithmContext& ctx);

} // namespace sixseven
