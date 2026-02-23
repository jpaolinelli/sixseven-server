#pragma once

#include "giodb/common/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace giodb {

/// Unique identifier for databases within the catalog.
using database_id_t = int32_t;

/// Unique identifier for tables within the catalog.
using table_id_t = int32_t;

/// Unique identifier for indexes within the catalog.
using index_id_t = int32_t;

/// Unique identifier for edge types within the catalog.
using edge_id_t = int32_t;

/// Default database ID for the built-in 'giodb' database.
inline constexpr database_id_t default_database_id = 1;

/// System database ID for the built-in 'giodb_system' database.
inline constexpr database_id_t system_database_id = 2;

/// System database name.
inline constexpr const char* system_database_name = "giodb_system";

/// Metadata for a database in the system catalog.
struct Database {
    database_id_t database_id = 0;
    std::string name;
};

/// Describes a column in a catalog table schema.
/// Richer than the tuple-level ColumnDef (which only has name + type).
struct CatalogColumnDef {
    int32_t ordinal = 0;
    std::string name;
    TypeId type_id = TypeId::INT32;
    bool nullable = true;
    std::string default_expr;
};

/// Full metadata for a table in the system catalog.
struct TableSchema {
    table_id_t table_id = 0;
    std::string name;
    std::vector<CatalogColumnDef> columns;
    std::string pk_columns;
};

/// Metadata for an index in the system catalog.
struct IndexDef {
    index_id_t index_id = 0;
    table_id_t table_id = 0;
    std::string name;
    std::string index_type; // e.g., "btree", "hash"
    std::string columns;    // comma-separated column names
    bool is_unique = false;
};

/// Metadata for a graph edge type in the system catalog.
struct EdgeTypeDef {
    edge_id_t edge_id = 0;
    std::string name;
    table_id_t source_table_id = 0;
    table_id_t target_table_id = 0;
    std::string properties; // JSON or comma-separated property definitions
};

/// Metadata for an EMBEDDING column configuration.
struct EmbeddingColumnDef {
    table_id_t table_id = 0;
    int32_t column_id = 0;
    int32_t dimension = 0;
    std::string source_expr;
    std::string provider;
};

/// Configuration for an embedding provider stored in the system catalog.
///
/// Providers are identified by name (e.g., "ollama/all-minilm") and
/// configured with a type, endpoint, model, and optional API key.
struct EmbeddingProviderConfig {
    std::string name;      ///< Unique provider name, e.g., "ollama/all-minilm"
    std::string type;      ///< Provider type: "ollama", "openai", "builtin"
    std::string base_url;  ///< API base URL, e.g., "http://localhost:11434"
    std::string model;     ///< Model identifier, e.g., "all-minilm"
    std::string api_key;   ///< API key (OpenAI only)
    int32_t dimension = 0; ///< Expected embedding dimension
};

/// Returns the system table schema for sys_databases(database_id INT32, name STRING).
inline TableSchema sys_databases_schema() {
    TableSchema schema;
    schema.table_id = 0; // System tables use reserved IDs.
    schema.name = "sys_databases";
    schema.columns = {
        {0, "database_id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, false, ""},
    };
    schema.pk_columns = "database_id";
    return schema;
}

/// Returns the system table schema for sys_settings.
inline TableSchema sys_settings_schema() {
    TableSchema schema;
    schema.table_id = 0; // System tables use reserved IDs.
    schema.name = "sys_settings";
    schema.columns = {
        {0, "key", TypeId::STRING, false, ""},
        {1, "value", TypeId::STRING, true, ""},
        {2, "category", TypeId::STRING, true, ""},
        {3, "description", TypeId::STRING, true, ""},
        {4, "is_runtime_mutable", TypeId::BOOL, true, ""},
    };
    schema.pk_columns = "key";
    return schema;
}

} // namespace giodb
