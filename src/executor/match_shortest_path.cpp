#include "sixseven/executor/match_shortest_path.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/graph_traversal_core.h"
#include "sixseven/table/tuple.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

MatchShortestPathOperator::MatchShortestPathOperator(
    GraphEngine& graph_engine,
    const Catalog& catalog,
    StorageManager& storage,
    database_id_t database_id,
    MatchConfig config,
    OutputSchema schema,
    const Expr* where_expr,
    const BoundStatement& bound,
    PathSelector path_selector,
    std::string path_variable,
    int32_t shortest_k,
    size_t max_visited,
    const Expr* weight_expr,
    const std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes,
    const std::unordered_map<index_id_t, HashIndex*>* hash_indexes)
    : graph_engine_(graph_engine), catalog_(catalog), storage_(storage), database_id_(database_id),
      config_(std::move(config)), schema_(std::move(schema)), where_expr_(where_expr),
      bound_(bound), path_selector_(path_selector), path_variable_(std::move(path_variable)),
      shortest_k_(shortest_k), weight_expr_(weight_expr), max_visited_(max_visited),
      btree_indexes_(btree_indexes), hash_indexes_(hash_indexes) {}

std::string MatchShortestPathOperator::plan_node_name() const {
    std::string prefix = weight_expr_ ? "Weighted " : "";
    switch (path_selector_) {
    case PathSelector::ANY_SHORTEST:
        return prefix + "Any Shortest Path Match";
    case PathSelector::ALL_SHORTEST:
        return prefix + "All Shortest Path Match";
    case PathSelector::SHORTEST_K:
        return prefix + "Shortest " + std::to_string(shortest_k_) + " Path Match";
    default:
        return prefix + "Shortest Path Match";
    }
}

std::string MatchShortestPathOperator::plan_node_detail() const {
    if (!config_.edges.empty()) {
        return config_.edges[0].edge_type;
    }
    return "";
}

Result<void> MatchShortestPathOperator::do_open() {
    results_.clear();
    cursor_ = 0;
    return execute_shortest_paths();
}

Result<std::optional<Tuple>> MatchShortestPathOperator::do_next() {
    while (cursor_ < results_.size()) {
        auto& tuple = results_[cursor_++];

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

void MatchShortestPathOperator::do_close() {
    results_.clear();
    cursor_ = 0;
}

const OutputSchema& MatchShortestPathOperator::output_schema() const {
    return schema_;
}

Result<std::vector<Value>>
MatchShortestPathOperator::get_all_pks(const std::string& table_name) const {
    auto schema = catalog_.get_table(database_id_, table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }

    auto ts = storage_.get_table_storage(schema->table_id);
    if (!ts) {
        return tl::unexpected(ts.error());
    }

    int pk_col_idx = -1;
    for (size_t i = 0; i < schema->columns.size(); ++i) {
        if (schema->columns[i].name == schema->pk_columns) {
            pk_col_idx = static_cast<int>(i);
            break;
        }
    }
    if (pk_col_idx < 0) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "table " + table_name + " has no primary key");
    }

    std::vector<Value> pks;
    auto iter = (*ts)->heap->begin();
    if (!iter) {
        return tl::unexpected(iter.error());
    }

    while (true) {
        auto row_result = iter->next();
        if (!row_result) {
            return tl::unexpected(row_result.error());
        }
        if (!row_result->has_value()) {
            break;
        }

        auto [rid, data] = **row_result;
        auto deserialized = TupleSerializer::deserialize(data, (*ts)->storage_schema);
        if (!deserialized) {
            continue;
        }
        if (static_cast<size_t>(pk_col_idx) >= deserialized->size()) {
            continue;
        }
        pks.push_back((*deserialized)[static_cast<size_t>(pk_col_idx)]);
    }

    return ok(std::move(pks));
}

Result<std::vector<std::pair<Value, int64_t>>> MatchShortestPathOperator::get_neighbors(
    const std::string& edge_type, const Value& pk, TraverseDirection direction) const {
    return expand_neighbors_ids(graph_engine_, database_id_, edge_type, pk, direction);
}

Result<std::vector<Value>> MatchShortestPathOperator::fetch_node_data(const std::string& table_name,
                                                                      const Value& pk) const {
    auto schema = catalog_.get_table(database_id_, table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }

    auto ts = storage_.get_table_storage(schema->table_id);
    if (!ts) {
        return tl::unexpected(ts.error());
    }

    int pk_col_idx = -1;
    for (size_t i = 0; i < schema->columns.size(); ++i) {
        if (schema->columns[i].name == schema->pk_columns) {
            pk_col_idx = static_cast<int>(i);
            break;
        }
    }
    if (pk_col_idx < 0) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "table " + table_name + " has no primary key");
    }

    // Index point-lookup path: use the PK index when available.
    if (btree_indexes_ != nullptr || hash_indexes_ != nullptr) {
        auto indexes = catalog_.list_indexes(schema->table_id);
        for (const auto& idx_def : indexes) {
            if (idx_def.columns != schema->pk_columns) {
                continue;
            }
            if (idx_def.index_type == "btree" && btree_indexes_ != nullptr) {
                auto it = btree_indexes_->find(idx_def.index_id);
                if (it != btree_indexes_->end()) {
                    KeyType key = {pk};
                    auto rid_result = it->second->search(key);
                    if (!rid_result) {
                        return tl::unexpected(rid_result.error());
                    }
                    if (!rid_result->has_value()) {
                        // Key absent from this index (e.g. partial/stale index);
                        // fall through to the heap scan instead of reporting the
                        // node missing -- the row may still be in the heap, which
                        // is what the pre-index scan path would have returned.
                        break;
                    }
                    auto data = (*ts)->heap->get_tuple(**rid_result);
                    if (!data) {
                        break;
                    }
                    auto deserialized = TupleSerializer::deserialize(*data, (*ts)->storage_schema);
                    if (!deserialized) {
                        break;
                    }
                    return ok(std::move(*deserialized));
                }
            }
            if (idx_def.index_type == "hash" && hash_indexes_ != nullptr) {
                auto it = hash_indexes_->find(idx_def.index_id);
                if (it != hash_indexes_->end()) {
                    KeyType key = {pk};
                    auto rid_result = it->second->search(key);
                    if (!rid_result) {
                        return tl::unexpected(rid_result.error());
                    }
                    if (!rid_result->has_value()) {
                        // Key absent from this index (e.g. partial/stale index);
                        // fall through to the heap scan instead of reporting the
                        // node missing -- the row may still be in the heap, which
                        // is what the pre-index scan path would have returned.
                        break;
                    }
                    auto data = (*ts)->heap->get_tuple(**rid_result);
                    if (!data) {
                        break;
                    }
                    auto deserialized = TupleSerializer::deserialize(*data, (*ts)->storage_schema);
                    if (!deserialized) {
                        break;
                    }
                    return ok(std::move(*deserialized));
                }
            }
        }
    }

    // Fallback: full heap scan when no usable PK index is available.
    auto iter = (*ts)->heap->begin();
    if (!iter) {
        return tl::unexpected(iter.error());
    }

    while (true) {
        auto row_result = iter->next();
        if (!row_result) {
            return tl::unexpected(row_result.error());
        }
        if (!row_result->has_value()) {
            break;
        }

        auto [rid, data] = **row_result;
        auto deserialized = TupleSerializer::deserialize(data, (*ts)->storage_schema);
        if (!deserialized) {
            continue;
        }
        if (static_cast<size_t>(pk_col_idx) >= deserialized->size()) {
            continue;
        }

        auto cmp = compare((*deserialized)[static_cast<size_t>(pk_col_idx)], pk);
        if (cmp && *cmp == std::strong_ordering::equal) {
            return ok(std::move(*deserialized));
        }
    }

    return make_error(StatusCode::NOT_FOUND, "node with pk not found in table " + table_name);
}

Result<Tuple>
MatchShortestPathOperator::binding_to_tuple(const std::unordered_map<std::string, Value>& binding,
                                            const Path* path_value) const {
    Tuple tuple;

    for (size_t col_idx = 0; col_idx < schema_.column_count(); ++col_idx) {
        const auto& out_col = schema_.column(col_idx);

        // Check if this column is the path variable.
        if (path_value != nullptr && out_col.table_name == path_variable_ &&
            out_col.name == "path") {
            tuple.values.push_back(Value(*path_value));
            continue;
        }

        auto bit = binding.find(out_col.table_name);
        if (bit != binding.end()) {
            std::string label;
            for (const auto& node : config_.nodes) {
                if (node.variable == out_col.table_name) {
                    label = node.label;
                    break;
                }
            }

            if (!label.empty()) {
                auto node_data = fetch_node_data(label, bit->second);
                if (node_data) {
                    auto tbl_schema = catalog_.get_table(database_id_, label);
                    if (tbl_schema) {
                        auto col_it =
                            std::find_if(tbl_schema->columns.begin(),
                                         tbl_schema->columns.end(),
                                         [&](const auto& c) { return c.name == out_col.name; });
                        if (col_it != tbl_schema->columns.end()) {
                            size_t idx = static_cast<size_t>(
                                std::distance(tbl_schema->columns.begin(), col_it));
                            if (idx < node_data->size()) {
                                tuple.values.push_back((*node_data)[idx]);
                                continue;
                            }
                        }
                    }
                }
            }
        }

        tuple.values.push_back(Value());
    }

    return ok(std::move(tuple));
}

Result<std::vector<Path>>
MatchShortestPathOperator::find_shortest_paths(const Value& src_pk,
                                               const Value& tgt_pk,
                                               table_id_t src_table_id,
                                               table_id_t tgt_table_id,
                                               const std::string& edge_type,
                                               TraverseDirection direction,
                                               int32_t min_hops,
                                               int32_t max_depth) {

    std::vector<Path> result_paths;

    // Validate that PKs are integer types (PathStep stores int64_t).
    auto src_int = pk_to_int64(src_pk);
    if (!src_int) {
        return tl::unexpected(src_int.error());
    }

    // Check trivial case: source == target.
    // Requires BOTH the same table AND the same PK — GDB-851.
    // For homogeneous edges src_table_id == tgt_table_id so the check is
    // equivalent to the old bare-pk comparison; for heterogeneous edges it
    // correctly rejects the bogus 0-hop path.
    //
    // Only emit the 0-hop self-path when min_hops == 0.  If min_hops >= 1 the
    // quantifier requires at least one real edge traversal, so (x,x) may only
    // appear via a genuine cycle — returning an empty vector here lets BFS find
    // that cycle (or correctly produce no result, consistent with
    // VariableLengthMatchOperator — GDB-858).
    if (src_table_id == tgt_table_id && ValueEqual{}(src_pk, tgt_pk) && min_hops == 0) {
        Path p;
        p.steps.push_back({*src_int, -1});
        result_paths.push_back(std::move(p));
        return ok(std::move(result_paths));
    }

    // BFS with path tracking.
    // Each entry tracks: current node identity (table+pk), the full path so far.
    struct BfsEntry {
        NodeId current_node;
        Path path_so_far;
    };

    std::deque<BfsEntry> queue;
    // Key visited set by (table_id, pk) to prevent cross-table PK collisions
    // from pruning real paths or fabricating meeting points (GDB-851).
    std::unordered_set<NodeId, NodeIdHash> globally_visited;
    size_t total_visited = 0;

    // Seed BFS from source.
    NodeId src_node{src_table_id, src_pk};
    BfsEntry start;
    start.current_node = src_node;
    start.path_so_far.steps.push_back({*src_int, -1});
    queue.push_back(std::move(start));
    globally_visited.insert(src_node);

    int32_t depth = 0;
    bool found_at_this_depth = false;

    while (!queue.empty() && depth < max_depth) {
        ++depth;
        size_t level_size = queue.size();

        // For ALL_SHORTEST: nodes discovered at this level should not block
        // other paths at the same level from reaching the same node.
        std::vector<NodeId> level_new_nodes;

        for (size_t i = 0; i < level_size; ++i) {
            auto entry = std::move(queue.front());
            queue.pop_front();
            ++total_visited;

            if (total_visited > max_visited_) {
                return ok(std::move(result_paths));
            }

            auto neighbors = get_neighbors(edge_type, entry.current_node.pk, direction);
            if (!neighbors) {
                return tl::unexpected(neighbors.error());
            }

            // Neighbors of a src-table node arrive in the tgt table (OUT), and vice
            // versa.  For homogeneous edges src_table_id == tgt_table_id so this is
            // a no-op; for heterogeneous it picks the correct table per hop.
            table_id_t nbr_table_id =
                (entry.current_node.table_id == src_table_id) ? tgt_table_id : src_table_id;

            for (auto& [nbr_pk, edge_id] : *neighbors) {
                auto nbr_int = pk_to_int64(nbr_pk);
                if (!nbr_int) {
                    return tl::unexpected(nbr_int.error());
                }

                NodeId nbr_node{nbr_table_id, nbr_pk};

                // Build new path.
                Path new_path = entry.path_so_far;
                // Update the last step's edge_id (the edge FROM previous node TO this one).
                new_path.steps.back().edge_id = edge_id;
                // Add the new node.
                new_path.steps.push_back({*nbr_int, -1});

                // Check if we've reached the target (table AND pk must match).
                // Enforce quantifier bounds inclusively (matching VariableLengthMatch
                // and Dijkstra): only accept paths where
                //   min_hops <= length <= max_depth   (max_depth == max_hops)
                // The upper bound corrects a BFS off-by-one (GDB-1273): the loop body
                // can expand neighbours that land one hop past the quantifier ceiling,
                // so a path of length max_hops+1 must be rejected here — but a path of
                // length exactly max_hops is valid and must be kept.
                // The lower bound ensures real discovered paths satisfy min_hops
                // (GDB-1272): e.g. a direct self-loop under {2,N} has length 1 < 2.
                if (nbr_node == NodeId{tgt_table_id, tgt_pk}) {
                    if (new_path.length() >= static_cast<int64_t>(min_hops) &&
                        new_path.length() <= static_cast<int64_t>(max_depth)) {
                        found_at_this_depth = true;
                        result_paths.push_back(std::move(new_path));

                        if (path_selector_ == PathSelector::ANY_SHORTEST) {
                            return ok(std::move(result_paths));
                        }
                        if (path_selector_ == PathSelector::SHORTEST_K &&
                            static_cast<int32_t>(result_paths.size()) >= shortest_k_) {
                            return ok(std::move(result_paths));
                        }
                    }
                    continue;
                }

                // For ALL_SHORTEST/SHORTEST_K: allow multiple paths through same node at same
                // level.  Key by NodeId to prevent cross-table PK collisions (GDB-851).
                if (path_selector_ == PathSelector::ANY_SHORTEST) {
                    if (globally_visited.count(nbr_node) > 0) {
                        continue;
                    }
                    globally_visited.insert(nbr_node);
                } else {
                    // Don't revisit nodes from previous levels, but allow same-level visits.
                    if (globally_visited.count(nbr_node) > 0) {
                        continue;
                    }
                    // Copy (not move) here: nbr_node is still consumed below to build
                    // the next BfsEntry. Moving twice (GDB-1270) leaves a moved-from
                    // NodeId whose pk is empty for non-trivially-copyable PKs (e.g.
                    // string PKs), silently dropping path expansions.
                    level_new_nodes.push_back(nbr_node);
                }

                BfsEntry next;
                next.current_node = std::move(nbr_node);
                next.path_so_far = std::move(new_path);
                queue.push_back(std::move(next));
            }
        }

        // After processing a level, add new nodes to globally visited.
        for (const auto& node : level_new_nodes) {
            globally_visited.insert(node);
        }

        // If we found paths at this depth and want ALL_SHORTEST, stop expanding.
        if (found_at_this_depth && path_selector_ == PathSelector::ALL_SHORTEST) {
            break;
        }
        // For SHORTEST_K, continue if we haven't found enough.
        if (found_at_this_depth && path_selector_ == PathSelector::SHORTEST_K &&
            static_cast<int32_t>(result_paths.size()) >= shortest_k_) {
            break;
        }
    }

    return ok(std::move(result_paths));
}

Result<void> MatchShortestPathOperator::execute_shortest_paths() {
    if (config_.nodes.size() < 2 || config_.edges.empty()) {
        return ok();
    }

    // Get the variable-length edge (there should be exactly one for shortest path).
    const auto& edge_def = config_.edges[0];
    const auto& src_node = config_.nodes[0];
    const auto& tgt_node = config_.nodes[config_.nodes.size() - 1];

    if (src_node.label.empty() || tgt_node.label.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "MATCH shortest path nodes must have labels");
    }

    int32_t min_hops = edge_def.min_hops.value_or(0);
    int32_t max_depth = edge_def.max_hops.value_or(100);

    // Look up table IDs so we can key BFS state by (table_id, pk) and avoid
    // cross-table PK collisions when src and tgt belong to different tables
    // (heterogeneous edges — GDB-851).
    auto src_schema = catalog_.get_table(database_id_, src_node.label);
    if (!src_schema) {
        return tl::unexpected(src_schema.error());
    }
    const table_id_t src_table_id = src_schema->table_id;

    auto tgt_schema = catalog_.get_table(database_id_, tgt_node.label);
    if (!tgt_schema) {
        return tl::unexpected(tgt_schema.error());
    }
    const table_id_t tgt_table_id = tgt_schema->table_id;

    // Get all source and target PKs.
    auto src_pks = get_all_pks(src_node.label);
    if (!src_pks) {
        return tl::unexpected(src_pks.error());
    }

    auto tgt_pks = get_all_pks(tgt_node.label);
    if (!tgt_pks) {
        return tl::unexpected(tgt_pks.error());
    }

    // For each source-target pair, find shortest path(s).
    for (const auto& src_pk : *src_pks) {
        for (const auto& tgt_pk : *tgt_pks) {
            auto paths = weight_expr_ ? find_weighted_shortest_paths(src_pk,
                                                                     tgt_pk,
                                                                     src_table_id,
                                                                     tgt_table_id,
                                                                     edge_def.edge_type,
                                                                     edge_def.direction,
                                                                     min_hops,
                                                                     max_depth)
                                      : find_shortest_paths(src_pk,
                                                            tgt_pk,
                                                            src_table_id,
                                                            tgt_table_id,
                                                            edge_def.edge_type,
                                                            edge_def.direction,
                                                            min_hops,
                                                            max_depth);
            if (!paths) {
                return tl::unexpected(paths.error());
            }

            for (auto& path : *paths) {
                std::unordered_map<std::string, Value> binding;
                binding[src_node.variable] = src_pk;
                binding[tgt_node.variable] = tgt_pk;

                auto tuple_result = binding_to_tuple(binding, &path);
                if (tuple_result) {
                    results_.push_back(std::move(*tuple_result));
                }
            }
        }
    }

    return ok();
}

namespace {

/// Extract a numeric weight from a Value. Returns an error for non-numeric or negative values.
Result<double> value_to_weight(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WEIGHT expression evaluated to NULL");
    }
    double w = 0.0;
    if (auto* p = std::get_if<double>(&v.data())) {
        w = *p;
    } else if (auto* p = std::get_if<float>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<int64_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<int32_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<int16_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<int8_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<uint64_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<uint32_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<uint16_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else if (auto* p = std::get_if<uint8_t>(&v.data())) {
        w = static_cast<double>(*p);
    } else {
        return make_error(StatusCode::TYPE_ERROR,
                          "WEIGHT expression must evaluate to a numeric type");
    }
    if (w < 0.0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WEIGHT value must be non-negative, got " + std::to_string(w));
    }
    return ok(w);
}

} // namespace

Result<std::vector<std::tuple<Value, int64_t, double>>>
MatchShortestPathOperator::get_neighbors_with_weight(const std::string& edge_type,
                                                     const Value& pk,
                                                     TraverseDirection direction) {
    // Find the property index for the weight expression.
    // The weight_expr_ is a ColumnRefExpr like r.distance — we need the column name.
    const auto* col_ref = dynamic_cast<const ColumnRefExpr*>(weight_expr_);
    if (!col_ref) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WEIGHT expression must be a column reference (e.g., r.distance)");
    }
    const std::string& prop_name = col_ref->column;

    // Look up property index from EdgeTable config.
    auto et = graph_engine_.get_edge_table(database_id_, edge_type);
    if (!et) {
        return tl::unexpected(et.error());
    }
    const auto& prop_cols = (*et)->config().property_columns;
    int prop_idx = -1;
    for (size_t i = 0; i < prop_cols.size(); ++i) {
        if (prop_cols[i].name == prop_name) {
            prop_idx = static_cast<int>(i);
            break;
        }
    }
    if (prop_idx < 0) {
        return make_error(StatusCode::NOT_FOUND,
                          "edge type '" + edge_type + "' has no property '" + prop_name + "'");
    }

    std::vector<std::tuple<Value, int64_t, double>> neighbors;

    if (direction == TraverseDirection::OUT || direction == TraverseDirection::BOTH) {
        auto fwd = graph_engine_.get_edges_from(database_id_, edge_type, pk);
        if (!fwd) {
            return tl::unexpected(fwd.error());
        }
        for (auto& e : *fwd) {
            if (static_cast<size_t>(prop_idx) >= e.properties.size()) {
                return make_error(StatusCode::INTERNAL_ERROR, "edge missing weight property");
            }
            auto w = value_to_weight(e.properties[static_cast<size_t>(prop_idx)]);
            if (!w) {
                return tl::unexpected(w.error());
            }
            neighbors.emplace_back(std::move(e.target_pk), static_cast<int64_t>(e.edge_row_id), *w);
        }
    }
    if (direction == TraverseDirection::IN || direction == TraverseDirection::BOTH) {
        auto rev = graph_engine_.get_edges_to(database_id_, edge_type, pk);
        if (!rev) {
            return tl::unexpected(rev.error());
        }
        for (auto& e : *rev) {
            if (static_cast<size_t>(prop_idx) >= e.properties.size()) {
                return make_error(StatusCode::INTERNAL_ERROR, "edge missing weight property");
            }
            auto w = value_to_weight(e.properties[static_cast<size_t>(prop_idx)]);
            if (!w) {
                return tl::unexpected(w.error());
            }
            neighbors.emplace_back(std::move(e.source_pk), static_cast<int64_t>(e.edge_row_id), *w);
        }
    }

    return ok(std::move(neighbors));
}

Result<std::vector<Path>>
MatchShortestPathOperator::find_weighted_shortest_paths(const Value& src_pk,
                                                        const Value& tgt_pk,
                                                        table_id_t src_table_id,
                                                        table_id_t tgt_table_id,
                                                        const std::string& edge_type,
                                                        TraverseDirection direction,
                                                        int32_t min_hops,
                                                        int32_t max_depth) {
    std::vector<Path> result_paths;

    auto src_int = pk_to_int64(src_pk);
    if (!src_int) {
        return tl::unexpected(src_int.error());
    }

    // Trivial case: source == target — requires same table AND same pk (GDB-851).
    // Only emit the 0-hop self-path when min_hops == 0; if min_hops >= 1 the
    // quantifier requires at least one edge traversal and the self-path is
    // only valid if reached via a real cycle (GDB-858).
    if (src_table_id == tgt_table_id && ValueEqual{}(src_pk, tgt_pk) && min_hops == 0) {
        Path p;
        p.steps.push_back({*src_int, -1});
        p.total_weight = 0.0;
        result_paths.push_back(std::move(p));
        return ok(std::move(result_paths));
    }

    // Dijkstra's algorithm with priority queue.
    struct DijkstraEntry {
        double cost;
        NodeId current_node;
        Path path_so_far;

        bool operator>(const DijkstraEntry& other) const { return cost > other.cost; }
    };

    std::priority_queue<DijkstraEntry, std::vector<DijkstraEntry>, std::greater<>> pq;
    // Key best_cost by NodeId (table_id, pk) to avoid cross-table PK collisions
    // that would make Dijkstra believe it has already found a cheaper path to an
    // unrelated node in the other table (GDB-851).
    std::unordered_map<NodeId, double, NodeIdHash> best_cost;
    double best_dest_cost = std::numeric_limits<double>::infinity();
    size_t total_visited = 0;

    // Seed from source.
    NodeId src_node{src_table_id, src_pk};
    DijkstraEntry start;
    start.cost = 0.0;
    start.current_node = src_node;
    start.path_so_far.steps.push_back({*src_int, -1});
    start.path_so_far.total_weight = 0.0;
    pq.push(std::move(start));
    best_cost[src_node] = 0.0;

    while (!pq.empty()) {
        auto entry = std::move(const_cast<DijkstraEntry&>(pq.top()));
        pq.pop();
        ++total_visited;

        if (total_visited > max_visited_) {
            break;
        }

        // If we've already found a path to the destination, check whether
        // further exploration can yield new shortest paths. For ALL_SHORTEST,
        // entries at exactly best_dest_cost may still discover equal-cost
        // paths (e.g. via zero-weight final edges), so use strict >.
        // For SHORTEST_K, terminate only when we have K paths AND the current
        // entry cost exceeds the K-th best — since Dijkstra processes by cost
        // order, no cheaper destination paths can be discovered after this.
        bool can_terminate = false;
        if (path_selector_ == PathSelector::ALL_SHORTEST) {
            can_terminate = entry.cost > best_dest_cost;
        } else if (path_selector_ == PathSelector::SHORTEST_K) {
            if (static_cast<int32_t>(result_paths.size()) >= shortest_k_) {
                // Find the K-th cheapest path cost.
                double kth_cost = 0.0;
                std::vector<double> costs;
                costs.reserve(result_paths.size());
                for (const auto& p : result_paths) {
                    costs.push_back(p.total_weight);
                }
                std::sort(costs.begin(), costs.end());
                kth_cost = costs[static_cast<size_t>(shortest_k_) - 1];
                can_terminate = entry.cost > kth_cost;
            }
        } else {
            can_terminate = entry.cost >= best_dest_cost;
        }
        if (can_terminate && !result_paths.empty()) {
            if (path_selector_ == PathSelector::ANY_SHORTEST) {
                result_paths.resize(1);
            } else if (path_selector_ == PathSelector::SHORTEST_K &&
                       static_cast<int32_t>(result_paths.size()) > shortest_k_) {
                result_paths.resize(shortest_k_);
            }
            return ok(std::move(result_paths));
        }

        // Skip if we've already found a better path to this node (keyed by NodeId).
        auto it = best_cost.find(entry.current_node);
        if (it != best_cost.end() && entry.cost > it->second) {
            continue;
        }

        // Check max depth (path length in hops).
        if (static_cast<int32_t>(entry.path_so_far.length()) >= max_depth) {
            continue;
        }

        auto neighbors = get_neighbors_with_weight(edge_type, entry.current_node.pk, direction);
        if (!neighbors) {
            return tl::unexpected(neighbors.error());
        }

        // Neighbors of a src-table node arrive in the tgt table (OUT), and vice versa.
        table_id_t nbr_table_id =
            (entry.current_node.table_id == src_table_id) ? tgt_table_id : src_table_id;

        for (auto& [nbr_pk, edge_id, weight] : *neighbors) {
            double new_cost = entry.cost + weight;

            auto nbr_int = pk_to_int64(nbr_pk);
            if (!nbr_int) {
                return tl::unexpected(nbr_int.error());
            }

            NodeId nbr_node{nbr_table_id, nbr_pk};

            // Build new path.
            Path new_path = entry.path_so_far;
            new_path.steps.back().edge_id = edge_id;
            new_path.steps.push_back({*nbr_int, -1});
            new_path.total_weight = new_cost;

            // Check if we've reached the target (table AND pk must match — GDB-851).
            // Also enforce quantifier lower bound: only accept paths with length >=
            // min_hops.  A direct self-loop under {1,N} must pass (length 1 >=
            // min_hops 1); under {2,N} it must be rejected (GDB-1272).
            if (nbr_node == NodeId{tgt_table_id, tgt_pk}) {
                if (new_path.length() >= static_cast<int64_t>(min_hops)) {
                    if (path_selector_ == PathSelector::SHORTEST_K) {
                        // For SHORTEST_K, accept all paths — we sort and trim later.
                        if (new_cost < best_dest_cost) {
                            best_dest_cost = new_cost;
                        }
                        result_paths.push_back(std::move(new_path));
                    } else {
                        // Skip paths that exceed the best known destination cost.
                        if (new_cost > best_dest_cost) {
                            continue;
                        }
                        if (new_cost < best_dest_cost) {
                            best_dest_cost = new_cost;
                            result_paths.clear();
                        }
                        result_paths.push_back(std::move(new_path));
                    }
                }
                continue;
            }

            // Only visit if we found a better (or equal, for ALL_SHORTEST) cost.
            // For SHORTEST_K, allow revisits at higher costs to find K paths.
            // Key by NodeId to prevent cross-table PK collisions (GDB-851).
            auto cost_it = best_cost.find(nbr_node);
            bool dominated = false;
            if (cost_it != best_cost.end()) {
                if (path_selector_ == PathSelector::ALL_SHORTEST) {
                    dominated = new_cost > cost_it->second;
                } else if (path_selector_ == PathSelector::SHORTEST_K) {
                    // Never dominate for SHORTEST_K — we need alternative paths.
                    dominated = false;
                } else {
                    dominated = new_cost >= cost_it->second;
                }
            }
            if (!dominated) {
                if (cost_it == best_cost.end() || new_cost < cost_it->second) {
                    best_cost[nbr_node] = new_cost;
                }

                DijkstraEntry next;
                next.cost = new_cost;
                next.current_node = std::move(nbr_node);
                next.path_so_far = std::move(new_path);
                pq.push(std::move(next));
            }
        }
    }

    if (path_selector_ == PathSelector::ANY_SHORTEST && result_paths.size() > 1) {
        result_paths.resize(1);
    } else if (path_selector_ == PathSelector::SHORTEST_K &&
               static_cast<int32_t>(result_paths.size()) > shortest_k_) {
        // Sort by total weight so we keep the K cheapest paths.
        std::sort(result_paths.begin(), result_paths.end(), [](const Path& a, const Path& b) {
            return a.total_weight < b.total_weight;
        });
        result_paths.resize(shortest_k_);
    }
    return ok(std::move(result_paths));
}

} // namespace sixseven
