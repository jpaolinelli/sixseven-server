#include "sixseven/executor/traversal.h"

#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/graph_traversal_core.h"

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
    parent_map_.clear();
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
    parent_map_.clear();
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

    // Seed the BFS with the start node. For heterogeneous edges the start node
    // lives in a different table than the target nodes, so adding its PK to the
    // visited set would incorrectly suppress target nodes whose PK happens to
    // match (GDB-696). TRACE remains safe without the seed: reconstruct_path()
    // is depth-guarded against parent-map cycles (GDB-694).
    if (!config_.heterogeneous) {
        visited.insert(config_.start_key);
    }
    queue.push_back({config_.start_key, 0, Value(), -1, {}});

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

            // TRACE mode: append the full start-to-node path as a trailing column.
            if (config_.trace) {
                auto path = reconstruct_path(current.node_pk, current.depth);
                if (!path) {
                    return tl::unexpected(path.error());
                }
                tuple.values.push_back(Value(std::move(*path)));
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
            // Check memory bound. TraversalOperator materializes results_
            // incrementally as nodes are dequeued (unlike the two-phase
            // Edge/Enriched/MatchShortestPath operators), so by the time
            // this limit trips, results_ may already hold rows for nodes
            // dequeued earlier in this same run_bfs() call. Clear results_
            // before returning the error so do_next() can never yield a
            // leaked partial row after a failed open() (GDB-1288).
            if (visited.size() >= config_.max_visited) {
                results_.clear();
                edges_.clear();
                parent_map_.clear();
                return tl::unexpected(make_error(StatusCode::INVALID_ARGUMENT,
                                                 "graph traversal exceeded max_visited limit (" +
                                                     std::to_string(config_.max_visited) + ")"));
            }

            // Cycle detection: skip already-visited nodes.
            if (visited.count(neighbor_pk) > 0) {
                continue;
            }

            visited.insert(neighbor_pk);

            // TRACE mode: record how this node was first reached so the path can
            // be reconstructed later. Done before the edge is moved-from below.
            if (config_.trace) {
                parent_map_.emplace(
                    neighbor_pk,
                    ParentInfo{current.node_pk, static_cast<int64_t>(edge.edge_row_id)});
            }

            if (config_.collect_edges) {
                edges_.push_back(std::move(edge));
            }

            queue.push_back({neighbor_pk, current.depth + 1, current.node_pk, -1, {}});
        }
    }

    return ok();
}

Result<std::vector<std::pair<Value, EdgeRow>>>
TraversalOperator::get_neighbors(const Value& node_pk) const {
    return expand_neighbors(
        graph_engine_, config_.database_id, config_.edge_type, node_pk, config_.direction);
}

Result<Path> TraversalOperator::reconstruct_path(const Value& target, int32_t target_depth) const {
    return sixseven::reconstruct_path(target, target_depth, parent_map_);
}

} // namespace sixseven
