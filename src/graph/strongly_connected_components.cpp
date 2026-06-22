#include "sixseven/graph/strongly_connected_components.h"

#include "sixseven/graph/graph_engine.h"

#include <algorithm>
#include <stack>
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
                                  "strongly_connected_components requires integer node keys");
            }
        },
        v.data());
}

} // namespace

AlgorithmDef make_strongly_connected_components_def() {
    AlgorithmDef def;
    def.name = "strongly_connected_components";
    def.params = {};
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"component_id", TypeId::INT64, false},
        {"component_size", TypeId::INT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>>
strongly_connected_components_execute(const AlgorithmContext& ctx) {
    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build directed adjacency list and collect all nodes.
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

        // Skip self-loops — a self-loop alone does not form an SCC cycle.
        if (*src == *tgt) {
            all_nodes.insert(*src);
            continue;
        }

        adj[*src].push_back(*tgt);
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    // Tarjan's algorithm — iterative to avoid stack overflow on large graphs.
    //
    // Each node gets a discovery index and a low-link value. Nodes on the
    // current DFS path are kept on a stack. When a root of an SCC is found
    // (disc == low), all nodes above it on the stack form the SCC.

    int64_t index_counter = 0;
    std::unordered_map<int64_t, int64_t> disc; // discovery index
    std::unordered_map<int64_t, int64_t> low;  // low-link value
    std::unordered_set<int64_t> on_stack;      // nodes currently on stack
    std::stack<int64_t> tarjan_stack;          // Tarjan stack

    // Map from node_id to component_id (smallest node in its SCC).
    std::unordered_map<int64_t, int64_t> component_of;

    // Process nodes in sorted order for deterministic results.
    std::vector<int64_t> sorted_nodes(all_nodes.begin(), all_nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    // Iterative Tarjan's using an explicit call stack.
    // Each frame stores: (node, index into adjacency list).
    struct Frame {
        int64_t node;
        size_t adj_idx;
    };

    for (int64_t start : sorted_nodes) {
        if (disc.count(start) > 0) {
            continue; // already visited
        }

        std::stack<Frame> call_stack;

        // Initialize start node.
        disc[start] = index_counter;
        low[start] = index_counter;
        ++index_counter;
        tarjan_stack.push(start);
        on_stack.insert(start);
        call_stack.push({start, 0});

        static const std::vector<int64_t> kEmpty;
        while (!call_stack.empty()) {
            auto& frame = call_stack.top();
            int64_t u = frame.node;
            auto adj_it = adj.find(u);
            const auto& neighbors = (adj_it != adj.end()) ? adj_it->second : kEmpty;

            if (frame.adj_idx < neighbors.size()) {
                int64_t w = neighbors[frame.adj_idx];
                ++frame.adj_idx;

                if (disc.count(w) == 0) {
                    // Tree edge: push w and recurse.
                    disc[w] = index_counter;
                    low[w] = index_counter;
                    ++index_counter;
                    tarjan_stack.push(w);
                    on_stack.insert(w);
                    call_stack.push({w, 0});
                } else if (on_stack.count(w) > 0) {
                    // Back edge: update low-link.
                    low[u] = std::min(low[u], disc[w]);
                }
            } else {
                // All neighbors processed — check if u is a root.
                if (low[u] == disc[u]) {
                    // Pop SCC from tarjan_stack and find smallest node id.
                    std::vector<int64_t> scc_nodes;
                    int64_t min_id = u;
                    int64_t top = -1;
                    do {
                        top = tarjan_stack.top();
                        tarjan_stack.pop();
                        on_stack.erase(top);
                        scc_nodes.push_back(top);
                        if (top < min_id) {
                            min_id = top;
                        }
                    } while (top != u);

                    // Assign component_id = smallest node in the SCC.
                    for (int64_t n : scc_nodes) {
                        component_of[n] = min_id;
                    }
                }

                // Pop frame and propagate low-link to parent.
                call_stack.pop();
                if (!call_stack.empty()) {
                    int64_t parent = call_stack.top().node;
                    low[parent] = std::min(low[parent], low[u]);
                }
            }
        }
    }

    // Compute component sizes.
    std::unordered_map<int64_t, int64_t> comp_size;
    for (const auto& [node, comp] : component_of) {
        ++comp_size[comp];
    }

    // Build sorted output rows.
    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());
    for (int64_t node : sorted_nodes) {
        int64_t comp = component_of[node];
        rows.push_back({std::vector<Value>{
            Value(node),
            Value(comp),
            Value(comp_size[comp]),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
