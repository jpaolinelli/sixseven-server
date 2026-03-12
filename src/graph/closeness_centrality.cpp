#include "sixseven/graph/closeness_centrality.h"

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
                                  "closeness centrality requires integer node keys");
            }
        },
        v.data());
}

/// Extract a string from a Value for the variant parameter.
Result<std::string> value_to_string(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL parameter value");
    }
    return std::visit(
        [](const auto& val) -> Result<std::string> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return ok(std::string(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR,
                                  "expected string value for variant parameter");
            }
        },
        v.data());
}

/// Find weakly connected components using union-find.
/// Treats directed edges as undirected for component membership.
class UnionFind {
public:
    int64_t find(int64_t x) {
        if (parent_.find(x) == parent_.end()) {
            parent_[x] = x;
            rank_[x] = 0;
        }
        if (parent_[x] != x) {
            parent_[x] = find(parent_[x]);
        }
        return parent_[x];
    }

    void unite(int64_t x, int64_t y) {
        int64_t rx = find(x);
        int64_t ry = find(y);
        if (rx == ry) {
            return;
        }
        if (rank_[rx] < rank_[ry]) {
            std::swap(rx, ry);
        }
        parent_[ry] = rx;
        if (rank_[rx] == rank_[ry]) {
            ++rank_[rx];
        }
    }

private:
    std::unordered_map<int64_t, int64_t> parent_;
    std::unordered_map<int64_t, int64_t> rank_;
};

} // namespace

AlgorithmDef make_closeness_centrality_def() {
    AlgorithmDef def;
    def.name = "closeness";
    def.params = {
        {"variant", TypeId::STRING, false, Value(std::string("standard"))},
    };
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"closeness", TypeId::FLOAT64, false},
        {"sum_farness", TypeId::INT64, false},
        {"reachable_count", TypeId::INT64, false},
        {"component_size", TypeId::INT64, false},
        {"normalized_closeness", TypeId::FLOAT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> closeness_centrality_execute(const AlgorithmContext& ctx) {
    // Extract variant parameter.
    std::string variant = "standard";
    if (auto it = ctx.named_args.find("variant"); it != ctx.named_args.end()) {
        auto v = value_to_string(it->second);
        if (!v.has_value()) {
            return tl::unexpected(v.error());
        }
        variant = *v;
    }

    if (variant != "standard" && variant != "wasserman_faust") {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "variant must be 'standard' or 'wasserman_faust'; got '" + variant + "'");
    }

    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build adjacency list, collect all nodes, and compute weakly connected components.
    std::unordered_map<int64_t, std::vector<int64_t>> adj;
    std::unordered_set<int64_t> all_nodes;
    UnionFind uf;

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
        uf.unite(*src, *tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // Compute component sizes.
    std::unordered_map<int64_t, int64_t> component_size;
    for (int64_t node : all_nodes) {
        component_size[uf.find(node)]++;
    }

    auto total_nodes = static_cast<int64_t>(all_nodes.size());

    // For each node, run BFS to compute shortest path distances.
    std::unordered_map<int64_t, double> closeness;
    std::unordered_map<int64_t, int64_t> farness;
    std::unordered_map<int64_t, int64_t> reachable;
    std::unordered_map<int64_t, int64_t> comp_size_map;
    std::unordered_map<int64_t, double> norm_closeness;

    for (int64_t source : all_nodes) {
        // BFS from source.
        std::unordered_map<int64_t, int64_t> dist;
        dist[source] = 0;

        std::queue<int64_t> bfs_queue;
        bfs_queue.push(source);

        int64_t sum_dist = 0;
        int64_t reachable_count = 1; // source itself

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
                    sum_dist += dist[w];
                    ++reachable_count;
                    bfs_queue.push(w);
                }
            }
        }

        farness[source] = sum_dist;
        reachable[source] = reachable_count;

        int64_t nc = component_size[uf.find(source)];
        comp_size_map[source] = nc;

        // Within-component closeness: (reachable_count - 1) / sum_farness.
        double within_closeness = 0.0;
        if (reachable_count > 1 && sum_dist > 0) {
            within_closeness =
                static_cast<double>(reachable_count - 1) / static_cast<double>(sum_dist);
        }

        if (variant == "wasserman_faust") {
            // C_WF(v) = [(n_c - 1)/(N - 1)] * [(n_c - 1)/sum_farness]
            double wf_closeness = 0.0;
            double wf_normalized = 0.0;

            if (nc > 1 && sum_dist > 0) {
                wf_normalized =
                    static_cast<double>(nc - 1) / static_cast<double>(sum_dist);
                double scaling = static_cast<double>(nc - 1) / static_cast<double>(total_nodes - 1);
                wf_closeness = scaling * wf_normalized;
            }

            closeness[source] = wf_closeness;
            norm_closeness[source] = wf_normalized;
        } else {
            // Standard variant.
            closeness[source] = within_closeness;
            norm_closeness[source] = within_closeness;
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
            Value(closeness[node]),
            Value(farness[node]),
            Value(reachable[node]),
            Value(comp_size_map[node]),
            Value(norm_closeness[node]),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
