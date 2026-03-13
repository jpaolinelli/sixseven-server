#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the triangle count algorithm.
/// Output schema: (node_id INT64, triangle_count INT64).
/// No parameters.
[[nodiscard]] AlgorithmDef make_triangle_count_def();

/// Execute function for the triangle count algorithm.
/// Counts the number of triangles each node participates in.
///
/// Uses node-iterator with degree-ordering optimization: for each node v,
/// only check neighbors with higher degree (breaking ties by node id).
/// This avoids counting each triangle more than once per node.
///
/// O(V * d_max^2) worst case where d_max is the maximum degree.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
triangle_count_execute(const AlgorithmContext& ctx);

} // namespace sixseven
