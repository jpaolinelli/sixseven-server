#include "sixseven/executor/match_shortest_path.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

#include <algorithm>
#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

namespace {

int64_t pk_to_int64(const Value& pk) {
    if (!pk.is_null()) {
        if (auto* p = std::get_if<int64_t>(&pk.data())) {
            return *p;
        }
        if (auto* p32 = std::get_if<int32_t>(&pk.data())) {
            return *p32;
        }
    }
    return 0;
}

} // namespace

MatchShortestPathOperator::MatchShortestPathOperator(GraphEngine& graph_engine,
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
                                                     size_t max_visited)
    : graph_engine_(graph_engine), catalog_(catalog), storage_(storage), database_id_(database_id),
      config_(std::move(config)), schema_(std::move(schema)), where_expr_(where_expr),
      bound_(bound), path_selector_(path_selector), path_variable_(std::move(path_variable)),
      shortest_k_(shortest_k), max_visited_(max_visited) {}

std::string MatchShortestPathOperator::plan_node_name() const {
    switch (path_selector_) {
    case PathSelector::ANY_SHORTEST:
        return "Any Shortest Path Match";
    case PathSelector::ALL_SHORTEST:
        return "All Shortest Path Match";
    case PathSelector::SHORTEST_K:
        return "Shortest " + std::to_string(shortest_k_) + " Path Match";
    default:
        return "Shortest Path Match";
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
        auto row = iter->next();
        if (!row) {
            break;
        }

        auto [rid, data] = *row;
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
    std::vector<std::pair<Value, int64_t>> neighbors;

    if (direction == TraverseDirection::OUT || direction == TraverseDirection::BOTH) {
        auto fwd = graph_engine_.get_edges_from(edge_type, pk);
        if (fwd) {
            for (auto& e : *fwd) {
                neighbors.emplace_back(std::move(e.target_pk), static_cast<int64_t>(e.edge_row_id));
            }
        }
    }
    if (direction == TraverseDirection::IN || direction == TraverseDirection::BOTH) {
        auto rev = graph_engine_.get_edges_to(edge_type, pk);
        if (rev) {
            for (auto& e : *rev) {
                neighbors.emplace_back(std::move(e.source_pk), static_cast<int64_t>(e.edge_row_id));
            }
        }
    }

    return ok(std::move(neighbors));
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

    auto iter = (*ts)->heap->begin();
    if (!iter) {
        return tl::unexpected(iter.error());
    }

    while (true) {
        auto row = iter->next();
        if (!row) {
            break;
        }

        auto [rid, data] = *row;
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
                                               const std::string& edge_type,
                                               TraverseDirection direction,
                                               int32_t max_depth) {

    std::vector<Path> result_paths;

    // Check trivial case: source == target.
    if (ValueEqual{}(src_pk, tgt_pk)) {
        Path p;
        p.steps.push_back({pk_to_int64(src_pk), -1});
        result_paths.push_back(std::move(p));
        return ok(std::move(result_paths));
    }

    // BFS with path tracking.
    // Each entry tracks: current node PK, the full path so far.
    struct BfsEntry {
        Value current_pk;
        Path path_so_far;
    };

    std::deque<BfsEntry> queue;
    // For ANY_SHORTEST we only need to track visited nodes (no duplicates).
    // For ALL_SHORTEST/SHORTEST_K we track visited per-level to allow multiple paths.
    std::unordered_set<int64_t> globally_visited;
    size_t total_visited = 0;

    // Seed BFS from source.
    BfsEntry start;
    start.current_pk = src_pk;
    start.path_so_far.steps.push_back({pk_to_int64(src_pk), -1});
    queue.push_back(std::move(start));
    globally_visited.insert(pk_to_int64(src_pk));

    int32_t depth = 0;
    bool found_at_this_depth = false;

    while (!queue.empty() && depth < max_depth) {
        ++depth;
        size_t level_size = queue.size();

        // For ALL_SHORTEST: nodes discovered at this level should not block
        // other paths at the same level from reaching the same node.
        std::unordered_set<int64_t> level_new_nodes;

        for (size_t i = 0; i < level_size; ++i) {
            auto entry = std::move(queue.front());
            queue.pop_front();
            ++total_visited;

            if (total_visited > max_visited_) {
                return ok(std::move(result_paths));
            }

            auto neighbors = get_neighbors(edge_type, entry.current_pk, direction);
            if (!neighbors) {
                continue;
            }

            for (auto& [nbr_pk, edge_id] : *neighbors) {
                int64_t nbr_int = pk_to_int64(nbr_pk);

                // Build new path.
                Path new_path = entry.path_so_far;
                // Update the last step's edge_id (the edge FROM previous node TO this one).
                new_path.steps.back().edge_id = edge_id;
                // Add the new node.
                new_path.steps.push_back({nbr_int, -1});

                // Check if we've reached the target.
                if (ValueEqual{}(nbr_pk, tgt_pk)) {
                    found_at_this_depth = true;
                    result_paths.push_back(std::move(new_path));

                    if (path_selector_ == PathSelector::ANY_SHORTEST) {
                        return ok(std::move(result_paths));
                    }
                    if (path_selector_ == PathSelector::SHORTEST_K &&
                        static_cast<int32_t>(result_paths.size()) >= shortest_k_) {
                        return ok(std::move(result_paths));
                    }
                    continue;
                }

                // For ALL_SHORTEST/SHORTEST_K: allow multiple paths through same node at same
                // level.
                if (path_selector_ == PathSelector::ANY_SHORTEST) {
                    if (globally_visited.count(nbr_int) > 0) {
                        continue;
                    }
                    globally_visited.insert(nbr_int);
                } else {
                    // Don't revisit nodes from previous levels, but allow same-level visits.
                    if (globally_visited.count(nbr_int) > 0) {
                        continue;
                    }
                    level_new_nodes.insert(nbr_int);
                }

                BfsEntry next;
                next.current_pk = std::move(nbr_pk);
                next.path_so_far = std::move(new_path);
                queue.push_back(std::move(next));
            }
        }

        // After processing a level, add new nodes to globally visited.
        for (int64_t nid : level_new_nodes) {
            globally_visited.insert(nid);
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

    int32_t max_depth = edge_def.max_hops.value_or(100);

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
            auto paths = find_shortest_paths(
                src_pk, tgt_pk, edge_def.edge_type, edge_def.direction, max_depth);
            if (!paths) {
                continue;
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

} // namespace sixseven
