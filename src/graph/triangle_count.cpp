#include "sixseven/graph/triangle_count.h"

#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/value_extract.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace sixseven {

AlgorithmDef make_triangle_count_def() {
    AlgorithmDef def;
    def.name = "triangle_count";
    def.params = {};
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"triangle_count", TypeId::INT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> triangle_count_execute(const AlgorithmContext& ctx) {
    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build adjacency sets (undirected view) and track all nodes.
    std::unordered_map<int64_t, std::unordered_set<int64_t>> neighbors;
    std::unordered_set<int64_t> all_nodes;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk, "triangle count");
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk, "triangle count");
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }

        // Skip self-loops — they don't contribute to triangles.
        if (*src == *tgt) {
            all_nodes.insert(*src);
            continue;
        }

        neighbors[*src].insert(*tgt);
        neighbors[*tgt].insert(*src);
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // Count triangles per node using degree-ordering optimization.
    // For each edge (u, v) where u < v in the ordering, check common neighbors.
    // A node is "lower" if it has smaller degree, breaking ties by node id.
    // This ensures each triangle is found exactly once.

    // Precompute degrees for ordering.
    auto degree_order = [&neighbors](int64_t a, int64_t b) -> bool {
        auto deg_a = neighbors.count(a) > 0 ? neighbors.at(a).size() : 0u;
        auto deg_b = neighbors.count(b) > 0 ? neighbors.at(b).size() : 0u;
        if (deg_a != deg_b) {
            return deg_a < deg_b;
        }
        return a < b;
    };

    std::unordered_map<int64_t, int64_t> triangle_counts;
    for (int64_t node : all_nodes) {
        triangle_counts[node] = 0;
    }

    // For each node u, iterate over neighbors v where v is "higher" in degree order.
    // For each such (u, v), count common neighbors w that are "higher" than both.
    // Each triangle {u, v, w} is discovered exactly once. Credit all three nodes.
    for (int64_t u : all_nodes) {
        auto u_it = neighbors.find(u);
        if (u_it == neighbors.end()) {
            continue;
        }
        const auto& u_nbrs = u_it->second;

        for (int64_t v : u_nbrs) {
            if (!degree_order(u, v)) {
                continue;
            }

            auto v_it = neighbors.find(v);
            if (v_it == neighbors.end()) {
                continue;
            }
            const auto& v_nbrs = v_it->second;

            // Find common neighbors w of u and v where w is "higher" than v.
            for (int64_t w : u_nbrs) {
                if (!degree_order(v, w)) {
                    continue;
                }
                if (v_nbrs.count(w) > 0) {
                    // Triangle {u, v, w} found. Credit each node.
                    ++triangle_counts[u];
                    ++triangle_counts[v];
                    ++triangle_counts[w];
                }
            }
        }
    }

    // Build sorted output rows.
    std::vector<int64_t> sorted_nodes(all_nodes.begin(), all_nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());
    for (int64_t node : sorted_nodes) {
        rows.push_back({std::vector<Value>{
            Value(node),
            Value(triangle_counts[node]),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
