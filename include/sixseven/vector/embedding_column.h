#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sixseven {

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

    /// Parse a comma-separated source_expr into a list of trimmed column names.
    ///
    /// "name,active" -> ["name", "active"]
    /// " name , active " -> ["name", "active"]
    /// "name" -> ["name"]
    /// "" -> []
    ///
    /// @param source_expr The EMBEDDING source_expr field value.
    /// @return Ordered list of trimmed column names (empty tokens omitted).
    [[nodiscard]] static std::vector<std::string>
    parse_source_columns(const std::string& source_expr);

    /// Result of build_source_text.
    struct SourceTextResult {
        /// Space-joined text from all resolved columns (NULLs/non-STRINGs skipped).
        std::string text;
        /// How many parsed column names actually resolved to a matching schema column.
        /// Zero means the source_expr is misconfigured (no column name matched any
        /// schema column), distinguishable from legitimate all-NULL rows where
        /// resolved_count > 0 but text is empty.
        size_t resolved_count = 0;
    };

    /// Build source text by concatenating values for each parsed column name.
    ///
    /// For each column name from parse_source_columns(source_expr):
    ///   - Finds the column index in schema_columns by name.
    ///   - If the corresponding value is a non-null STRING, appends it.
    ///   - NULL, non-STRING, or empty values are skipped (resolved_count still
    ///     increments because the column was found in the schema).
    /// Parts are joined with a single space separator.
    ///
    /// @param source_expr    The EMBEDDING source_expr field value.
    /// @param schema_columns Schema column list (name + ordinal).
    /// @param values         Row values in schema-column order.
    /// @return SourceTextResult with the joined text and how many source columns resolved.
    [[nodiscard]] static SourceTextResult
    build_source_text(const std::string& source_expr,
                      const std::vector<CatalogColumnDef>& schema_columns,
                      const std::vector<Value>& values);

private:
    /// Check whether a source expression references any of the changed columns.
    [[nodiscard]] static bool
    source_expr_references(const std::string& source_expr,
                           const std::vector<std::string>& changed_columns);

    Catalog& catalog_;
};

} // namespace sixseven
