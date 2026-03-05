#include "sixseven/executor/pattern_match.h"

#include "sixseven/common/coercion.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

#include <unordered_map>

namespace sixseven {

PatternMatchOperator::PatternMatchOperator(GraphEngine& graph_engine,
                                           const Catalog& catalog,
                                           StorageManager& storage,
                                           database_id_t database_id,
                                           MatchConfig config,
                                           OutputSchema schema,
                                           const Expr* where_expr,
                                           const BoundStatement& bound)
    : graph_engine_(graph_engine), catalog_(catalog), storage_(storage), database_id_(database_id),
      config_(std::move(config)), schema_(std::move(schema)), where_expr_(where_expr),
      bound_(bound) {}

std::string PatternMatchOperator::plan_node_name() const {
    return "Pattern Match";
}
std::string PatternMatchOperator::plan_node_detail() const {
    return "";
}

Result<void> PatternMatchOperator::do_open() {
    results_.clear();
    cursor_ = 0;

    if (config_.edges.empty()) {
        // No edges in pattern — just return nodes from the table.
        return ok();
    }

    if (config_.edges.size() == 1) {
        return execute_single_hop();
    }

    return execute_multi_hop();
}

Result<std::optional<Tuple>> PatternMatchOperator::do_next() {
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

void PatternMatchOperator::do_close() {
    results_.clear();
    cursor_ = 0;
}

const OutputSchema& PatternMatchOperator::output_schema() const {
    return schema_;
}

Result<void> PatternMatchOperator::execute_single_hop() {
    const auto& edge_def = config_.edges[0];
    const auto& src_node = config_.nodes[0];
    const auto& tgt_node = config_.nodes.size() > 1 ? config_.nodes[1] : config_.nodes[0];

    auto edge_table = graph_engine_.get_edge_table(edge_def.edge_type);
    if (!edge_table) {
        return tl::unexpected(edge_table.error());
    }

    // Get all edges from the edge table by scanning all source nodes.
    // For a single-hop pattern, we iterate all edges in the table.
    auto* et = *edge_table;
    size_t edge_count = et->size();
    if (edge_count == 0) {
        return ok();
    }

    // Since EdgeTable doesn't have a full scan, we need a different approach.
    // We'll fetch all table rows from the source table, then for each PK,
    // look up edges.
    std::string src_table_name = src_node.label;
    if (src_table_name.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "MATCH node must have a label");
    }

    auto src_schema = catalog_.get_table(database_id_, src_table_name);
    if (!src_schema) {
        return tl::unexpected(src_schema.error());
    }

    auto src_storage = storage_.get_table_storage(src_schema->table_id);
    if (!src_storage) {
        return tl::unexpected(src_storage.error());
    }

    // Find PK column index.
    int pk_col_idx = -1;
    for (size_t i = 0; i < src_schema->columns.size(); ++i) {
        if (src_schema->columns[i].name == src_schema->pk_columns) {
            pk_col_idx = static_cast<int>(i);
            break;
        }
    }
    if (pk_col_idx < 0) {
        return make_error(StatusCode::INTERNAL_ERROR, "source table has no primary key for MATCH");
    }

    // Scan source table.
    auto iter = (*src_storage)->heap->begin();
    if (!iter) {
        return tl::unexpected(iter.error());
    }

    while (true) {
        auto row = iter->next();
        if (!row) {
            break;
        }

        auto [rid, data] = *row;

        auto deserialized = TupleSerializer::deserialize(data, (*src_storage)->storage_schema);
        if (!deserialized) {
            continue;
        }

        if (static_cast<size_t>(pk_col_idx) >= deserialized->size()) {
            continue;
        }

        Value src_pk = (*deserialized)[static_cast<size_t>(pk_col_idx)];

        // Get edges from this source node in the appropriate direction.
        // Each entry is (edge, neighbor_pk) — neighbor_pk is already resolved
        // based on which index the edge came from.
        std::vector<std::pair<EdgeRow, Value>> edges_with_neighbor;
        if (edge_def.direction == TraverseDirection::OUT ||
            edge_def.direction == TraverseDirection::BOTH) {
            auto fwd = graph_engine_.get_edges_from(edge_def.edge_type, src_pk);
            if (fwd) {
                for (auto& e : *fwd) {
                    Value nbr = e.target_pk;
                    edges_with_neighbor.emplace_back(std::move(e), std::move(nbr));
                }
            }
        }
        if (edge_def.direction == TraverseDirection::IN ||
            edge_def.direction == TraverseDirection::BOTH) {
            auto rev = graph_engine_.get_edges_to(edge_def.edge_type, src_pk);
            if (rev) {
                for (auto& e : *rev) {
                    Value nbr = e.source_pk;
                    edges_with_neighbor.emplace_back(std::move(e), std::move(nbr));
                }
            }
        }

        for (auto& [edge, tgt_pk] : edges_with_neighbor) {
            // Build output tuple with columns from both nodes.
            // The output schema was built by the binder based on RETURN items.
            // We need to populate values matching the schema columns.
            Tuple tuple;

            // For each column in the output schema, fill the appropriate value.
            for (size_t col_idx = 0; col_idx < schema_.column_count(); ++col_idx) {
                const auto& out_col = schema_.column(col_idx);

                if (out_col.table_name == src_node.variable) {
                    // Column from source node.
                    auto col_it =
                        std::find_if(src_schema->columns.begin(),
                                     src_schema->columns.end(),
                                     [&](const auto& c) { return c.name == out_col.name; });
                    if (col_it != src_schema->columns.end()) {
                        size_t idx =
                            static_cast<size_t>(std::distance(src_schema->columns.begin(), col_it));
                        if (idx < deserialized->size()) {
                            tuple.values.push_back((*deserialized)[idx]);
                        } else {
                            tuple.values.push_back(Value());
                        }
                    } else {
                        tuple.values.push_back(Value());
                    }
                } else if (out_col.table_name == tgt_node.variable && !tgt_node.label.empty()) {
                    // Column from target node — need to fetch target row.
                    auto tgt_data = fetch_node_data(tgt_node.label, tgt_pk);
                    if (tgt_data) {
                        auto tgt_schema = catalog_.get_table(database_id_, tgt_node.label);
                        if (tgt_schema) {
                            auto tgt_col_it =
                                std::find_if(tgt_schema->columns.begin(),
                                             tgt_schema->columns.end(),
                                             [&](const auto& c) { return c.name == out_col.name; });
                            if (tgt_col_it != tgt_schema->columns.end()) {
                                size_t idx = static_cast<size_t>(
                                    std::distance(tgt_schema->columns.begin(), tgt_col_it));
                                if (idx < tgt_data->size()) {
                                    tuple.values.push_back((*tgt_data)[idx]);
                                } else {
                                    tuple.values.push_back(Value());
                                }
                            } else {
                                tuple.values.push_back(Value());
                            }
                        } else {
                            tuple.values.push_back(Value());
                        }
                    } else {
                        tuple.values.push_back(Value());
                    }
                } else {
                    tuple.values.push_back(Value());
                }
            }

            results_.push_back(std::move(tuple));
        }
    }

    return ok();
}

Result<void> PatternMatchOperator::execute_multi_hop() {
    // Multi-hop: chain of joins.
    // Start with all source nodes, then for each edge in the pattern,
    // expand through the edge table.

    if (config_.nodes.empty()) {
        return ok();
    }

    // Intermediate results: each row is a map of variable -> pk.
    using Binding = std::unordered_map<std::string, Value>;
    std::vector<Binding> bindings;

    // Seed with all PKs from the first node's table.
    const auto& first_node = config_.nodes[0];
    if (first_node.label.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "first MATCH node must have a label");
    }

    auto src_schema = catalog_.get_table(database_id_, first_node.label);
    if (!src_schema) {
        return tl::unexpected(src_schema.error());
    }

    auto src_storage = storage_.get_table_storage(src_schema->table_id);
    if (!src_storage) {
        return tl::unexpected(src_storage.error());
    }

    int pk_col_idx = -1;
    for (size_t i = 0; i < src_schema->columns.size(); ++i) {
        if (src_schema->columns[i].name == src_schema->pk_columns) {
            pk_col_idx = static_cast<int>(i);
            break;
        }
    }
    if (pk_col_idx < 0) {
        return make_error(StatusCode::INTERNAL_ERROR, "source table has no primary key for MATCH");
    }

    auto iter = (*src_storage)->heap->begin();
    if (!iter) {
        return tl::unexpected(iter.error());
    }

    while (true) {
        auto row = iter->next();
        if (!row) {
            break;
        }

        auto [rid, data] = *row;
        auto deserialized = TupleSerializer::deserialize(data, (*src_storage)->storage_schema);
        if (!deserialized) {
            continue;
        }

        if (static_cast<size_t>(pk_col_idx) >= deserialized->size()) {
            continue;
        }

        Binding b;
        b[first_node.variable] = (*deserialized)[static_cast<size_t>(pk_col_idx)];
        bindings.push_back(std::move(b));
    }

    // For each edge in the pattern, expand bindings.
    for (size_t ei = 0; ei < config_.edges.size(); ++ei) {
        const auto& edge_def = config_.edges[ei];
        const auto& src_var = config_.nodes[ei].variable;
        const auto& tgt_var = config_.nodes[ei + 1].variable;

        std::vector<Binding> new_bindings;

        for (auto& binding : bindings) {
            auto it = binding.find(src_var);
            if (it == binding.end()) {
                continue;
            }

            const Value& src_pk = it->second;

            std::vector<Value> neighbor_pks;
            if (edge_def.direction == TraverseDirection::OUT ||
                edge_def.direction == TraverseDirection::BOTH) {
                auto fwd = graph_engine_.get_edges_from(edge_def.edge_type, src_pk);
                if (fwd) {
                    for (auto& e : *fwd) {
                        neighbor_pks.push_back(std::move(e.target_pk));
                    }
                }
            }
            if (edge_def.direction == TraverseDirection::IN ||
                edge_def.direction == TraverseDirection::BOTH) {
                auto rev = graph_engine_.get_edges_to(edge_def.edge_type, src_pk);
                if (rev) {
                    for (auto& e : *rev) {
                        neighbor_pks.push_back(std::move(e.source_pk));
                    }
                }
            }

            for (auto& tgt_pk : neighbor_pks) {
                Binding new_b = binding;
                new_b[tgt_var] = std::move(tgt_pk);
                new_bindings.push_back(std::move(new_b));
            }
        }

        bindings = std::move(new_bindings);
    }

    // Convert bindings to output tuples.
    for (auto& binding : bindings) {
        Tuple tuple;

        for (size_t col_idx = 0; col_idx < schema_.column_count(); ++col_idx) {
            const auto& out_col = schema_.column(col_idx);

            // Find the variable that matches this column's table_name.
            auto bit = binding.find(out_col.table_name);
            if (bit != binding.end()) {
                // We have the PK for this variable. Fetch the column value.
                // Find the node label for this variable.
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

        results_.push_back(std::move(tuple));
    }

    return ok();
}

Result<std::vector<Value>> PatternMatchOperator::fetch_node_data(const std::string& table_name,
                                                                 const Value& pk) const {
    auto schema = catalog_.get_table(database_id_, table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }

    auto ts = storage_.get_table_storage(schema->table_id);
    if (!ts) {
        return tl::unexpected(ts.error());
    }

    // Find PK column index.
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

    // Scan the table to find the matching row.
    // (In production, we'd use an index lookup, but this is correct for now.)
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

} // namespace sixseven
