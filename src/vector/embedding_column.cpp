#include "giodb/vector/embedding_column.h"

#include "giodb/common/logging.h"

#include <algorithm>
#include <sstream>

namespace giodb {

EmbeddingColumnManager::EmbeddingColumnManager(Catalog& catalog) : catalog_(catalog) {}

Result<void> EmbeddingColumnManager::register_table_embeddings(
    table_id_t table_id, const std::vector<EmbeddingColumnDef>& embedding_defs) {
    // Look up the table schema to validate and get column names for index naming.
    auto table_result = catalog_.get_table_by_id(table_id);
    if (!table_result.has_value()) {
        return make_error(StatusCode::NOT_FOUND, "table not found for embedding registration");
    }
    const auto& table_schema = *table_result;

    for (const auto& def : embedding_defs) {
        if (def.dimension <= 0) {
            return make_error(StatusCode::INVALID_ARGUMENT, "EMBEDDING dimension must be positive");
        }

        if (def.source_expr.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "EMBEDDING source expression must not be empty");
        }

        if (def.provider.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "EMBEDDING provider must not be empty");
        }

        // Register the embedding column metadata in the catalog.
        EmbeddingColumnDef catalog_def = def;
        catalog_def.table_id = table_id;
        auto reg = catalog_.register_embedding_column(catalog_def);
        if (!reg.has_value()) {
            return make_error(reg.error().code, reg.error().message);
        }

        // Resolve the column name for index naming.
        std::string column_name = "col" + std::to_string(def.column_id);
        for (const auto& col : table_schema.columns) {
            if (col.ordinal == def.column_id) {
                column_name = col.name;
                break;
            }
        }

        // Auto-create a companion HNSW index.
        IndexDef index_def;
        index_def.table_id = table_id;
        index_def.name = make_index_name(table_schema.name, column_name);
        index_def.index_type = "hnsw";
        index_def.columns = column_name;
        index_def.is_unique = false;

        auto idx = catalog_.create_index(index_def);
        if (!idx.has_value()) {
            return make_error(idx.error().code,
                              "failed to create HNSW index: " + idx.error().message);
        }

        GIODB_LOG_INFO("registered EMBEDDING column: table={}, column={}, dim={}, provider={}, "
                       "index={}",
                       table_schema.name,
                       column_name,
                       def.dimension,
                       def.provider,
                       index_def.name);
    }

    return ok();
}

Result<std::vector<EmbeddingJob>> EmbeddingColumnManager::create_insert_jobs(
    table_id_t table_id,
    int64_t row_id,
    const std::vector<std::pair<int32_t, std::string>>& source_texts) {
    auto emb_cols = catalog_.list_embedding_columns(table_id);
    if (emb_cols.empty()) {
        return ok(std::vector<EmbeddingJob>{});
    }

    std::vector<EmbeddingJob> jobs;
    jobs.reserve(emb_cols.size());

    for (const auto& emb : emb_cols) {
        EmbeddingJob job;
        job.table_id = table_id;
        job.row_id = row_id;
        job.column_id = emb.column_id;
        job.provider = emb.provider;
        job.dimension = emb.dimension;
        job.type = EmbeddingJob::Type::INSERT;
        job.retry_count = 0;

        // Find the source text for this embedding column.
        for (const auto& [col_id, text] : source_texts) {
            if (col_id == emb.column_id) {
                job.source_text = text;
                break;
            }
        }

        jobs.push_back(std::move(job));
    }

    return ok(std::move(jobs));
}

Result<std::vector<EmbeddingJob>> EmbeddingColumnManager::create_update_jobs(
    table_id_t table_id,
    int64_t row_id,
    const std::vector<std::string>& changed_columns,
    const std::vector<std::pair<int32_t, std::string>>& source_texts) {
    auto emb_cols = catalog_.list_embedding_columns(table_id);
    if (emb_cols.empty()) {
        return ok(std::vector<EmbeddingJob>{});
    }

    std::vector<EmbeddingJob> jobs;

    for (const auto& emb : emb_cols) {
        // Only re-embed if a source column was changed.
        if (!source_expr_references(emb.source_expr, changed_columns)) {
            continue;
        }

        EmbeddingJob job;
        job.table_id = table_id;
        job.row_id = row_id;
        job.column_id = emb.column_id;
        job.provider = emb.provider;
        job.dimension = emb.dimension;
        job.type = EmbeddingJob::Type::UPDATE;
        job.retry_count = 0;

        // Find the source text for this embedding column.
        for (const auto& [col_id, text] : source_texts) {
            if (col_id == emb.column_id) {
                job.source_text = text;
                break;
            }
        }

        jobs.push_back(std::move(job));
    }

    return ok(std::move(jobs));
}

std::vector<EmbeddingColumnInfo>
EmbeddingColumnManager::describe_embedding_columns(table_id_t table_id) const {
    auto emb_cols = catalog_.list_embedding_columns(table_id);
    if (emb_cols.empty()) {
        return {};
    }

    // Look up table schema for column names.
    auto table_result = catalog_.get_table_by_id(table_id);
    std::string table_name;
    std::vector<CatalogColumnDef> columns;
    if (table_result.has_value()) {
        table_name = table_result->name;
        columns = table_result->columns;
    }

    std::vector<EmbeddingColumnInfo> infos;
    infos.reserve(emb_cols.size());

    for (const auto& emb : emb_cols) {
        EmbeddingColumnInfo info;
        info.dimension = emb.dimension;
        info.source_expr = emb.source_expr;
        info.provider = emb.provider;

        // Resolve column name.
        info.column_name = "col" + std::to_string(emb.column_id);
        for (const auto& col : columns) {
            if (col.ordinal == emb.column_id) {
                info.column_name = col.name;
                break;
            }
        }

        info.index_name = make_index_name(table_name, info.column_name);

        infos.push_back(std::move(info));
    }

    return infos;
}

std::string EmbeddingColumnManager::make_index_name(const std::string& table_name,
                                                    const std::string& column_name) {
    return "hnsw_" + table_name + "_" + column_name;
}

bool EmbeddingColumnManager::source_expr_references(
    const std::string& source_expr, const std::vector<std::string>& changed_columns) {
    // source_expr is a comma-separated list of column names (e.g., "name" or "name,active").
    // Check if any changed column appears in the source expression.
    for (const auto& col : changed_columns) {
        // Parse the source_expr by comma to match exact column names.
        std::istringstream stream(source_expr);
        std::string token;
        while (std::getline(stream, token, ',')) {
            // Trim whitespace.
            auto start = token.find_first_not_of(' ');
            auto end = token.find_last_not_of(' ');
            if (start != std::string::npos) {
                token = token.substr(start, end - start + 1);
            }
            if (token == col) {
                return true;
            }
        }
    }
    return false;
}

} // namespace giodb
