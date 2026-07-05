#include "sixseven/graph/connected_components.h"

#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/value_extract.h"

#include <algorithm>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sixseven {

namespace {

/// Union-Find (Disjoint Set Union) with path compression and union by rank.
class UnionFind {
public:
    /// Find the root representative of the set containing x.
    int64_t find(int64_t x) {
        auto it = parent_.find(x);
        if (it == parent_.end()) {
            // First time seeing this node — it's its own parent.
            parent_[x] = x;
            rank_[x] = 0;
            return x;
        }
        if (it->second != x) {
            it->second = find(it->second); // path compression
        }
        return it->second;
    }

    /// Unite the sets containing x and y.
    void unite(int64_t x, int64_t y) {
        int64_t rx = find(x);
        int64_t ry = find(y);
        if (rx == ry) {
            return;
        }
        // Union by rank.
        if (rank_[rx] < rank_[ry]) {
            parent_[rx] = ry;
        } else if (rank_[rx] > rank_[ry]) {
            parent_[ry] = rx;
        } else {
            parent_[ry] = rx;
            ++rank_[rx];
        }
    }

    /// Return all nodes tracked by this Union-Find.
    [[nodiscard]] std::vector<int64_t> nodes() const {
        std::vector<int64_t> result;
        result.reserve(parent_.size());
        for (const auto& [node, _] : parent_) {
            result.push_back(node);
        }
        return result;
    }

private:
    std::unordered_map<int64_t, int64_t> parent_;
    std::unordered_map<int64_t, int64_t> rank_;
};

} // namespace

AlgorithmDef make_connected_components_def() {
    AlgorithmDef def;
    def.name = "connected_components";
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"component_id", TypeId::INT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> connected_components_execute(const AlgorithmContext& ctx) {
    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build Union-Find from edges.
    UnionFind uf;
    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk, "connected_components");
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk, "connected_components");
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }
        // Ensure both nodes are registered even if they share an edge.
        (void)uf.find(*src);
        (void)uf.find(*tgt);
        uf.unite(*src, *tgt);
    }

    // Collect all nodes and their component IDs.
    auto all_nodes = uf.nodes();
    std::sort(all_nodes.begin(), all_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(all_nodes.size());
    for (int64_t node : all_nodes) {
        int64_t component = uf.find(node);
        rows.push_back({std::vector<Value>{Value(node), Value(component)}});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
