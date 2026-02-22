#pragma once

#include "giodb/common/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace giodb {

/// Unique identifier for tables within the catalog.
using table_id_t = int32_t;

/// Unique identifier for indexes within the catalog.
using index_id_t = int32_t;

/// Unique identifier for edge types within the catalog.
using edge_id_t = int32_t;

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

} // namespace giodb
