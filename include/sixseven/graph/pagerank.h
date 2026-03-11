#pragma once

#include "sixseven/graph/algorithm_registry.h"

namespace sixseven {

/// Return the AlgorithmDef for the pagerank algorithm.
/// Output schema: (node_id INT64, score FLOAT64).
/// Parameters: damping FLOAT64 (default 0.85), iterations INT64 (default 20).
[[nodiscard]] AlgorithmDef make_pagerank_def();

/// Execute function for the pagerank algorithm.
/// Uses the iterative power method with configurable damping and iterations.
[[nodiscard]] Result<std::vector<AlgorithmRow>> pagerank_execute(const AlgorithmContext& ctx);

} // namespace sixseven
