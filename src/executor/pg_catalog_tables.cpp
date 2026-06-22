#include "sixseven/executor/pg_catalog_tables.h"

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"

#include <array>
#include <sstream>
#include <string>
#include <unordered_map>

namespace sixseven {

namespace {

constexpr std::array<TypeId, 23> all_types = {
    TypeId::BOOL,    TypeId::INT8,      TypeId::INT16,    TypeId::INT32,  TypeId::INT64,
    TypeId::UINT8,   TypeId::UINT16,    TypeId::UINT32,   TypeId::UINT64, TypeId::FLOAT32,
    TypeId::FLOAT64, TypeId::DECIMAL,   TypeId::STRING,   TypeId::BLOB,   TypeId::DATE,
    TypeId::TIME,    TypeId::TIMESTAMP, TypeId::INTERVAL, TypeId::POINT,  TypeId::JSON,
    TypeId::UUID,    TypeId::EMBEDDING, TypeId::PATH,
};

std::string pg_typname(TypeId t) {
    switch (t) {
    case TypeId::BOOL:
        return "bool";
    case TypeId::INT8:
        return "int2";
    case TypeId::INT16:
        return "int2";
    case TypeId::INT32:
        return "int4";
    case TypeId::INT64:
        return "int8";
    case TypeId::UINT8:
        return "int2";
    case TypeId::UINT16:
        return "int4";
    case TypeId::UINT32:
        return "int8";
    case TypeId::UINT64:
        return "numeric";
    case TypeId::FLOAT32:
        return "float4";
    case TypeId::FLOAT64:
        return "float8";
    case TypeId::DECIMAL:
        return "numeric";
    case TypeId::STRING:
        return "text";
    case TypeId::BLOB:
        return "bytea";
    case TypeId::DATE:
        return "date";
    case TypeId::TIME:
        return "time";
    case TypeId::TIMESTAMP:
        return "timestamp";
    case TypeId::INTERVAL:
        return "interval";
    case TypeId::POINT:
        return "point";
    case TypeId::JSON:
        return "json";
    case TypeId::UUID:
        return "uuid";
    case TypeId::EMBEDDING:
        return "embedding";
    case TypeId::PATH:
        return "text";
    }
    return "text";
}

int32_t pg_typlen(TypeId t) {
    switch (t) {
    case TypeId::BOOL:
        return 1;
    case TypeId::INT8:
        return 2;
    case TypeId::INT16:
        return 2;
    case TypeId::INT32:
        return 4;
    case TypeId::INT64:
        return 8;
    case TypeId::UINT8:
        return 2;
    case TypeId::UINT16:
        return 4;
    case TypeId::UINT32:
        return 8;
    case TypeId::UINT64:
        return -1;
    case TypeId::FLOAT32:
        return 4;
    case TypeId::FLOAT64:
        return 8;
    case TypeId::DECIMAL:
        return -1;
    case TypeId::STRING:
        return -1;
    case TypeId::BLOB:
        return -1;
    case TypeId::DATE:
        return 4;
    case TypeId::TIME:
        return 8;
    case TypeId::TIMESTAMP:
        return 8;
    case TypeId::INTERVAL:
        return 16;
    case TypeId::POINT:
        return 16;
    case TypeId::JSON:
        return -1;
    case TypeId::UUID:
        return 16;
    case TypeId::EMBEDDING:
        return -1;
    case TypeId::PATH:
        return -1;
    }
    return -1;
}

uint32_t pg_oid(TypeId t) {
    switch (t) {
    case TypeId::BOOL:
        return 16;
    case TypeId::INT8:
        return 21;
    case TypeId::INT16:
        return 21;
    case TypeId::INT32:
        return 23;
    case TypeId::INT64:
        return 20;
    case TypeId::UINT8:
        return 21;
    case TypeId::UINT16:
        return 23;
    case TypeId::UINT32:
        return 20;
    case TypeId::UINT64:
        return 1700;
    case TypeId::FLOAT32:
        return 700;
    case TypeId::FLOAT64:
        return 701;
    case TypeId::DECIMAL:
        return 1700;
    case TypeId::STRING:
        return 25;
    case TypeId::BLOB:
        return 17;
    case TypeId::DATE:
        return 1082;
    case TypeId::TIME:
        return 1083;
    case TypeId::TIMESTAMP:
        return 1114;
    case TypeId::INTERVAL:
        return 1186;
    case TypeId::POINT:
        return 600;
    case TypeId::JSON:
        return 114;
    case TypeId::UUID:
        return 2950;
    case TypeId::EMBEDDING:
        return 100000;
    case TypeId::PATH:
        return 25;
    }
    return 25;
}

} // namespace

VirtualTableDef make_pg_database(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_database";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "datname", TypeId::STRING, false, ""},
        {2, "datdba", TypeId::INT32, false, ""},
        {3, "encoding", TypeId::INT32, false, ""},
    };
    def.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& db : catalog.list_databases()) {
            rows.push_back({std::to_string(db.database_id), db.name, "10", "6"});
        }
        return rows;
    };
    return def;
}

VirtualTableDef make_pg_namespace() {
    VirtualTableDef def;
    def.name = "pg_namespace";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "nspname", TypeId::STRING, false, ""},
        {2, "nspowner", TypeId::INT32, false, ""},
    };
    def.generator = []() -> std::vector<std::vector<std::string>> {
        return {
            {"11", "pg_catalog", "10"},
            {"2200", "public", "10"},
        };
    };
    return def;
}

VirtualTableDef make_pg_type() {
    VirtualTableDef def;
    def.name = "pg_type";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "typname", TypeId::STRING, false, ""},
        {2, "typnamespace", TypeId::INT32, false, ""},
        {3, "typlen", TypeId::INT32, false, ""},
        {4, "typtype", TypeId::STRING, false, ""},
        {5, "typelem", TypeId::INT32, false, ""},
        {6, "typrelid", TypeId::INT32, false, ""},
        {7, "typbasetype", TypeId::INT32, false, ""},
    };
    def.generator = []() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(all_types.size());
        for (auto t : all_types) {
            rows.push_back({
                std::to_string(pg_oid(t)),
                pg_typname(t),
                "11",
                std::to_string(pg_typlen(t)),
                "b",
                "0",
                "0",
                "0",
            });
        }
        return rows;
    };
    return def;
}

VirtualTableDef make_pg_class(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_class";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "relname", TypeId::STRING, false, ""},
        {2, "relnamespace", TypeId::INT32, false, ""},
        {3, "relkind", TypeId::STRING, false, ""},
        {4, "reltuples", TypeId::FLOAT64, false, ""},
        {5, "relhasindex", TypeId::BOOL, false, ""},
        {6, "relnatts", TypeId::INT32, false, ""},
    };
    def.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& db : catalog.list_databases()) {
            for (const auto& table : catalog.list_tables(db.database_id)) {
                if (table.table_id < first_user_table_id) {
                    continue;
                }
                bool has_index = !catalog.list_indexes(table.table_id).empty();
                rows.push_back({
                    std::to_string(table.table_id),
                    table.name,
                    "2200",
                    "r",
                    "-1",
                    has_index ? "true" : "false",
                    std::to_string(static_cast<int32_t>(table.columns.size())),
                });
            }
        }
        return rows;
    };
    return def;
}

VirtualTableDef make_pg_attribute(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_attribute";
    def.columns = {
        {0, "attrelid", TypeId::INT32, false, ""},
        {1, "attname", TypeId::STRING, false, ""},
        {2, "atttypid", TypeId::INT32, false, ""},
        {3, "attlen", TypeId::INT32, false, ""},
        {4, "attnum", TypeId::INT32, false, ""},
        {5, "attnotnull", TypeId::BOOL, false, ""},
        {6, "attisdropped", TypeId::BOOL, false, ""},
    };
    def.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& db : catalog.list_databases()) {
            for (const auto& table : catalog.list_tables(db.database_id)) {
                if (table.table_id < first_user_table_id) {
                    continue;
                }
                for (const auto& col : table.columns) {
                    rows.push_back({
                        std::to_string(table.table_id),
                        col.name,
                        std::to_string(pg_oid(col.type_id)),
                        std::to_string(pg_typlen(col.type_id)),
                        std::to_string(col.ordinal + 1),
                        col.nullable ? "false" : "true",
                        "false",
                    });
                }
            }
        }
        return rows;
    };
    return def;
}

VirtualTableDef make_pg_index(Catalog& catalog) {
    VirtualTableDef def;
    def.name = "pg_index";
    def.columns = {
        {0, "indexrelid", TypeId::INT32, false, ""},
        {1, "indrelid", TypeId::INT32, false, ""},
        {2, "indnatts", TypeId::INT16, false, ""},
        {3, "indisunique", TypeId::BOOL, false, ""},
        {4, "indisprimary", TypeId::BOOL, false, ""},
        {5, "indkey", TypeId::STRING, false, ""},
    };
    def.generator = [&catalog]() -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> rows;
        for (const auto& idx : catalog.list_all_indexes()) {
            auto table_r = catalog.get_table_by_id(idx.table_id);
            if (!table_r.has_value()) {
                continue;
            }
            const auto& table = table_r.value();

            std::unordered_map<std::string, int> col_pos;
            for (const auto& col : table.columns) {
                col_pos[col.name] = col.ordinal + 1;
            }

            std::string indkey;
            int natts = 0;
            std::istringstream stream(idx.columns);
            std::string col_name;
            while (std::getline(stream, col_name, ',')) {
                if (!indkey.empty()) {
                    indkey += ' ';
                }
                auto it = col_pos.find(col_name);
                indkey += (it != col_pos.end()) ? std::to_string(it->second) : "0";
                ++natts;
            }

            bool is_primary = (idx.columns == table.pk_columns);

            rows.push_back({
                std::to_string(idx.index_id),
                std::to_string(idx.table_id),
                std::to_string(natts),
                idx.is_unique ? "true" : "false",
                is_primary ? "true" : "false",
                indkey,
            });
        }
        return rows;
    };
    return def;
}

void register_pg_catalog_tables(Catalog& catalog) {
    catalog.register_virtual_table(make_pg_database(catalog));
    catalog.register_virtual_table(make_pg_namespace());
    catalog.register_virtual_table(make_pg_type());
    catalog.register_virtual_table(make_pg_class(catalog));
    catalog.register_virtual_table(make_pg_attribute(catalog));
    catalog.register_virtual_table(make_pg_index(catalog));
}

} // namespace sixseven
