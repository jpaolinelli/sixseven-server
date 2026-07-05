#include "sixseven/graph/betweenness_centrality.h"

#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/value_extract.h"

#include <algorithm>
#include <queue>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace sixseven {

namespace {

/// Extract a bool from a Value for the normalized parameter.
Result<bool> value_to_bool(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL parameter value");
    }
    return std::visit(
        [](const auto& val) -> Result<bool> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, bool>) {
                return ok(val);
            } else if constexpr (std::is_integral_v<T>) {
                return ok(val != 0);
            } else {
                return make_error(StatusCode::TYPE_ERROR,
                                  "expected boolean value for normalized parameter");
            }
        },
        v.data());
}

} // namespace

AlgorithmDef make_betweenness_centrality_def() {
    AlgorithmDef def;
    def.name = "betweenness";
    def.params = {
        {"normalized", TypeId::BOOL, false, Value(true)},
    };
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"centrality", TypeId::FLOAT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> betweenness_centrality_execute(const AlgorithmContext& ctx) {
    // Extract parameters.
    bool normalized = true;

    if (auto it = ctx.named_args.find("normalized"); it != ctx.named_args.end()) {
        auto n = value_to_bool(it->second);
        if (!n.has_value()) {
            return tl::unexpected(n.error());
        }
        normalized = *n;
    }

    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build adjacency list and collect all nodes (deduplicate edges).
    std::unordered_map<int64_t, std::unordered_set<int64_t>> adj_set;
    std::unordered_set<int64_t> all_nodes;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk, "betweenness centrality");
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk, "betweenness centrality");
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }
        adj_set[*src].insert(*tgt);
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
    }

    // Convert to vector-based adjacency list for BFS iteration.
    std::unordered_map<int64_t, std::vector<int64_t>> adj;
    for (auto& [node, neighbors] : adj_set) {
        adj[node].assign(neighbors.begin(), neighbors.end());
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // Initialize centrality scores to zero.
    std::unordered_map<int64_t, double> centrality;
    for (int64_t node : all_nodes) {
        centrality[node] = 0.0;
    }

    // Brandes' algorithm: BFS from each source node.
    for (int64_t source : all_nodes) {
        // Stack of nodes in order of non-increasing distance from source.
        std::stack<int64_t> stack;

        // Predecessors on shortest paths from source.
        std::unordered_map<int64_t, std::vector<int64_t>> predecessors;

        // Number of shortest paths from source to each node.
        std::unordered_map<int64_t, double> sigma;
        for (int64_t node : all_nodes) {
            sigma[node] = 0.0;
        }
        sigma[source] = 1.0;

        // Distance from source to each node (-1 = not visited).
        std::unordered_map<int64_t, int64_t> dist;
        for (int64_t node : all_nodes) {
            dist[node] = -1;
        }
        dist[source] = 0;

        // BFS queue.
        std::queue<int64_t> bfs_queue;
        bfs_queue.push(source);

        // Forward phase: BFS to compute shortest paths.
        while (!bfs_queue.empty()) {
            int64_t v = bfs_queue.front();
            bfs_queue.pop();
            stack.push(v);

            auto adj_it = adj.find(v);
            if (adj_it == adj.end()) {
                continue;
            }

            for (int64_t w : adj_it->second) {
                // First visit to w?
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    bfs_queue.push(w);
                }
                // Is edge (v, w) on a shortest path to w?
                if (dist[w] == dist[v] + 1) {
                    sigma[w] += sigma[v];
                    predecessors[w].push_back(v);
                }
            }
        }

        // Backward phase: accumulate dependency scores.
        std::unordered_map<int64_t, double> delta;
        for (int64_t node : all_nodes) {
            delta[node] = 0.0;
        }

        while (!stack.empty()) {
            int64_t w = stack.top();
            stack.pop();

            auto pred_it = predecessors.find(w);
            if (pred_it != predecessors.end()) {
                for (int64_t v : pred_it->second) {
                    delta[v] += (sigma[v] / sigma[w]) * (1.0 + delta[w]);
                }
            }

            if (w != source) {
                centrality[w] += delta[w];
            }
        }
    }

    // Normalize if requested.
    auto n = static_cast<double>(all_nodes.size());
    if (normalized && n > 2) {
        // For directed graphs, normalization factor is 1/((n-1)(n-2)).
        double norm = 1.0 / ((n - 1.0) * (n - 2.0));
        for (auto& [node, score] : centrality) {
            score *= norm;
        }
    }

    // Build sorted output rows.
    std::vector<int64_t> sorted_nodes(all_nodes.begin(), all_nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());
    for (int64_t node : sorted_nodes) {
        rows.push_back({std::vector<Value>{Value(node), Value(centrality[node])}});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
