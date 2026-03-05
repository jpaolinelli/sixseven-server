#include "sixseven/executor/traversal.h"

#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"

#include <deque>
#include <unordered_set>

namespace sixseven {

TraversalOperator::TraversalOperator(GraphEngine& graph_engine,
                                     TraversalConfig config,
                                     OutputSchema schema,
                                     const Expr* where_expr,
                                     const BoundStatement& bound)
    : graph_engine_(graph_engine), config_(std::move(config)), schema_(std::move(schema)),
      where_expr_(where_expr), bound_(bound) {}

std::string TraversalOperator::plan_node_name() const {
    return "Graph Traverse";
}
std::string TraversalOperator::plan_node_detail() const {
    return "";
}

Result<void> TraversalOperator::do_open() {
    results_.clear();
    edges_.clear();
    cursor_ = 0;
    return run_bfs();
}

Result<std::optional<Tuple>> TraversalOperator::do_next() {
    while (cursor_ < results_.size()) {
        auto& tuple = results_[cursor_++];

        // Apply WHERE post-filter if present.
        if (where_expr_) {
            auto pred = evaluate_predicate(*where_expr_, tuple, schema_, bound_);
            if (!pred) {
                return tl::unexpected(pred.error());
            }
            if (!*pred) {
                continue;
            }
        }

        return ok(std::make_optional(std::move(tuple)));
    }
    return ok(std::optional<Tuple>(std::nullopt));
}

void TraversalOperator::do_close() {
    results_.clear();
    edges_.clear();
    cursor_ = 0;
}

const OutputSchema& TraversalOperator::output_schema() const {
    return schema_;
}

Result<void> TraversalOperator::run_bfs() {
    using VisitedSet = std::unordered_set<Value, ValueHash, ValueEqual>;

    VisitedSet visited;
    // BFS queue: (node_pk, depth, source_pk).
    std::deque<TraversalResult> queue;

    // Seed the BFS with the start node.
    visited.insert(config_.start_key);
    queue.push_back({config_.start_key, 0, Value(), {}});

    while (!queue.empty()) {
        auto current = std::move(queue.front());
        queue.pop_front();

        // Emit this node as a result (skip depth 0 = the start node itself).
        if (current.depth > 0) {
            Tuple tuple;
            if (config_.fetch) {
                // FETCH mode: node_pk, depth, source_pk (enrichment happens in planner/QE).
                tuple.values = {
                    current.node_pk, Value(static_cast<int64_t>(current.depth)), current.source_pk};
            } else {
                tuple.values = {current.node_pk, Value(static_cast<int64_t>(current.depth))};
            }
            results_.push_back(std::move(tuple));
        }

        // Don't expand beyond max_depth.
        if (current.depth >= config_.max_depth) {
            continue;
        }

        // Get neighbors.
        auto neighbors = get_neighbors(current.node_pk);
        if (!neighbors) {
            return tl::unexpected(neighbors.error());
        }

        for (auto& [neighbor_pk, edge] : *neighbors) {
            // Check memory bound.
            if (visited.size() >= config_.max_visited) {
                break;
            }

            // Cycle detection: skip already-visited nodes.
            if (visited.count(neighbor_pk) > 0) {
                continue;
            }

            visited.insert(neighbor_pk);

            if (config_.collect_edges) {
                edges_.push_back(std::move(edge));
            }

            queue.push_back({neighbor_pk, current.depth + 1, current.node_pk, {}});
        }
    }

    return ok();
}

Result<std::vector<std::pair<Value, EdgeRow>>>
TraversalOperator::get_neighbors(const Value& node_pk) const {
    std::vector<std::pair<Value, EdgeRow>> result;

    if (config_.direction == TraverseDirection::OUT ||
        config_.direction == TraverseDirection::BOTH) {
        auto edges = graph_engine_.get_edges_from(config_.edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.emplace_back(edge.target_pk, std::move(edge));
        }
    }

    if (config_.direction == TraverseDirection::IN ||
        config_.direction == TraverseDirection::BOTH) {
        auto edges = graph_engine_.get_edges_to(config_.edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.emplace_back(edge.source_pk, std::move(edge));
        }
    }

    return ok(std::move(result));
}

} // namespace sixseven
