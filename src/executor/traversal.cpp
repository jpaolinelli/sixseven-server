#include "sixseven/executor/traversal.h"

#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace sixseven {

namespace {

/// Convert a Value PK to int64_t for PathStep storage.
/// Returns an error if the PK is not an integer type.
Result<int64_t> pk_to_int64(const Value& pk) {
    if (!pk.is_null()) {
        if (auto* p = std::get_if<int64_t>(&pk.data())) {
            return ok(*p);
        }
        if (auto* p32 = std::get_if<int32_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*p32));
        }
    }
    return make_error(StatusCode::INVALID_ARGUMENT,
                      "TRAVERSE WITH TRACE requires integer primary keys");
}

} // namespace

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

    // Seed the BFS with the start node.
    visited.insert(config_.start_key);
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
            // Check memory bound.
            if (visited.size() >= config_.max_visited) {
                break;
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
    std::vector<std::pair<Value, EdgeRow>> result;

    if (config_.direction == TraverseDirection::OUT ||
        config_.direction == TraverseDirection::BOTH) {
        auto edges = graph_engine_.get_edges_from(config_.database_id, config_.edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.emplace_back(edge.target_pk, std::move(edge));
        }
    }

    if (config_.direction == TraverseDirection::IN ||
        config_.direction == TraverseDirection::BOTH) {
        auto edges = graph_engine_.get_edges_to(config_.database_id, config_.edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.emplace_back(edge.source_pk, std::move(edge));
        }
    }

    return ok(std::move(result));
}

Result<Path> TraversalOperator::reconstruct_path(const Value& target, int32_t target_depth) const {
    // Walk parent pointers backward from target to the start node, collecting
    // (node_pk, incoming_edge_row_id) pairs, then reverse to get start->target.
    std::vector<PathStep> reversed;

    // A valid parent chain consumes each parent-map entry at most once plus a
    // terminal step for the start node; anything longer means the parent map
    // contains a cycle (GDB-694).
    const size_t max_steps = parent_map_.size() + 1;

    Value cursor = target;
    int32_t remaining = target_depth;
    while (true) {
        if (reversed.size() >= max_steps) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "TRACE path reconstruction detected a cycle in the parent map");
        }

        auto cursor_int = pk_to_int64(cursor);
        if (!cursor_int) {
            return tl::unexpected(cursor_int.error());
        }

        // After target_depth hops the cursor is the start node. The depth guard
        // keeps reconstruction bounded even if the parent map contains a cycle
        // (e.g. cross-table PK collisions, GDB-694).
        if (remaining <= 0) {
            reversed.push_back({*cursor_int, -1});
            break;
        }

        auto it = parent_map_.find(cursor);
        if (it == parent_map_.end()) {
            // Reached the start node (no parent entry). The start node has no
            // incoming edge: edge_id = -1.
            reversed.push_back({*cursor_int, -1});
            break;
        }

        // The edge_row_id stored here is the edge from parent -> cursor, i.e. the
        // edge that, in start->target order, leaves the parent. We attach it to
        // the parent's step below, so carry it as this step's edge for now and
        // shift after reversal.
        reversed.push_back({*cursor_int, it->second.edge_row_id});
        cursor = it->second.parent_pk;
        --remaining;
    }

    // reversed is target..start. Reverse to start..target.
    std::reverse(reversed.begin(), reversed.end());

    // After reversal each step still carries its own *incoming* edge id. In Path
    // semantics a step's edge_id is the *outgoing* edge to the next step, so shift
    // edge ids one position toward the start and clear the terminal step's edge.
    Path path;
    path.steps.reserve(reversed.size());
    for (size_t i = 0; i < reversed.size(); ++i) {
        int64_t outgoing = (i + 1 < reversed.size()) ? reversed[i + 1].edge_id : -1;
        path.steps.push_back({reversed[i].node_pk, outgoing});
    }

    return ok(std::move(path));
}

} // namespace sixseven
