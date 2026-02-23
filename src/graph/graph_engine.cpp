#include "giodb/graph/graph_engine.h"

#include "giodb/common/logging.h"

namespace giodb {

GraphEngine::GraphEngine(Catalog& catalog) : catalog_(catalog) {}

Result<edge_id_t> GraphEngine::create_edge_type(const std::string& name,
                                                table_id_t source_table_id,
                                                table_id_t target_table_id,
                                                TypeId source_pk_type,
                                                TypeId target_pk_type,
                                                const std::vector<ColumnDef>& property_columns,
                                                bool prevent_duplicates) {
    std::lock_guard lock(mu_);

    // Check if edge table already exists locally.
    if (edge_tables_.count(name) > 0) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "edge type '" + name + "' already exists in graph engine");
    }

    // Build property list for catalog (comma-separated column names).
    std::string props_str;
    for (size_t i = 0; i < property_columns.size(); ++i) {
        if (i > 0)
            props_str += ",";
        props_str += property_columns[i].name;
    }

    // Register in catalog.
    EdgeTypeDef def;
    def.name = name;
    def.source_table_id = source_table_id;
    def.target_table_id = target_table_id;
    def.properties = props_str;

    auto edge_id_result = catalog_.create_edge_type(def);
    if (!edge_id_result.has_value()) {
        return tl::unexpected(edge_id_result.error());
    }

    // Create the backing EdgeTable.
    EdgeTableConfig config;
    config.edge_id = *edge_id_result;
    config.name = name;
    config.source_table_id = source_table_id;
    config.target_table_id = target_table_id;
    config.source_pk_type = source_pk_type;
    config.target_pk_type = target_pk_type;
    config.property_columns = property_columns;
    config.prevent_duplicates = prevent_duplicates;

    edge_tables_.emplace(name, std::make_unique<EdgeTable>(std::move(config)));

    GIODB_LOG_INFO("created edge type '{}' (id={})", name, *edge_id_result);
    return ok(*edge_id_result);
}

Result<void> GraphEngine::drop_edge_type(const std::string& name) {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(name);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "edge type '" + name + "' not found in graph engine");
    }

    // Remove from catalog.
    auto drop_result = catalog_.drop_edge_type(name);
    if (!drop_result.has_value()) {
        return tl::unexpected(drop_result.error());
    }

    // Remove the backing EdgeTable.
    edge_tables_.erase(it);

    GIODB_LOG_INFO("dropped edge type '{}'", name);
    return ok();
}

Result<uint64_t> GraphEngine::link(const std::string& edge_type,
                                   const Value& source_pk,
                                   const Value& target_pk,
                                   const std::vector<Value>& properties) {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(edge_type);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    auto result = it->second->insert_edge(source_pk, target_pk, properties);
    if (!result.has_value()) {
        return tl::unexpected(result.error());
    }

    GIODB_LOG_DEBUG("LINK via '{}': edge_row_id={}", edge_type, *result);
    return ok(*result);
}

Result<void>
GraphEngine::unlink(const std::string& edge_type, const Value& source_pk, const Value& target_pk) {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(edge_type);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    // Find the edge by source/target.
    auto found = it->second->find_edge(source_pk, target_pk);
    if (!found.has_value()) {
        return tl::unexpected(found.error());
    }
    if (!found->has_value()) {
        return make_error(StatusCode::NOT_FOUND, "edge not found for UNLINK");
    }

    auto del = it->second->delete_edge((*found)->edge_row_id);
    if (!del.has_value()) {
        return tl::unexpected(del.error());
    }

    GIODB_LOG_DEBUG("UNLINK via '{}': removed edge_row_id={}", edge_type, (*found)->edge_row_id);
    return ok();
}

Result<uint64_t> GraphEngine::unlink_where(const std::string& edge_type,
                                           const Value& source_pk,
                                           const Value& target_pk,
                                           std::function<bool(const EdgeRow&)> predicate) {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(edge_type);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    auto& table = *it->second;

    // Find all edges from source to target.
    auto edges = table.get_edges_from(source_pk);
    if (!edges.has_value()) {
        return tl::unexpected(edges.error());
    }

    // Collect matching edge row IDs first, then delete in a second pass.
    // This avoids partial deletion: we validate all candidates before mutating.
    std::vector<uint64_t> to_delete;
    for (const auto& edge : *edges) {
        KeyType lhs = {edge.target_pk};
        KeyType rhs = {target_pk};
        auto cmp = compare_keys(lhs, rhs);
        if (!cmp.has_value() || *cmp != std::strong_ordering::equal) {
            continue;
        }
        if (!predicate(edge)) {
            continue;
        }
        to_delete.push_back(edge.edge_row_id);
    }

    // Delete all matching edges.
    for (uint64_t row_id : to_delete) {
        auto del = table.delete_edge(row_id);
        if (!del.has_value()) {
            return tl::unexpected(del.error());
        }
    }

    GIODB_LOG_DEBUG("UNLINK WHERE via '{}': removed {} edges", edge_type, to_delete.size());
    return ok(static_cast<uint64_t>(to_delete.size()));
}

Result<std::vector<EdgeRow>> GraphEngine::get_edges_from(const std::string& edge_type,
                                                         const Value& source_pk) const {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(edge_type);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    return it->second->get_edges_from(source_pk);
}

Result<std::vector<EdgeRow>> GraphEngine::get_edges_to(const std::string& edge_type,
                                                       const Value& target_pk) const {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(edge_type);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + edge_type + "' not found");
    }

    return it->second->get_edges_to(target_pk);
}

Result<EdgeTable*> GraphEngine::get_edge_table(const std::string& name) {
    std::lock_guard lock(mu_);

    auto it = edge_tables_.find(name);
    if (it == edge_tables_.end()) {
        return make_error(StatusCode::NOT_FOUND, "edge type '" + name + "' not found");
    }
    return ok(it->second.get());
}

std::vector<std::string> GraphEngine::list_edge_types() const {
    std::lock_guard lock(mu_);

    std::vector<std::string> names;
    names.reserve(edge_tables_.size());
    for (const auto& [name, _] : edge_tables_) {
        names.push_back(name);
    }
    return names;
}

} // namespace giodb
