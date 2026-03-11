#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the community_detect (Louvain) algorithm.
/// Output schema: (node_id INT64, community_id INT64).
/// Parameters: resolution FLOAT64 (default 1.0), max_iterations INT64 (default 10).
[[nodiscard]] AlgorithmDef make_community_detect_def();

/// Execute function for the community_detect (Louvain) algorithm.
/// Uses modularity optimization with iterative node-community reassignment.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
community_detect_execute(const AlgorithmContext& ctx);

} // namespace sixseven
