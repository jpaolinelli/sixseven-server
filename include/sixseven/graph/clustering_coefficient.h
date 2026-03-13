#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the clustering coefficient algorithm.
/// Output schema: (node_id INT64, local_coefficient FLOAT64, triangles INT64, degree INT64).
/// No parameters.
[[nodiscard]] AlgorithmDef make_clustering_coefficient_def();

/// Execute function for the clustering coefficient algorithm.
/// Computes the local clustering coefficient for every node.
///
/// Formula: C(v) = 2T(v) / (k(v) * (k(v) - 1))
///   T(v) = number of triangles through v
///   k(v) = degree of v (total degree for directed graphs)
///
/// Nodes with degree < 2 have coefficient = 0.
/// O(V * d_max^2) worst case where d_max is the maximum degree.
[[nodiscard]] Result<std::vector<AlgorithmRow>>
clustering_coefficient_execute(const AlgorithmContext& ctx);

} // namespace sixseven
