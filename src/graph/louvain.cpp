#include "sixseven/graph/louvain.h"

#include "sixseven/graph/graph_engine.h"

#include <algorithm>
#include <cmath>
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
                                  "community_detect requires integer node keys");
            }
        },
        v.data());
}

/// Extract a double from a Value that may hold an integer or floating-point type.
Result<double> value_to_double(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL parameter value");
    }
    return std::visit(
        [](const auto& val) -> Result<double> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_arithmetic_v<T>) {
                return ok(static_cast<double>(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR, "expected numeric value for parameter");
            }
        },
        v.data());
}

/// Extract an int64_t from a Value for integer parameters.
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
                                  "expected integer value for max_iterations parameter");
            }
        },
        v.data());
}

/// Undirected weighted graph for Louvain community detection.
/// Edges are stored as adjacency lists with weights. Since the graph is
/// undirected, each edge (u,v) is stored in both directions.
class LouvainGraph {
public:
    /// Add an undirected edge between u and v with the given weight.
    /// If the edge already exists, the weight is accumulated.
    void add_edge(int64_t u, int64_t v, double weight) {
        adj_[u][v] += weight;
        adj_[v][u] += weight;
        total_weight_ += weight;
        degree_[u] += weight;
        degree_[v] += weight;
    }

    /// Register a node (even if it has no edges).
    void add_node(int64_t n) {
        adj_.try_emplace(n);
        degree_.try_emplace(n, 0.0);
    }

    [[nodiscard]] const std::unordered_map<int64_t, double>& neighbors(int64_t n) const {
        static const std::unordered_map<int64_t, double> empty;
        auto it = adj_.find(n);
        return (it != adj_.end()) ? it->second : empty;
    }

    [[nodiscard]] double degree(int64_t n) const {
        auto it = degree_.find(n);
        return (it != degree_.end()) ? it->second : 0.0;
    }

    [[nodiscard]] double total_weight() const { return total_weight_; }

    [[nodiscard]] std::vector<int64_t> nodes() const {
        std::vector<int64_t> result;
        result.reserve(adj_.size());
        for (const auto& [n, _] : adj_) {
            result.push_back(n);
        }
        return result;
    }

    [[nodiscard]] size_t num_nodes() const { return adj_.size(); }

private:
    std::unordered_map<int64_t, std::unordered_map<int64_t, double>> adj_;
    std::unordered_map<int64_t, double> degree_;
    double total_weight_ = 0.0;
};

/// Compute the sum of edge weights from node to all nodes in the given community.
double edges_to_community(const LouvainGraph& graph,
                          int64_t node,
                          int64_t community,
                          const std::unordered_map<int64_t, int64_t>& node_to_community) {
    double weight = 0.0;
    for (const auto& [neighbor, w] : graph.neighbors(node)) {
        auto it = node_to_community.find(neighbor);
        if (it != node_to_community.end() && it->second == community) {
            weight += w;
        }
    }
    return weight;
}

/// Run one pass of Phase 1: local node-community reassignment.
/// Returns true if any node changed community.
bool louvain_phase1(const LouvainGraph& graph,
                    std::unordered_map<int64_t, int64_t>& node_to_community,
                    double resolution,
                    int64_t max_iterations) {
    double m2 = 2.0 * graph.total_weight();
    if (m2 == 0.0) {
        return false;
    }

    // Pre-compute community degrees for efficiency.
    std::unordered_map<int64_t, double> comm_degree;
    for (const auto& [node, comm] : node_to_community) {
        comm_degree[comm] += graph.degree(node);
    }

    bool any_changed = false;
    for (int64_t iter = 0; iter < max_iterations; ++iter) {
        bool changed_this_pass = false;

        auto all_nodes = graph.nodes();
        std::sort(all_nodes.begin(), all_nodes.end());

        for (int64_t node : all_nodes) {
            int64_t current_comm = node_to_community[node];
            double ki = graph.degree(node);

            // Remove node from its community for evaluation.
            comm_degree[current_comm] -= ki;

            // Compute modularity gain for each neighboring community.
            double best_gain = 0.0;
            int64_t best_comm = current_comm;

            // Collect unique neighboring communities.
            std::unordered_set<int64_t> neighbor_comms;
            for (const auto& [neighbor, _] : graph.neighbors(node)) {
                neighbor_comms.insert(node_to_community[neighbor]);
            }
            // Also consider staying in the current community.
            neighbor_comms.insert(current_comm);

            for (int64_t candidate_comm : neighbor_comms) {
                double ki_in = edges_to_community(graph, node, candidate_comm, node_to_community);
                double sigma_tot = comm_degree[candidate_comm];

                // Modularity gain:
                // delta_Q = ki_in / m - resolution * ki * sigma_tot / (m^2 / 2)
                // Simplified: delta_Q = ki_in / m - resolution * ki * sigma_tot / (2 * m^2)
                // Using m2 = 2m:
                // delta_Q = 2*ki_in / m2 - resolution * ki * sigma_tot / (m2 * m2 / 2)
                double gain = ki_in - resolution * ki * sigma_tot / m2;

                if (gain > best_gain) {
                    best_gain = gain;
                    best_comm = candidate_comm;
                }
            }

            // Move node to the best community.
            comm_degree[best_comm] += ki;
            if (best_comm != current_comm) {
                node_to_community[node] = best_comm;
                changed_this_pass = true;
                any_changed = true;
            }
        }

        if (!changed_this_pass) {
            break;
        }
    }

    return any_changed;
}

/// Renumber community IDs to be contiguous starting from 0.
std::unordered_map<int64_t, int64_t>
renumber_communities(const std::unordered_map<int64_t, int64_t>& node_to_community) {
    std::unordered_map<int64_t, int64_t> old_to_new;
    int64_t next_id = 0;

    // Assign new IDs in order of first appearance by sorted node.
    std::vector<int64_t> sorted_nodes;
    sorted_nodes.reserve(node_to_community.size());
    for (const auto& [node, _] : node_to_community) {
        sorted_nodes.push_back(node);
    }
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    for (int64_t node : sorted_nodes) {
        int64_t comm = node_to_community.at(node);
        if (old_to_new.find(comm) == old_to_new.end()) {
            old_to_new[comm] = next_id++;
        }
    }

    std::unordered_map<int64_t, int64_t> result;
    for (const auto& [node, comm] : node_to_community) {
        result[node] = old_to_new[comm];
    }
    return result;
}

} // namespace

AlgorithmDef make_community_detect_def() {
    AlgorithmDef def;
    def.name = "community_detect";
    def.params = {
        {"resolution", TypeId::FLOAT64, false, Value(1.0)},
        {"max_iterations", TypeId::INT64, false, Value(static_cast<int64_t>(10))},
    };
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"community_id", TypeId::INT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> community_detect_execute(const AlgorithmContext& ctx) {
    // Extract parameters.
    double resolution = 1.0;
    int64_t max_iterations = 10;

    if (auto it = ctx.named_args.find("resolution"); it != ctx.named_args.end()) {
        auto r = value_to_double(it->second);
        if (!r.has_value()) {
            return tl::unexpected(r.error());
        }
        resolution = *r;
    }

    if (auto it = ctx.named_args.find("max_iterations"); it != ctx.named_args.end()) {
        auto i = value_to_iterations(it->second);
        if (!i.has_value()) {
            return tl::unexpected(i.error());
        }
        max_iterations = *i;
    }

    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build undirected weighted graph.
    LouvainGraph graph;
    std::unordered_set<int64_t> seen_edges;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk);
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk);
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }

        graph.add_node(*src);
        graph.add_node(*tgt);

        if (*src != *tgt) {
            // Use a canonical edge key to avoid double-counting directed edges
            // that represent the same undirected edge.
            int64_t lo = std::min(*src, *tgt);
            int64_t hi = std::max(*src, *tgt);
            // Hash-combine for a unique edge key.
            auto edge_key = static_cast<int64_t>((static_cast<uint64_t>(lo) << 32) |
                                                 (static_cast<uint64_t>(hi) & 0xFFFFFFFF));
            if (seen_edges.insert(edge_key).second) {
                graph.add_edge(*src, *tgt, 1.0);
            }
        }
    }

    if (graph.num_nodes() == 0) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // Initialize: each node in its own community.
    std::unordered_map<int64_t, int64_t> node_to_community;
    for (int64_t node : graph.nodes()) {
        node_to_community[node] = node;
    }

    // Run Phase 1 of Louvain: iterative local optimization.
    louvain_phase1(graph, node_to_community, resolution, max_iterations);

    // Renumber communities to contiguous IDs starting from 0.
    auto final_communities = renumber_communities(node_to_community);

    // Build sorted output rows.
    auto sorted_nodes = graph.nodes();
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());
    for (int64_t node : sorted_nodes) {
        rows.push_back({std::vector<Value>{Value(node), Value(final_communities[node])}});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
