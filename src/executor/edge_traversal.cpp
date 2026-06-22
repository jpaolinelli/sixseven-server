#include "sixseven/executor/edge_traversal.h"

#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/graph_traversal_core.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

EdgeTraversalOperator::EdgeTraversalOperator(GraphEngine& graph_engine,
                                             TraversalConfig config,
                                             OutputSchema schema,
                                             const Expr* where_expr,
                                             const BoundStatement& bound,
                                             bool heterogeneous)
    : graph_engine_(graph_engine), config_(std::move(config)), schema_(std::move(schema)),
      where_expr_(where_expr), bound_(bound), heterogeneous_(heterogeneous) {}

std::string EdgeTraversalOperator::plan_node_name() const {
    return "Edge Graph Traverse";
}

std::string EdgeTraversalOperator::plan_node_detail() const {
    return config_.edge_type;
}

const OutputSchema& EdgeTraversalOperator::output_schema() const {
    return schema_;
}

Result<void> EdgeTraversalOperator::do_open() {
    bfs_results_.clear();
    edge_results_.clear();
    parent_map_.clear();
    cursor_ = 0;

    auto bfs = run_bfs();
    if (!bfs) {
        return bfs;
    }

    return collect_edges();
}

Result<std::optional<Tuple>> EdgeTraversalOperator::do_next() {
    while (cursor_ < edge_results_.size()) {
        auto& tuple = edge_results_[cursor_++];

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

void EdgeTraversalOperator::do_close() {
    bfs_results_.clear();
    edge_results_.clear();
    parent_map_.clear();
    cursor_ = 0;
}

// ---------------------------------------------------------------------------
// Phase 1: BFS traversal (same algorithm as EnrichedTraversalOperator)
// ---------------------------------------------------------------------------

Result<void> EdgeTraversalOperator::run_bfs() {
    using VisitedSet = std::unordered_set<Value, ValueHash, ValueEqual>;

    VisitedSet visited;
    std::deque<TraversalResult> queue;

    // For heterogeneous edges the start node lives in a different table than
    // the target nodes, so adding its PK to the visited set would incorrectly
    // suppress target nodes whose PK happens to match.
    if (!heterogeneous_) {
        visited.insert(config_.start_key);
    }
    queue.push_back({config_.start_key, 0, Value(), -1, {}});

    while (!queue.empty()) {
        auto current = std::move(queue.front());
        queue.pop_front();

        // Record all nodes including start (depth 0) for edge scanning.
        if (current.depth > 0) {
            bfs_results_.push_back(current);
        }

        // Don't expand beyond max_depth.
        if (current.depth >= config_.max_depth) {
            continue;
        }

        auto neighbors = get_neighbors(current.node_pk);
        if (!neighbors) {
            return tl::unexpected(neighbors.error());
        }

        for (auto& [neighbor_pk, edge] : *neighbors) {
            if (visited.size() >= config_.max_visited) {
                break;
            }

            if (visited.count(neighbor_pk) > 0) {
                continue;
            }

            visited.insert(neighbor_pk);

            // TRACE mode: record how this node was first reached so the path to
            // each edge's __from node can be reconstructed during edge collection.
            if (config_.trace) {
                parent_map_.emplace(
                    neighbor_pk,
                    ParentInfo{current.node_pk, static_cast<int64_t>(edge.edge_row_id)});
            }

            queue.push_back({neighbor_pk, current.depth + 1, current.node_pk, -1, edge.properties});
        }
    }

    return ok();
}

Result<std::vector<std::pair<Value, EdgeRow>>>
EdgeTraversalOperator::get_neighbors(const Value& node_pk) const {
    return expand_neighbors(
        graph_engine_, config_.database_id, config_.edge_type, node_pk, config_.direction);
}

// ---------------------------------------------------------------------------
// Phase 2: Edge scan — collect all edges between discovered nodes
// ---------------------------------------------------------------------------

Result<void> EdgeTraversalOperator::collect_edges() {
    using DepthMap = std::unordered_map<Value, int32_t, ValueHash, ValueEqual>;

    // Meta-column count: __from, __to, __depth, plus __path when trace is enabled.
    const size_t meta_columns = config_.trace ? 4 : 3;

    // Number of edge property columns = schema columns - meta-columns.
    const size_t expected_props =
        schema_.column_count() > meta_columns ? schema_.column_count() - meta_columns : 0;

    // Helper to emit a single edge as a result tuple.
    auto emit_edge =
        [&](const EdgeRow& edge, int32_t from_depth, int32_t to_depth) -> Result<void> {
        Tuple tuple;
        tuple.values.reserve(schema_.column_count());

        tuple.values.push_back(edge.source_pk); // __from
        tuple.values.push_back(edge.target_pk); // __to
        tuple.values.push_back(
            Value(static_cast<int64_t>(std::max(from_depth, to_depth)))); // __depth

        // TRACE mode: append the start-to-__from path after __depth, before
        // edge properties. The __from node is edge.source_pk at from_depth.
        if (config_.trace) {
            auto path = reconstruct_path(edge.source_pk, from_depth);
            if (!path) {
                return tl::unexpected(path.error());
            }
            tuple.values.push_back(Value(std::move(*path)));
        }

        // Edge property values.
        for (size_t i = 0; i < expected_props; ++i) {
            if (i < edge.properties.size()) {
                tuple.values.push_back(edge.properties[i]);
            } else {
                tuple.values.push_back(Value()); // NULL for missing properties
            }
        }

        edge_results_.push_back(std::move(tuple));
        return ok();
    };

    if (!heterogeneous_) {
        // Homogeneous: single depth map, no PK collision risk.
        DepthMap depths;
        depths[config_.start_key] = 0;
        for (const auto& r : bfs_results_) {
            depths[r.node_pk] = r.depth;
        }

        // Scan outgoing edges from every discovered node.
        for (const auto& [node_pk, node_depth] : depths) {
            auto edges =
                graph_engine_.get_edges_from(config_.database_id, config_.edge_type, node_pk);
            if (!edges) {
                return tl::unexpected(edges.error());
            }
            for (const auto& edge : *edges) {
                auto it = depths.find(edge.target_pk);
                if (it != depths.end()) {
                    auto emitted = emit_edge(edge, node_depth, it->second);
                    if (!emitted) {
                        return tl::unexpected(emitted.error());
                    }
                }
            }
        }
    } else {
        // Heterogeneous: separate maps for source and target table nodes
        // to avoid PK collision between different tables.
        DepthMap source_depths;
        DepthMap target_depths;

        if (config_.direction == TraverseDirection::OUT) {
            // Start node is in source table, BFS results are in target table.
            source_depths[config_.start_key] = 0;
            for (const auto& r : bfs_results_) {
                target_depths[r.node_pk] = r.depth;
            }

            // Only scan from source table nodes (start node).
            auto edges = graph_engine_.get_edges_from(
                config_.database_id, config_.edge_type, config_.start_key);
            if (!edges) {
                return tl::unexpected(edges.error());
            }
            for (const auto& edge : *edges) {
                auto it = target_depths.find(edge.target_pk);
                if (it != target_depths.end()) {
                    auto emitted = emit_edge(edge, 0, it->second);
                    if (!emitted) {
                        return tl::unexpected(emitted.error());
                    }
                }
            }
        } else {
            // IN direction: Start node is in target table, BFS results are source table.
            target_depths[config_.start_key] = 0;
            for (const auto& r : bfs_results_) {
                source_depths[r.node_pk] = r.depth;
            }

            // Scan from each discovered source table node.
            for (const auto& [src_pk, src_depth] : source_depths) {
                auto edges =
                    graph_engine_.get_edges_from(config_.database_id, config_.edge_type, src_pk);
                if (!edges) {
                    return tl::unexpected(edges.error());
                }
                for (const auto& edge : *edges) {
                    auto it = target_depths.find(edge.target_pk);
                    if (it != target_depths.end()) {
                        auto emitted = emit_edge(edge, src_depth, it->second);
                        if (!emitted) {
                            return tl::unexpected(emitted.error());
                        }
                    }
                }
            }
        }
    }

    return ok();
}

Result<Path> EdgeTraversalOperator::reconstruct_path(const Value& target,
                                                     int32_t target_depth) const {
    return sixseven::reconstruct_path(target, target_depth, parent_map_);
}

} // namespace sixseven
