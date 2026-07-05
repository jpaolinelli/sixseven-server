#include "sixseven/graph/louvain.h"

#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/value_extract.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace sixseven {

namespace {

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
///
/// Edge weight convention (consistent across all levels):
///   - An undirected edge (u,v,w) with u!=v: adj_[u][v]+=w, adj_[v][u]+=w,
///     total_weight_+=w, degree_[u]+=w, degree_[v]+=w.
///   - A self-loop (u,u,w) at coarsened levels (intra-community weight):
///     add_edge(u,u,w) stores adj_[u][u]+=2w, degree_[u]+=2w, total_weight_+=w.
///     This matches standard Louvain: a self-loop counts 2w toward degree and w
///     toward total weight, which is correct for the sigma_tot/m2 term.
///     edges_to_community() SKIPS self-loops (neighbor==node) so they do not
///     distort the ki_in numerator — self-loops move with the super-node and are
///     not part of the inter-community edge weight in the gain formula.
///   - At the ORIGINAL level, self-loops from input are skipped in
///     community_detect_execute (src==tgt guard), preserving the SelfLoopIgnored
///     contract: input self-loops do not affect degree or modularity at level 0.
///   - At COARSENED levels, self-loops introduced by Phase-2 aggregation participate
///     fully in degree and modularity — they encode intra-community weight.
class LouvainGraph { // NOLINT(bugprone-exception-escape)
public:
    /// Add an undirected edge (or self-loop) with the given weight.
    /// When u==v: adj_[u][u] accumulates 2*weight, degree_[u] accumulates 2*weight,
    /// total_weight_ accumulates weight. See class-level doc for the convention.
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

/// Compute the sum of edge weights from node to all OTHER nodes in the given community.
/// Self-loops (neighbor == node) are explicitly excluded because:
///   - At the original level, self-loops are not added to the graph (src==tgt guard).
///   - At coarsened levels, adj_[node][node]=2w represents intra-community weight that
///     moves with the super-node regardless of which community it joins. Including it
///     would give a biased incentive to stay in the current community that is not part
///     of the standard Louvain gain formula (Blondel et al. 2008).
/// The self-loop does contribute to ki (degree) correctly for sigma_tot/m2 accounting.
double edges_to_community(const LouvainGraph& graph,
                          int64_t node,
                          int64_t community,
                          const std::unordered_map<int64_t, int64_t>& node_to_community) {
    double weight = 0.0;
    for (const auto& [neighbor, w] : graph.neighbors(node)) {
        if (neighbor == node) {
            continue; // Self-loop: not counted in inter-community edge weight.
        }
        auto it = node_to_community.find(neighbor);
        if (it != node_to_community.end() && it->second == community) {
            weight += w;
        }
    }
    return weight;
}

/// Run one pass of Phase 1: local node-community reassignment (greedy modularity optimization).
///
/// Termination: canonical Blondel 2008 — stop when one full pass over all nodes produces
/// no node move. max_iterations bounds the number of such passes per invocation.
///
/// At coarsened levels, ki (node degree) includes self-loop contributions (each self-loop
/// adds 2w to degree), which is correct for the sigma_tot/m2 term. ki_in is computed by
/// edges_to_community which EXCLUDES self-loops — they move with the node and should not
/// appear in the modularity gain numerator (Blondel et al. 2008, standard treatment).
///
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

                // Modularity gain formula (Blondel et al. 2008):
                //   delta_Q proportional to: ki_in - resolution * ki * sigma_tot / m2
                // where m2 = 2 * total_weight. The /m scaling is constant and dropped.
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

        // Canonical Blondel 2008 termination: stop when no node moved in a full pass.
        if (!changed_this_pass) {
            break;
        }
    }

    return any_changed;
}

/// Phase 2 — community aggregation (coarsening).
///
/// Build a coarsened LouvainGraph where each community from the current partition
/// becomes a single super-node. Intra-community edge weight accumulates as a self-loop
/// on the super-node; inter-community edge weight accumulates as a weighted edge.
///
/// Super-node IDs are assigned in ascending order of the minimum original node in each
/// community, ensuring deterministic output across runs.
///
/// Self-loop handling: a self-loop in the current graph is stored as adj_[u][u]=2w
/// (from add_edge(u,u,w)). When building the coarsened graph we process each
/// undirected edge once (v>=u), halve stored_w for self-loops to recover w, then
/// call add_edge(su, su, w) which re-stores it as 2w at the new level. This keeps
/// the self-loop convention consistent across all levels.
///
/// Returns the coarsened graph and a mapping: old node ID -> new super-node ID.
std::pair<LouvainGraph, std::unordered_map<int64_t, int64_t>>
louvain_phase2(const LouvainGraph& graph,
               const std::unordered_map<int64_t, int64_t>& node_to_community) {
    // Assign contiguous super-node IDs by sorting communities on their minimum member node.
    std::unordered_map<int64_t, int64_t> comm_to_super;
    {
        std::unordered_map<int64_t, int64_t> comm_min_node;
        for (const auto& [node, comm] : node_to_community) {
            auto it = comm_min_node.find(comm);
            if (it == comm_min_node.end() || node < it->second) {
                comm_min_node[comm] = node;
            }
        }
        std::vector<std::pair<int64_t, int64_t>> sorted_comms; // (min_node, comm)
        sorted_comms.reserve(comm_min_node.size());
        for (const auto& [comm, min_node] : comm_min_node) {
            sorted_comms.emplace_back(min_node, comm);
        }
        std::sort(sorted_comms.begin(), sorted_comms.end());
        int64_t super_id = 0;
        for (const auto& [_, comm] : sorted_comms) {
            comm_to_super[comm] = super_id++;
        }
    }

    // Build mapping: old node -> super-node.
    std::unordered_map<int64_t, int64_t> node_to_super;
    node_to_super.reserve(node_to_community.size());
    for (const auto& [node, comm] : node_to_community) {
        node_to_super[node] = comm_to_super.at(comm);
    }

    // Build the coarsened graph.
    LouvainGraph coarsened;

    // Register all super-nodes first (handles communities with no inter-community edges).
    for (int64_t s = 0; s < std::ssize(comm_to_super); ++s) {
        coarsened.add_node(s);
    }

    // Process each undirected edge once (v >= u) to avoid double-counting.
    auto sorted_nodes = graph.nodes();
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    for (int64_t u : sorted_nodes) {
        int64_t su = node_to_super.at(u);
        for (const auto& [v, stored_w] : graph.neighbors(u)) {
            if (v < u) {
                continue;
            }
            int64_t sv = node_to_super.at(v);
            if (u == v) {
                // Self-loop: stored as 2w by add_edge(u,u,w); recover actual w = stored_w/2.
                coarsened.add_edge(su, su, stored_w / 2.0);
            } else {
                // Regular edge: stored_w is the actual edge weight.
                coarsened.add_edge(su, sv, stored_w);
            }
        }
    }

    return {std::move(coarsened), std::move(node_to_super)};
}

/// Renumber community IDs to be contiguous starting from 0.
/// IDs are assigned in order of first appearance scanning nodes in ascending order.
std::unordered_map<int64_t, int64_t>
renumber_communities(const std::unordered_map<int64_t, int64_t>& node_to_community) {
    std::unordered_map<int64_t, int64_t> old_to_new;
    int64_t next_id = 0;

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
        auto r = value_to_double(it->second, "parameter");
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
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build undirected weighted graph (level 0).
    // At the original level, self-loops (src==tgt) are explicitly skipped so that
    // input self-loops do not affect degree or modularity (SelfLoopIgnored contract).
    // seen_edges deduplication prevents double-counting when the graph engine returns
    // both directed orientations of the same undirected edge.
    LouvainGraph graph;
    std::set<std::pair<int64_t, int64_t>> seen_edges;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk, "community_detect");
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk, "community_detect");
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }

        graph.add_node(*src);
        graph.add_node(*tgt);

        if (*src != *tgt) {
            int64_t lo = std::min(*src, *tgt);
            int64_t hi = std::max(*src, *tgt);
            if (seen_edges.emplace(lo, hi).second) {
                graph.add_edge(*src, *tgt, 1.0);
            }
        }
    }

    if (graph.num_nodes() == 0) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // -------------------------------------------------------------------------
    // Multi-level Louvain (Blondel et al. 2008)
    //
    // Outer loop:
    //   1. Each node starts in its own singleton community.
    //   2. Phase 1: greedy local moves until no node moves in a full pass.
    //   3. If no node moved -> converged, stop.
    //   4. Phase 2: aggregate communities into super-nodes (intra-community
    //      edges become self-loops on the super-node).
    //   5. Compose original_to_super: each original node tracks its super-node
    //      in the new graph.
    //   6. Repeat on the coarsened graph.
    //
    // Termination: canonical Blondel 2008 — stop when Phase 1 makes no move.
    // Safety cap: MAX_LEVELS=25 (coarsening always reduces node count so
    // this is never reached on any real graph).
    //
    // max_iterations bounds the Phase-1 inner passes per level.
    // -------------------------------------------------------------------------

    // Initialize each node in its own singleton community.
    std::unordered_map<int64_t, int64_t> node_to_community;
    for (int64_t node : graph.nodes()) {
        node_to_community[node] = node;
    }

    // original_to_super[n] = super-node ID of original node n in current_graph.
    // Starts as identity (original nodes are their own super-nodes at level 0).
    std::unordered_map<int64_t, int64_t> original_to_super;
    for (int64_t node : graph.nodes()) {
        original_to_super[node] = node;
    }

    LouvainGraph* current_graph = &graph;
    LouvainGraph coarsened_storage;

    static constexpr int max_level_cap = 25;

    for (int level = 0; level < max_level_cap; ++level) {
        bool any_moved =
            louvain_phase1(*current_graph, node_to_community, resolution, max_iterations);

        if (!any_moved) {
            // No improvement — algorithm has converged.
            break;
        }

        // Build the coarsened graph and the old->new super-node mapping.
        auto [next_graph, node_to_super] = louvain_phase2(*current_graph, node_to_community);

        // Compose: for each original node, follow its super-node through the new mapping.
        for (auto& [orig, super] : original_to_super) {
            super = node_to_super.at(super);
        }

        coarsened_storage = std::move(next_graph);
        current_graph = &coarsened_storage;

        // Re-initialize: each super-node in its own singleton community.
        node_to_community.clear();
        for (int64_t snode : current_graph->nodes()) {
            node_to_community[snode] = snode;
        }

        if (current_graph->num_nodes() == 1) {
            break; // Nothing left to coarsen.
        }
    }

    // Compose final community for each original node:
    //   original -> super-node in current_graph -> community from last Phase 1.
    std::unordered_map<int64_t, int64_t> final_node_to_community;
    final_node_to_community.reserve(original_to_super.size());
    for (const auto& [orig, super] : original_to_super) {
        final_node_to_community[orig] = node_to_community.at(super);
    }

    // Renumber to contiguous IDs starting from 0.
    auto final_communities = renumber_communities(final_node_to_community);

    // Output rows sorted by node_id ascending.
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
