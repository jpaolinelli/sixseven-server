#include "giodb/executor/enriched_traversal.h"

#include "giodb/common/value_hash.h"
#include "giodb/executor/expr_evaluator.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace giodb {

EnrichedTraversalOperator::EnrichedTraversalOperator(GraphEngine& graph_engine,
                                                     TraversalConfig config,
                                                     OutputSchema schema,
                                                     const Expr* where_expr,
                                                     const BoundStatement& bound,
                                                     TableHeap& target_heap,
                                                     const Schema& target_storage_schema,
                                                     size_t target_pk_col_idx,
                                                     size_t target_column_count,
                                                     bool heterogeneous)
    : graph_engine_(graph_engine), config_(std::move(config)), schema_(std::move(schema)),
      where_expr_(where_expr), bound_(bound), target_heap_(target_heap),
      target_storage_schema_(target_storage_schema), target_pk_col_idx_(target_pk_col_idx),
      target_column_count_(target_column_count), heterogeneous_(heterogeneous) {}

std::string EnrichedTraversalOperator::plan_node_name() const {
    return "Enriched Graph Traverse";
}

std::string EnrichedTraversalOperator::plan_node_detail() const {
    return config_.edge_type;
}

const OutputSchema& EnrichedTraversalOperator::output_schema() const {
    return schema_;
}

Result<void> EnrichedTraversalOperator::do_open() {
    bfs_results_.clear();
    enriched_results_.clear();
    cursor_ = 0;

    auto bfs = run_bfs();
    if (!bfs) {
        return bfs;
    }

    return enrich_results();
}

Result<std::optional<Tuple>> EnrichedTraversalOperator::do_next() {
    while (cursor_ < enriched_results_.size()) {
        auto& tuple = enriched_results_[cursor_++];

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

void EnrichedTraversalOperator::do_close() {
    bfs_results_.clear();
    enriched_results_.clear();
    cursor_ = 0;
}

// ---------------------------------------------------------------------------
// Phase 1: BFS traversal (reuses algorithm from traversal.cpp)
// ---------------------------------------------------------------------------

Result<void> EnrichedTraversalOperator::run_bfs() {
    using VisitedSet = std::unordered_set<Value, ValueHash, ValueEqual>;

    VisitedSet visited;
    // BFS queue: (node_pk, depth, source_pk).
    std::deque<TraversalResult> queue;

    // Seed the BFS with the start node (no incoming edge, so empty edge_properties).
    // For heterogeneous edges the start node lives in a different table than the
    // target nodes, so adding its PK to the visited set would incorrectly suppress
    // target nodes whose PK happens to match (e.g. users(1) → posts(1)).
    if (!heterogeneous_) {
        visited.insert(config_.start_key);
    }
    queue.push_back({config_.start_key, 0, Value(), {}});

    while (!queue.empty()) {
        auto current = std::move(queue.front());
        queue.pop_front();

        // Emit this node as a result (skip depth 0 = the start node itself).
        if (current.depth > 0) {
            bfs_results_.push_back(current);
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
            queue.push_back({neighbor_pk, current.depth + 1, current.node_pk, edge.properties});
        }
    }

    return ok();
}

Result<std::vector<std::pair<Value, EdgeRow>>>
EnrichedTraversalOperator::get_neighbors(const Value& node_pk) const {
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

// ---------------------------------------------------------------------------
// Phase 2: Enrichment — batch scan target table, build PK hash map
// ---------------------------------------------------------------------------

Result<void> EnrichedTraversalOperator::enrich_results() {
    using RowMap = std::unordered_map<Value, std::vector<Value>, ValueHash, ValueEqual>;

    // Collect all unique PKs from BFS results.
    std::unordered_set<Value, ValueHash, ValueEqual> needed_pks;
    for (const auto& r : bfs_results_) {
        needed_pks.insert(r.node_pk);
    }

    // Single scan of target table: build PK -> row_data hash map.
    RowMap pk_to_row;
    if (!needed_pks.empty()) {
        auto iter = target_heap_.begin();
        if (!iter) {
            return tl::unexpected(iter.error());
        }

        while (true) {
            auto row = iter->next();
            if (!row) {
                break;
            }

            auto [rid, data] = *row;
            auto deserialized = TupleSerializer::deserialize(data, target_storage_schema_);
            if (!deserialized) {
                continue;
            }

            if (target_pk_col_idx_ >= deserialized->size()) {
                continue;
            }

            const auto& pk_val = (*deserialized)[target_pk_col_idx_];
            if (needed_pks.count(pk_val) > 0) {
                pk_to_row.emplace(pk_val, std::move(*deserialized));
                // Early exit if all needed PKs have been found.
                if (pk_to_row.size() == needed_pks.size()) {
                    break;
                }
            }
        }
    }

    // Assemble enriched tuples: [table_columns..., __node, __depth, __source].
    enriched_results_.reserve(bfs_results_.size());
    for (const auto& bfs : bfs_results_) {
        Tuple tuple;
        tuple.values.reserve(schema_.column_count());

        auto it = pk_to_row.find(bfs.node_pk);
        if (it != pk_to_row.end()) {
            // Found row data — copy target table columns.
            const auto& row_data = it->second;
            for (size_t i = 0; i < target_column_count_; ++i) {
                if (i < row_data.size()) {
                    tuple.values.push_back(row_data[i]);
                } else {
                    tuple.values.push_back(Value()); // NULL for missing columns
                }
            }
        } else {
            // Dangling edge — fill table columns with NULLs.
            for (size_t i = 0; i < target_column_count_; ++i) {
                tuple.values.push_back(Value());
            }
        }

        // Meta-columns: __node, __depth, __source.
        tuple.values.push_back(bfs.node_pk);
        tuple.values.push_back(Value(static_cast<int64_t>(bfs.depth)));
        tuple.values.push_back(bfs.source_pk);

        // Edge property values (after meta-columns).
        // The number of expected properties is schema_.column_count() minus
        // target_column_count_ minus 3 (meta-columns).
        size_t expected_props = schema_.column_count() - target_column_count_ - 3;
        for (size_t i = 0; i < expected_props; ++i) {
            if (i < bfs.edge_properties.size()) {
                tuple.values.push_back(bfs.edge_properties[i]);
            } else {
                tuple.values.push_back(Value()); // NULL for missing properties
            }
        }

        enriched_results_.push_back(std::move(tuple));
    }

    return ok();
}

} // namespace giodb
