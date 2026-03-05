#pragma once

#include "sixseven/common/secure_string.h"
#include "sixseven/common/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sixseven {

/// Unique identifier for databases within the catalog.
using database_id_t = int32_t;

/// Unique identifier for tables within the catalog.
using table_id_t = int32_t;

/// Unique identifier for indexes within the catalog.
using index_id_t = int32_t;

/// Unique identifier for edge types within the catalog.
using edge_id_t = int32_t;

/// Default database ID for the built-in 'sixseven' database.
inline constexpr database_id_t default_database_id = 1;

/// System database ID for the built-in 'sixseven_system' database.
inline constexpr database_id_t system_database_id = 2;

/// System database name.
inline constexpr const char* system_database_name = "sixseven_system";

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
    bool is_autoincrement = false;
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

/// Persistent provider configuration stored in the sys_providers table.
///
/// This is the persistent backend for the EMBEDDING type's provider system.
/// Providers are registered via SQL (INSERT INTO sixseven_system.sys_providers)
/// and cached in memory for fast lookups.
struct ProviderConfig {
    int32_t provider_id = 0;
    std::string name;        ///< Unique provider name, e.g., "openai-prod"
    std::string type;        ///< Provider type: "openai", "ollama", "onnx"
    std::string endpoint;    ///< API endpoint URL
    std::string model;       ///< Model identifier
    SecureString api_key;    ///< Decrypted API key (in-memory only, zeroed on deallocation)
    bool is_default = false; ///< At most one provider can be default
};

/// Reserved table IDs for system catalog tables.
/// These must be stable across restarts so files can be located.
inline constexpr table_id_t sys_settings_table_id = 1;
inline constexpr table_id_t sys_providers_table_id = 2;
inline constexpr table_id_t sys_tables_table_id = 3;
inline constexpr table_id_t sys_columns_table_id = 4;
inline constexpr table_id_t sys_indexes_table_id = 5;
inline constexpr table_id_t sys_edge_types_table_id = 6;
inline constexpr table_id_t sys_embedding_columns_table_id = 7;
inline constexpr table_id_t sys_embedding_jobs_table_id = 8;

/// First table ID available for user tables (after all system tables).
inline constexpr table_id_t first_user_table_id = 9;

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
    schema.table_id = sys_settings_table_id;
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

/// Returns the system table schema for sys_providers.
inline TableSchema sys_providers_schema() {
    TableSchema schema;
    schema.table_id = sys_providers_table_id;
    schema.name = "sys_providers";
    schema.columns = {
        {0, "provider_id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, false, ""},
        {2, "type", TypeId::STRING, false, ""},
        {3, "endpoint", TypeId::STRING, false, ""},
        {4, "model", TypeId::STRING, false, ""},
        {5, "api_key_encrypted", TypeId::STRING, true, ""},
        {6, "is_default", TypeId::BOOL, true, ""},
        {7, "created_at", TypeId::TIMESTAMP, true, ""},
    };
    schema.pk_columns = "provider_id";
    return schema;
}

/// Returns the system table schema for sys_replication_slots.
inline TableSchema sys_replication_slots_schema() {
    TableSchema schema;
    schema.table_id = 0; // System tables use reserved IDs.
    schema.name = "sys_replication_slots";
    schema.columns = {
        {0, "slot_name", TypeId::STRING, false, ""},
        {1, "slot_type", TypeId::STRING, false, ""},
        {2, "active", TypeId::BOOL, false, ""},
        {3, "restart_lsn", TypeId::INT64, true, ""},
        {4, "confirmed_flush_lsn", TypeId::INT64, true, ""},
        {5, "created_at", TypeId::TIMESTAMP, true, ""},
    };
    schema.pk_columns = "slot_name";
    return schema;
}

/// Returns the system table schema for sys_tables.
/// Stores metadata for every user table in the catalog.
inline TableSchema sys_tables_schema() {
    TableSchema schema;
    schema.table_id = sys_tables_table_id;
    schema.name = "sys_tables";
    schema.columns = {
        {0, "table_id", TypeId::INT32, false, ""},
        {1, "database_id", TypeId::INT32, false, ""},
        {2, "name", TypeId::STRING, false, ""},
        {3, "pk_columns", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "table_id";
    return schema;
}

/// Returns the system table schema for sys_columns.
/// Stores column definitions for every user table.
inline TableSchema sys_columns_schema() {
    TableSchema schema;
    schema.table_id = sys_columns_table_id;
    schema.name = "sys_columns";
    schema.columns = {
        {0, "table_id", TypeId::INT32, false, ""},
        {1, "ordinal", TypeId::INT32, false, ""},
        {2, "name", TypeId::STRING, false, ""},
        {3, "type_id", TypeId::INT32, false, ""},
        {4, "nullable", TypeId::BOOL, false, ""},
        {5, "default_expr", TypeId::STRING, true, ""},
        {6, "is_autoincrement", TypeId::BOOL, false, ""},
    };
    schema.pk_columns = "table_id,ordinal";
    return schema;
}

/// Returns the system table schema for sys_indexes.
/// Stores index definitions.
inline TableSchema sys_indexes_schema() {
    TableSchema schema;
    schema.table_id = sys_indexes_table_id;
    schema.name = "sys_indexes";
    schema.columns = {
        {0, "index_id", TypeId::INT32, false, ""},
        {1, "table_id", TypeId::INT32, false, ""},
        {2, "name", TypeId::STRING, false, ""},
        {3, "index_type", TypeId::STRING, false, ""},
        {4, "columns", TypeId::STRING, false, ""},
        {5, "is_unique", TypeId::BOOL, false, ""},
    };
    schema.pk_columns = "index_id";
    return schema;
}

/// Returns the system table schema for sys_edge_types.
/// Stores graph edge type definitions.
inline TableSchema sys_edge_types_schema() {
    TableSchema schema;
    schema.table_id = sys_edge_types_table_id;
    schema.name = "sys_edge_types";
    schema.columns = {
        {0, "edge_id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, false, ""},
        {2, "source_table_id", TypeId::INT32, false, ""},
        {3, "target_table_id", TypeId::INT32, false, ""},
        {4, "properties", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "edge_id";
    return schema;
}

/// Returns the system table schema for sys_embedding_columns.
/// Stores EMBEDDING column configurations.
inline TableSchema sys_embedding_columns_schema() {
    TableSchema schema;
    schema.table_id = sys_embedding_columns_table_id;
    schema.name = "sys_embedding_columns";
    schema.columns = {
        {0, "table_id", TypeId::INT32, false, ""},
        {1, "column_id", TypeId::INT32, false, ""},
        {2, "dimension", TypeId::INT32, false, ""},
        {3, "source_expr", TypeId::STRING, true, ""},
        {4, "provider", TypeId::STRING, false, ""},
    };
    schema.pk_columns = "table_id,column_id";
    return schema;
}

/// Returns the system table schema for sys_embedding_jobs.
/// Persists pending embedding generation jobs so they survive server restarts.
inline TableSchema sys_embedding_jobs_schema() {
    TableSchema schema;
    schema.table_id = sys_embedding_jobs_table_id;
    schema.name = "sys_embedding_jobs";
    schema.columns = {
        {0, "table_id", TypeId::INT32, false, ""},
        {1, "row_id", TypeId::INT64, false, ""},
        {2, "column_id", TypeId::INT32, false, ""},
        {3, "source_text", TypeId::STRING, false, ""},
        {4, "provider", TypeId::STRING, false, ""},
        {5, "dimension", TypeId::INT32, false, ""},
        {6, "job_type", TypeId::INT32, false, ""},
        {7, "retry_count", TypeId::INT32, false, ""},
    };
    schema.pk_columns = "table_id,row_id,column_id";
    return schema;
}

} // namespace sixseven
