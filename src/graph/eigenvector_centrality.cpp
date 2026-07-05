#include "sixseven/graph/eigenvector_centrality.h"

#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/value_extract.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace sixseven {

namespace {

/// Extract an int64_t from a Value for the iterations parameter.
Result<int64_t> value_to_iterations(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL parameter value");
    }
    return std::visit(
        [](const auto& val) -> Result<int64_t> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> ||
                          std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
                return ok(static_cast<int64_t>(val));
            } else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                                 std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>) {
                return ok(static_cast<int64_t>(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR,
                                  "expected integer value for iterations parameter");
            }
        },
        v.data());
}

/// Find weakly connected components using union-find.
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

AlgorithmDef make_eigenvector_centrality_def() {
    AlgorithmDef def;
    def.name = "eigenvector";
    def.params = {
        {"iterations", TypeId::INT64, false, Value(static_cast<int64_t>(100))},
        {"tolerance", TypeId::FLOAT64, false, Value(1e-6)},
    };
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"score", TypeId::FLOAT64, false},
        {"iterations_used", TypeId::INT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> eigenvector_centrality_execute(const AlgorithmContext& ctx) {
    // Extract parameters.
    int64_t max_iterations = 100;
    double tolerance = 1e-6;

    if (auto it = ctx.named_args.find("iterations"); it != ctx.named_args.end()) {
        auto i = value_to_iterations(it->second);
        if (!i.has_value()) {
            return tl::unexpected(i.error());
        }
        max_iterations = *i;
    }

    if (auto it = ctx.named_args.find("tolerance"); it != ctx.named_args.end()) {
        auto t = value_to_double(it->second, "tolerance parameter");
        if (!t.has_value()) {
            return tl::unexpected(t.error());
        }
        tolerance = *t;
    }

    if (max_iterations < 1) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "iterations must be >= 1; got " + std::to_string(max_iterations));
    }

    if (tolerance < 0.0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "tolerance must be >= 0");
    }

    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build undirected adjacency list, collect all nodes, and compute components.
    // Eigenvector centrality operates on the undirected/symmetric adjacency matrix.
    std::unordered_map<int64_t, std::unordered_set<int64_t>> adj;
    std::unordered_set<int64_t> all_nodes;
    UnionFind uf;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk, "eigenvector centrality");
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk, "eigenvector centrality");
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }
        adj[*src].insert(*tgt);
        adj[*tgt].insert(*src);
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
        uf.unite(*src, *tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // Group nodes by component.
    std::unordered_map<int64_t, std::vector<int64_t>> components;
    for (int64_t node : all_nodes) {
        components[uf.find(node)].push_back(node);
    }

    // Run power iteration independently per component.
    // Uses (A + I) matrix to guarantee convergence on bipartite graphs.
    // Adding the identity matrix shifts eigenvalues from λ to λ+1,
    // ensuring the dominant eigenvalue is strictly larger in absolute
    // value than all others.  The eigenvectors are unchanged.
    std::unordered_map<int64_t, double> scores;
    std::unordered_map<int64_t, int64_t> iters_used;

    for (auto& [root, comp_nodes] : components) {
        auto n = static_cast<double>(comp_nodes.size());

        // Single-node component: score 0 (no edges to contribute).
        if (comp_nodes.size() == 1) {
            scores[comp_nodes[0]] = 0.0;
            iters_used[comp_nodes[0]] = 0;
            continue;
        }

        // Initialise x = 1/sqrt(n) for each node in the component.
        std::unordered_map<int64_t, double> x;
        double init = 1.0 / std::sqrt(n);
        for (int64_t node : comp_nodes) {
            x[node] = init;
        }

        int64_t iter = 0;
        for (; iter < max_iterations; ++iter) {
            // Compute x_new = (A + I) * x.
            // The identity contribution (x_new[node] += x[node]) prevents
            // oscillation on bipartite graphs.
            std::unordered_map<int64_t, double> x_new;
            for (int64_t node : comp_nodes) {
                x_new[node] = x[node]; // Identity contribution.
            }

            for (int64_t node : comp_nodes) {
                auto adj_it = adj.find(node);
                if (adj_it == adj.end()) {
                    continue;
                }
                for (int64_t neighbor : adj_it->second) {
                    x_new[neighbor] += x[node];
                }
            }

            // Compute L2 norm of x_new.
            double norm = 0.0;
            for (int64_t node : comp_nodes) {
                norm += x_new[node] * x_new[node];
            }
            norm = std::sqrt(norm);

            // Normalise: x_new = x_new / ||x_new||.
            if (norm > 0.0) {
                for (int64_t node : comp_nodes) {
                    x_new[node] /= norm;
                }
            }

            // Convergence check: ||x_new - x|| < tolerance.
            double diff = 0.0;
            for (int64_t node : comp_nodes) {
                double d = x_new[node] - x[node];
                diff += d * d;
            }
            diff = std::sqrt(diff);

            x = std::move(x_new);

            if (diff < tolerance) {
                ++iter; // count this iteration
                break;
            }
        }

        for (int64_t node : comp_nodes) {
            scores[node] = x[node];
            iters_used[node] = iter;
        }
    }

    // Normalise so the maximum score across all nodes is 1.0.
    double max_score = 0.0;
    for (const auto& [node, score] : scores) {
        max_score = std::max(max_score, std::abs(score));
    }
    if (max_score > 0.0) {
        for (auto& [node, score] : scores) {
            score = std::abs(score) / max_score;
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
            Value(scores[node]),
            Value(iters_used[node]),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
