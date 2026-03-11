#include "sixseven/graph/degree_centrality.h"

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
                                  "degree centrality requires integer node keys");
            }
        },
        v.data());
}

/// Extract a string from a Value for the direction parameter.
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
                                  "expected string value for direction parameter");
            }
        },
        v.data());
}

} // namespace

AlgorithmDef make_degree_centrality_def() {
    AlgorithmDef def;
    def.name = "degree_centrality";
    def.params = {
        {"direction", TypeId::STRING, false, Value(std::string("all"))},
    };
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"in_degree", TypeId::INT64, false},
        {"out_degree", TypeId::INT64, false},
        {"degree", TypeId::INT64, false},
        {"normalized_degree", TypeId::FLOAT64, false},
    };
    return def;
}

Result<std::vector<AlgorithmRow>> degree_centrality_execute(const AlgorithmContext& ctx) {
    // Extract direction parameter.
    std::string direction = "all";

    if (auto it = ctx.named_args.find("direction"); it != ctx.named_args.end()) {
        auto d = value_to_string(it->second);
        if (!d.has_value()) {
            return tl::unexpected(d.error());
        }
        direction = *d;
    }

    if (direction != "in" && direction != "out" && direction != "all") {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "direction must be 'in', 'out', or 'all'; got '" + direction + "'");
    }

    // Get all edges for the specified edge type.
    auto edges = ctx.graph_engine.get_all_edges(ctx.edge_type);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Single pass over edges to compute in-degree and out-degree.
    std::unordered_map<int64_t, int64_t> in_deg;
    std::unordered_map<int64_t, int64_t> out_deg;
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

        out_deg[*src]++;
        in_deg[*tgt]++;
        all_nodes.insert(*src);
        all_nodes.insert(*tgt);
    }

    if (all_nodes.empty()) {
        return ok(std::vector<AlgorithmRow>{});
    }

    auto n = static_cast<double>(all_nodes.size());
    double norm_divisor = (n > 1) ? (n - 1.0) : 1.0;

    // Build sorted output rows.
    std::vector<int64_t> sorted_nodes(all_nodes.begin(), all_nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::vector<AlgorithmRow> rows;
    rows.reserve(sorted_nodes.size());

    for (int64_t node : sorted_nodes) {
        auto node_in = in_deg.count(node) ? in_deg[node] : int64_t{0};
        auto node_out = out_deg.count(node) ? out_deg[node] : int64_t{0};

        int64_t degree = 0;
        if (direction == "in") {
            degree = node_in;
        } else if (direction == "out") {
            degree = node_out;
        } else {
            degree = node_in + node_out;
        }

        double normalized = static_cast<double>(degree) / norm_divisor;

        rows.push_back({std::vector<Value>{
            Value(node),
            Value(node_in),
            Value(node_out),
            Value(degree),
            Value(normalized),
        }});
    }

    return ok(std::move(rows));
}

} // namespace sixseven
