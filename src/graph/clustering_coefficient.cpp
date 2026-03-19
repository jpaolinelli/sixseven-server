#include "sixseven/graph/clustering_coefficient.h"

#include "sixseven/graph/graph_engine.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace sixseven {

namespace {

/// Convert a Value holding an integer type to int64_t.
Result<int64_t> value_to_int64(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL node key in edge");
    }
    return std::visit(
        [](const auto& val) -> Result<int64_t> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> ||
                          std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
                return ok(static_cast<int64_t>(val));
            } else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                                 std::is_same_v<T, uint32_t>) {
                return ok(static_cast<int64_t>(val));
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                return ok(static_cast<int64_t>(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR,
                                  "clustering coefficient requires integer node keys");
            }
        },
        v.data());
}

} // namespace

AlgorithmDef make_clustering_coefficient_def() {
    AlgorithmDef def;
    def.name = "clustering_coefficient";
    def.params = {};
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"local_coefficient", TypeId::FLOAT64, false},
        {"triangles", TypeId::INT64, false},
        {"degree", TypeId::INT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> clustering_coefficient_execute(const AlgorithmContext& ctx) {
    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build adjacency sets (undirected view using total degree for directed graphs)
    // and track all nodes.
    std::unordered_map<int64_t, std::unordered_set<int64_t>> neighbors;
    std::unordered_set<int64_t> all_nodes;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk);
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk);
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

    // Compute clustering coefficient for each node.
    struct NodeResult {
        double coefficient = 0.0;
        int64_t triangles = 0;
        int64_t degree = 0;
    };
    std::unordered_map<int64_t, NodeResult> results;

    for (int64_t node : all_nodes) {
        auto it = neighbors.find(node);
        if (it == neighbors.end()) {
            results[node] = {0.0, 0, 0};
            continue;
        }

        const auto& nbrs = it->second;
        auto degree = static_cast<int64_t>(nbrs.size());

        if (degree < 2) {
            results[node] = {0.0, 0, degree};
            continue;
        }

        // Count triangles: for each pair of neighbors, check if they are connected.
        int64_t triangle_count = 0;
        std::vector<int64_t> nbr_list(nbrs.begin(), nbrs.end());
        for (size_t i = 0; i < nbr_list.size(); ++i) {
            for (size_t j = i + 1; j < nbr_list.size(); ++j) {
                auto nj_it = neighbors.find(nbr_list[i]);
                if (nj_it != neighbors.end() && nj_it->second.count(nbr_list[j]) > 0) {
                    ++triangle_count;
                }
            }
        }

        // C(v) = 2T(v) / (k(v) * (k(v) - 1))
        double coefficient = (2.0 * static_cast<double>(triangle_count)) /
                             (static_cast<double>(degree) * static_cast<double>(degree - 1));

        results[node] = {coefficient, triangle_count, degree};
    }

    // Build sorted output rows.
    std::vector<int64_t> sorted_nodes(all_nodes.begin(), all_nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());
    for (int64_t node : sorted_nodes) {
        const auto& r = results[node];
        rows.push_back({std::vector<Value>{
            Value(node),
            Value(r.coefficient),
            Value(r.triangles),
            Value(r.degree),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
