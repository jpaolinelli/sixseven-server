#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace giodb {

/// Describes an async embedding generation request.
struct EmbeddingJob {
    /// Type of operation that triggered the embedding job.
    enum class Type : uint8_t { INSERT, UPDATE };

    table_id_t table_id = 0;
    int64_t row_id = 0;
    int32_t column_id = 0;
    std::string source_text;
    std::string provider;
    int32_t dimension = 0;
    int32_t retry_count = 0;
    Type type = Type::INSERT;
};

/// Enriched metadata for an EMBEDDING column (used by SHOW COLUMNS / DESCRIBE).
struct EmbeddingColumnInfo {
    std::string column_name;
    int32_t dimension = 0;
    std::string source_expr;
    std::string provider;
    std::string index_name;
};

/// Manages EMBEDDING column lifecycle: catalog registration, companion HNSW
/// index creation, and trigger-based job generation for async embedding.
///
/// Usage:
/// ```
///   EmbeddingColumnManager mgr(catalog);
///   // During CREATE TABLE with EMBEDDING columns:
///   mgr.register_table_embeddings(table_id, embedding_defs);
///   // During INSERT:
///   auto jobs = mgr.create_insert_jobs(table_id, row_id, source_texts);
///   // During UPDATE:
///   auto jobs = mgr.create_update_jobs(table_id, row_id, changed_columns, source_texts);
/// ```
class EmbeddingColumnManager {
public:
    explicit EmbeddingColumnManager(Catalog& catalog);

    /// Register EMBEDDING columns for a table and create companion HNSW indexes.
    ///
    /// For each EmbeddingColumnDef:
    ///   1. Registers the embedding column metadata in the catalog.
    ///   2. Creates a companion HNSW index (type="hnsw") with the column's
    ///      dimension and default parameters (M=16, ef_construction=200).
    ///
    /// @param table_id The table that owns the embedding columns.
    /// @param embedding_defs Embedding column definitions to register.
    /// @return Ok on success, or the first error encountered.
    [[nodiscard]] Result<void>
    register_table_embeddings(table_id_t table_id,
                              const std::vector<EmbeddingColumnDef>& embedding_defs);

    /// Create embedding jobs for an INSERT operation.
    ///
    /// Generates one job per EMBEDDING column on the table.
    ///
    /// @param table_id The table that received the INSERT.
    /// @param row_id The row ID of the inserted row.
    /// @param source_texts Map of column_id to source text extracted from the row.
    /// @return Vector of jobs to enqueue, or error if table has no embedding columns.
    [[nodiscard]] Result<std::vector<EmbeddingJob>>
    create_insert_jobs(table_id_t table_id,
                       int64_t row_id,
                       const std::vector<std::pair<int32_t, std::string>>& source_texts);

    /// Create embedding jobs for an UPDATE operation on source columns.
    ///
    /// Only generates jobs for EMBEDDING columns whose source_expr references
    /// one of the changed columns.
    ///
    /// @param table_id The table that received the UPDATE.
    /// @param row_id The row ID of the updated row.
    /// @param changed_columns Column names that were modified.
    /// @param source_texts Map of column_id to updated source text.
    /// @return Vector of jobs to enqueue (may be empty if no embedding columns affected).
    [[nodiscard]] Result<std::vector<EmbeddingJob>>
    create_update_jobs(table_id_t table_id,
                       int64_t row_id,
                       const std::vector<std::string>& changed_columns,
                       const std::vector<std::pair<int32_t, std::string>>& source_texts);

    /// Get embedding column metadata for display (SHOW COLUMNS / DESCRIBE).
    ///
    /// @param table_id The table to describe.
    /// @return Vector of enriched embedding column info with index names.
    [[nodiscard]] std::vector<EmbeddingColumnInfo>
    describe_embedding_columns(table_id_t table_id) const;

    /// Build the auto-generated HNSW index name for an embedding column.
    ///
    /// @param table_name Table name.
    /// @param column_name Embedding column name.
    /// @return Index name in the format "hnsw_<table>_<column>".
    [[nodiscard]] static std::string make_index_name(const std::string& table_name,
                                                     const std::string& column_name);

private:
    /// Check whether a source expression references any of the changed columns.
    [[nodiscard]] static bool
    source_expr_references(const std::string& source_expr,
                           const std::vector<std::string>& changed_columns);

    Catalog& catalog_;
};

} // namespace giodb
