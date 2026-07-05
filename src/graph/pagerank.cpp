#include "sixseven/graph/pagerank.h"

#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/value_extract.h"

#include <algorithm>
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

} // namespace

AlgorithmDef make_pagerank_def() {
    AlgorithmDef def;
    def.name = "pagerank";
    def.params = {
        {"damping", TypeId::FLOAT64, false, Value(0.85)},
        {"iterations", TypeId::INT64, false, Value(static_cast<int64_t>(20))},
    };
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"score", TypeId::FLOAT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> pagerank_execute(const AlgorithmContext& ctx) {
    // Extract parameters.
    double damping = 0.85;
    int64_t iterations = 20;

    if (auto it = ctx.named_args.find("damping"); it != ctx.named_args.end()) {
        auto d = value_to_double(it->second, "damping parameter");
        if (!d.has_value()) {
            return tl::unexpected(d.error());
        }
        damping = *d;
    }

    if (auto it = ctx.named_args.find("iterations"); it != ctx.named_args.end()) {
        auto i = value_to_iterations(it->second);
        if (!i.has_value()) {
            return tl::unexpected(i.error());
        }
        iterations = *i;
    }

    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.database_id, ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Build adjacency list and collect all nodes.
    std::unordered_map<int64_t, std::vector<int64_t>> outgoing;
    std::unordered_set<int64_t> all_nodes;

    for (const auto& edge : *edges) {
        auto src = value_to_int64(edge.source_pk, "pagerank");
        if (!src.has_value()) {
            return tl::unexpected(src.error());
        }
        auto tgt = value_to_int64(edge.target_pk, "pagerank");
        if (!tgt.has_value()) {
            return tl::unexpected(tgt.error());
        }
        outgoing[*src].push_back(*tgt);
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    auto num_nodes = static_cast<double>(all_nodes.size());
    double initial_score = 1.0 / num_nodes;

    // Initialize scores.
    std::unordered_map<int64_t, double> scores;
    for (int64_t node : all_nodes) {
        scores[node] = initial_score;
    }

    // Iterative power method.
    for (int64_t iter = 0; iter < iterations; ++iter) {
        std::unordered_map<int64_t, double> new_scores;
        for (int64_t node : all_nodes) {
            new_scores[node] = (1.0 - damping) / num_nodes;
        }

        for (int64_t node : all_nodes) {
            auto out_it = outgoing.find(node);
            if (out_it != outgoing.end() && !out_it->second.empty()) {
                double contribution =
                    damping * scores[node] / static_cast<double>(out_it->second.size());
                for (int64_t target : out_it->second) {
                    new_scores[target] += contribution;
                }
            } else {
                // Dangling node: distribute its rank evenly to all nodes.
                double contribution = damping * scores[node] / num_nodes;
                for (int64_t n : all_nodes) {
                    new_scores[n] += contribution;
                }
            }
        }

        scores = std::move(new_scores);
    }

    // Build sorted output rows.
    std::vector<int64_t> sorted_nodes(all_nodes.begin(), all_nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());
    for (int64_t node : sorted_nodes) {
        rows.push_back({std::vector<Value>{Value(node), Value(scores[node])}});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
