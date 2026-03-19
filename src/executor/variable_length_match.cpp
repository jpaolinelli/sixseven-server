#include "sixseven/executor/variable_length_match.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

VariableLengthMatchOperator::VariableLengthMatchOperator(GraphEngine& graph_engine,
                                                         const Catalog& catalog,
                                                         StorageManager& storage,
                                                         database_id_t database_id,
                                                         MatchConfig config,
                                                         OutputSchema schema,
                                                         const Expr* where_expr,
                                                         const BoundStatement& bound,
                                                         size_t max_visited)
    : graph_engine_(graph_engine), catalog_(catalog), storage_(storage), database_id_(database_id),
      config_(std::move(config)), schema_(std::move(schema)), where_expr_(where_expr),
      bound_(bound), max_visited_(max_visited) {}

std::string VariableLengthMatchOperator::plan_node_name() const {
    return "Variable Length Match";
}

std::string VariableLengthMatchOperator::plan_node_detail() const {
    // Show the quantifier range for the variable-length edge.
    for (const auto& edge : config_.edges) {
        if (edge.is_variable_length()) {
            std::string detail = edge.edge_type + "{" + std::to_string(*edge.min_hops) + ",";
            if (edge.max_hops) {
                detail += std::to_string(*edge.max_hops);
            }
            detail += "}";
            return detail;
        }
    }
    return "";
}

Result<void> VariableLengthMatchOperator::do_open() {
    results_.clear();
    cursor_ = 0;
    return execute_variable_length();
}

Result<std::optional<Tuple>> VariableLengthMatchOperator::do_next() {
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

void VariableLengthMatchOperator::do_close() {
    results_.clear();
    cursor_ = 0;
}

const OutputSchema& VariableLengthMatchOperator::output_schema() const {
    return schema_;
}

Result<std::vector<Value>>
VariableLengthMatchOperator::get_all_pks(const std::string& table_name) const {
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

Result<std::vector<std::pair<Value, int64_t>>> VariableLengthMatchOperator::get_neighbors(
    const std::string& edge_type, const Value& pk, TraverseDirection direction) const {
    std::vector<std::pair<Value, int64_t>> neighbors;

    if (direction == TraverseDirection::OUT || direction == TraverseDirection::BOTH) {
        auto fwd = graph_engine_.get_edges_from(database_id_, edge_type, pk);
        if (fwd) {
            for (auto& e : *fwd) {
                neighbors.emplace_back(std::move(e.target_pk), static_cast<int64_t>(e.edge_row_id));
            }
        }
    }
    if (direction == TraverseDirection::IN || direction == TraverseDirection::BOTH) {
        auto rev = graph_engine_.get_edges_to(database_id_, edge_type, pk);
        if (rev) {
            for (auto& e : *rev) {
                neighbors.emplace_back(std::move(e.source_pk), static_cast<int64_t>(e.edge_row_id));
            }
        }
    }

    return ok(std::move(neighbors));
}

Result<std::vector<Value>>
VariableLengthMatchOperator::fetch_node_data(const std::string& table_name, const Value& pk) const {
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
VariableLengthMatchOperator::binding_to_tuple(const std::unordered_map<std::string, Value>& binding,
                                              const Path* path_value) const {
    Tuple tuple;

    for (size_t col_idx = 0; col_idx < schema_.column_count(); ++col_idx) {
        const auto& out_col = schema_.column(col_idx);

        // Check if this is a path_length() or path column.
        // For now, path data is exposed through the special "path" variable
        // if the edge variable is used as a path reference.

        // Try to find a variable binding for this column.
        auto bit = binding.find(out_col.table_name);
        if (bit != binding.end()) {
            // We have the PK for this variable. Fetch the column value.
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

        // Check if this column references a path variable.
        if (path_value != nullptr && out_col.name == "path") {
            tuple.values.push_back(Value(*path_value));
            continue;
        }

        tuple.values.push_back(Value());
    }

    return ok(std::move(tuple));
}

Result<bool> VariableLengthMatchOperator::evaluate_node_filter(const MatchNodeDef& node_def,
                                                               const Value& pk) const {
    if (!node_def.filter_expr) {
        return ok(true);
    }
    if (node_def.label.empty()) {
        return ok(true);
    }

    auto tbl_schema = catalog_.get_table(database_id_, node_def.label);
    if (!tbl_schema) {
        return tl::unexpected(tbl_schema.error());
    }

    auto node_data = fetch_node_data(node_def.label, pk);
    if (!node_data) {
        return ok(false); // Node not found, predicate fails.
    }

    // Build temporary OutputSchema and Tuple for predicate evaluation.
    std::vector<OutputColumn> cols;
    for (const auto& col : tbl_schema->columns) {
        OutputColumn oc;
        oc.table_name = node_def.variable;
        oc.name = col.name;
        oc.type_id = col.type_id;
        oc.nullable = col.nullable;
        oc.table_id = tbl_schema->table_id;
        cols.push_back(std::move(oc));
    }
    OutputSchema temp_schema(std::move(cols));
    Tuple temp_tuple;
    temp_tuple.values = *node_data;

    return evaluate_predicate(*node_def.filter_expr, temp_tuple, temp_schema, bound_);
}

Result<bool>
VariableLengthMatchOperator::evaluate_edge_filter(const MatchEdgeDef& edge_def,
                                                  const std::vector<Value>& properties) const {
    if (!edge_def.filter_expr) {
        return ok(true);
    }

    auto edge_type = catalog_.get_edge_type(database_id_, edge_def.edge_type);
    if (!edge_type) {
        return tl::unexpected(edge_type.error());
    }

    // Build temporary OutputSchema from edge property definitions.
    std::vector<OutputColumn> cols;
    if (!edge_type->properties.empty()) {
        std::string_view props(edge_type->properties);
        while (!props.empty()) {
            auto comma = props.find(',');
            auto entry = props.substr(0, comma);
            props = (comma == std::string_view::npos) ? "" : props.substr(comma + 1);
            auto colon = entry.find(':');
            std::string prop_name(entry.substr(0, colon));
            TypeId prop_type = TypeId::STRING;
            if (colon != std::string_view::npos) {
                auto type_str = std::string(entry.substr(colon + 1));
                // Map common type names to TypeId.
                if (type_str == "INT32" || type_str == "INT" || type_str == "INTEGER") {
                    prop_type = TypeId::INT32;
                } else if (type_str == "INT64" || type_str == "BIGINT") {
                    prop_type = TypeId::INT64;
                } else if (type_str == "FLOAT32" || type_str == "FLOAT") {
                    prop_type = TypeId::FLOAT32;
                } else if (type_str == "FLOAT64" || type_str == "DOUBLE") {
                    prop_type = TypeId::FLOAT64;
                } else if (type_str == "BOOL" || type_str == "BOOLEAN") {
                    prop_type = TypeId::BOOL;
                } else if (type_str == "DATE") {
                    prop_type = TypeId::DATE;
                } else if (type_str == "TIMESTAMP") {
                    prop_type = TypeId::TIMESTAMP;
                }
            }
            OutputColumn oc;
            oc.table_name = edge_def.variable;
            oc.name = prop_name;
            oc.type_id = prop_type;
            oc.nullable = true;
            cols.push_back(std::move(oc));
        }
    }
    OutputSchema temp_schema(std::move(cols));
    Tuple temp_tuple;
    temp_tuple.values = properties;

    return evaluate_predicate(*edge_def.filter_expr, temp_tuple, temp_schema, bound_);
}

Result<void> VariableLengthMatchOperator::execute_variable_length() {
    if (config_.nodes.size() < 2 || config_.edges.empty()) {
        return ok();
    }

    // Multi-segment algorithm: process edges sequentially.
    // For fixed-length edges: single-hop expansion (like PatternMatchOperator).
    // For variable-length edges: BFS expansion with min/max hop bounds.
    //
    // We maintain a set of "bindings" (variable -> PK maps) and expand them
    // through each edge segment in order.

    using Binding = std::unordered_map<std::string, Value>;

    // Seed bindings with all PKs from the first node's table.
    const auto& first_node = config_.nodes[0];
    if (first_node.label.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "MATCH source node must have a label");
    }

    auto src_pks = get_all_pks(first_node.label);
    if (!src_pks) {
        return tl::unexpected(src_pks.error());
    }

    std::vector<Binding> bindings;
    for (const auto& pk : *src_pks) {
        // Apply source node inline predicate.
        if (first_node.filter_expr) {
            auto pass = evaluate_node_filter(first_node, pk);
            if (!pass || !*pass) {
                continue;
            }
        }
        Binding b;
        b[first_node.variable] = pk;
        bindings.push_back(std::move(b));
    }

    size_t total_visited = 0;

    // Process each edge segment.
    for (size_t ei = 0; ei < config_.edges.size(); ++ei) {
        const auto& edge_def = config_.edges[ei];
        const auto& src_var = config_.nodes[ei].variable;
        const auto& tgt_var = config_.nodes[ei + 1].variable;

        std::vector<Binding> new_bindings;

        const auto& tgt_node = config_.nodes[ei + 1];

        if (!edge_def.is_variable_length()) {
            // Fixed-length edge: single-hop expansion.
            for (auto& binding : bindings) {
                auto it = binding.find(src_var);
                if (it == binding.end()) {
                    continue;
                }

                // Use graph engine directly to access edge properties for filtering.
                std::vector<EdgeRow> edge_rows;
                if (edge_def.direction == TraverseDirection::OUT ||
                    edge_def.direction == TraverseDirection::BOTH) {
                    auto fwd =
                        graph_engine_.get_edges_from(database_id_, edge_def.edge_type, it->second);
                    if (fwd) {
                        for (auto& e : *fwd) {
                            edge_rows.push_back(std::move(e));
                        }
                    }
                }
                if (edge_def.direction == TraverseDirection::IN ||
                    edge_def.direction == TraverseDirection::BOTH) {
                    auto rev =
                        graph_engine_.get_edges_to(database_id_, edge_def.edge_type, it->second);
                    if (rev) {
                        for (auto& e : *rev) {
                            edge_rows.push_back(std::move(e));
                        }
                    }
                }

                for (auto& edge_row : edge_rows) {
                    // Determine neighbor PK based on direction.
                    Value& nbr_pk = (edge_def.direction == TraverseDirection::IN)
                                        ? edge_row.source_pk
                                        : edge_row.target_pk;

                    // Apply edge inline predicate.
                    if (edge_def.filter_expr) {
                        auto pass = evaluate_edge_filter(edge_def, edge_row.properties);
                        if (!pass || !*pass) {
                            continue;
                        }
                    }

                    // Apply target node inline predicate.
                    if (tgt_node.filter_expr) {
                        auto pass = evaluate_node_filter(tgt_node, nbr_pk);
                        if (!pass || !*pass) {
                            continue;
                        }
                    }
                    Binding new_b = binding;
                    new_b[tgt_var] = std::move(nbr_pk);
                    new_bindings.push_back(std::move(new_b));
                }
            }
        } else {
            // Variable-length edge: BFS expansion with min/max hops.
            int32_t min_hops = edge_def.min_hops.value_or(1);
            int32_t max_hops = edge_def.max_hops.value_or(20);

            struct BfsEntry {
                Value current_pk;
                int32_t depth;
                std::unordered_set<Value, ValueHash, ValueEqual> visited;
            };

            for (auto& binding : bindings) {
                auto it = binding.find(src_var);
                if (it == binding.end()) {
                    continue;
                }

                const Value& start_pk = it->second;

                std::queue<BfsEntry> queue;
                BfsEntry start;
                start.current_pk = start_pk;
                start.depth = 0;
                start.visited.insert(start_pk);
                queue.push(std::move(start));

                while (!queue.empty()) {
                    if (total_visited >= max_visited_) {
                        return make_error(StatusCode::INVALID_ARGUMENT,
                                          "variable-length MATCH exceeded max_visited limit (" +
                                              std::to_string(max_visited_) + ")");
                    }

                    auto entry = std::move(queue.front());
                    queue.pop();
                    ++total_visited;

                    // Apply target node predicate (prunes: skip emit AND expansion).
                    if (entry.depth > 0 && tgt_node.filter_expr) {
                        auto pass = evaluate_node_filter(tgt_node, entry.current_pk);
                        if (!pass || !*pass) {
                            continue;
                        }
                    }

                    // Emit result at valid depths.
                    if (entry.depth >= min_hops && entry.depth <= max_hops) {
                        Binding new_b = binding;
                        new_b[tgt_var] = entry.current_pk;
                        new_bindings.push_back(std::move(new_b));
                    }

                    // Expand if we haven't reached max depth.
                    if (entry.depth < max_hops) {
                        // Get full edge rows for edge predicate evaluation.
                        std::vector<EdgeRow> edge_rows;
                        if (edge_def.direction == TraverseDirection::OUT ||
                            edge_def.direction == TraverseDirection::BOTH) {
                            auto fwd = graph_engine_.get_edges_from(
                                database_id_, edge_def.edge_type, entry.current_pk);
                            if (fwd) {
                                for (auto& e : *fwd) {
                                    edge_rows.push_back(std::move(e));
                                }
                            }
                        }
                        if (edge_def.direction == TraverseDirection::IN ||
                            edge_def.direction == TraverseDirection::BOTH) {
                            auto rev = graph_engine_.get_edges_to(
                                database_id_, edge_def.edge_type, entry.current_pk);
                            if (rev) {
                                for (auto& e : *rev) {
                                    edge_rows.push_back(std::move(e));
                                }
                            }
                        }

                        for (auto& edge_row : edge_rows) {
                            Value nbr_pk = (edge_def.direction == TraverseDirection::IN)
                                               ? edge_row.source_pk
                                               : edge_row.target_pk;
                            if (entry.visited.count(nbr_pk) > 0) {
                                continue;
                            }

                            // Apply edge inline predicate.
                            if (edge_def.filter_expr) {
                                auto pass = evaluate_edge_filter(edge_def, edge_row.properties);
                                if (!pass || !*pass) {
                                    continue;
                                }
                            }

                            BfsEntry next;
                            next.depth = entry.depth + 1;
                            next.visited = entry.visited;
                            next.visited.insert(nbr_pk);
                            next.current_pk = std::move(nbr_pk);
                            queue.push(std::move(next));
                        }
                    }
                }
            }
        }

        bindings = std::move(new_bindings);
    }

    // Convert final bindings to output tuples.
    for (auto& binding : bindings) {
        auto tuple_result = binding_to_tuple(binding, nullptr);
        if (tuple_result) {
            results_.push_back(std::move(*tuple_result));
        }
    }

    return ok();
}

} // namespace sixseven
