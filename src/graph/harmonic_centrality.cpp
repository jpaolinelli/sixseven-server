#include "sixseven/graph/harmonic_centrality.h"

#include "sixseven/graph/graph_engine.h"

#include <algorithm>
#include <queue>
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
                                  "harmonic centrality requires integer node keys");
            }
        },
        v.data());
}

} // namespace

AlgorithmDef make_harmonic_centrality_def() {
    AlgorithmDef def;
    def.name = "harmonic";
    def.params = {};
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"harmonic", TypeId::FLOAT64, false},
        {"normalized_harmonic", TypeId::FLOAT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> harmonic_centrality_execute(const AlgorithmContext& ctx) {
    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build adjacency list and collect all nodes.
    std::unordered_map<int64_t, std::vector<int64_t>> adj;
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
        adj[*src].push_back(*tgt);
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    auto total_nodes = static_cast<int64_t>(all_nodes.size());

    // For each node, run BFS to compute shortest-path distances and harmonic sum.
    std::unordered_map<int64_t, double> harmonic;
    std::unordered_map<int64_t, double> normalized;

    for (int64_t source : all_nodes) {
        std::unordered_map<int64_t, int64_t> dist;
        dist[source] = 0;

        std::queue<int64_t> bfs_queue;
        bfs_queue.push(source);

        double reciprocal_sum = 0.0;

        while (!bfs_queue.empty()) {
            int64_t v = bfs_queue.front();
            bfs_queue.pop();

            auto adj_it = adj.find(v);
            if (adj_it == adj.end()) {
                continue;
            }

            for (int64_t w : adj_it->second) {
                if (dist.find(w) == dist.end()) {
                    dist[w] = dist[v] + 1;
                    reciprocal_sum += 1.0 / static_cast<double>(dist[w]);
                    bfs_queue.push(w);
                }
            }
        }

        harmonic[source] = reciprocal_sum;

        if (total_nodes > 1) {
            normalized[source] = reciprocal_sum / static_cast<double>(total_nodes - 1);
        } else {
            normalized[source] = 0.0;
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
            Value(harmonic[node]),
            Value(normalized[node]),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
