#include "giodb/executor/query_engine.h"

#include "giodb/executor/planner.h"
#include "giodb/parser/ast.h"
#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"
#include "giodb/planner/binder.h"
#include "giodb/planner/type_resolver.h"

#include <string>

namespace giodb {

QueryEngine::QueryEngine(Catalog& catalog, StorageManager& storage)
    : catalog_(catalog), storage_(storage) {}

// ---------------------------------------------------------------------------
// Full pipeline
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute(const std::string& sql) {
    // 1. Lex.
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return make_error(tokens.error().code, tokens.error().message);
    }

    // 2. Parse.
    Parser parser(std::move(*tokens));
    auto stmt_ptr = parser.parse();
    if (!stmt_ptr) {
        return make_error(stmt_ptr.error().code, stmt_ptr.error().message);
    }

    // 3. Dispatch DDL before binding (CREATE TABLE creates the table
    //    that the binder would try to look up).
    if (auto* create = dynamic_cast<const CreateTableStmt*>(stmt_ptr->get())) {
        return execute_create_table(*create);
    }
    if (auto* drop = dynamic_cast<const DropTableStmt*>(stmt_ptr->get())) {
        return execute_drop_table(*drop);
    }

    // 4. Bind.
    Binder binder(catalog_);
    auto bound = binder.bind(**stmt_ptr);
    if (!bound) {
        return make_error(bound.error().code, bound.error().message);
    }

    // 5. Plan + Execute.
    return execute_plan(*bound);
}

// ---------------------------------------------------------------------------
// DDL: CREATE TABLE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_create_table(const CreateTableStmt& stmt) {
    TableSchema ts;
    ts.name = stmt.name;

    for (int32_t i = 0; i < static_cast<int32_t>(stmt.columns.size()); ++i) {
        const auto& col = stmt.columns[static_cast<size_t>(i)];
        auto type_result = resolve_type_spec(col.type);
        if (!type_result) {
            return make_error(type_result.error().code, type_result.error().message);
        }
        CatalogColumnDef ccd;
        ccd.ordinal = i;
        ccd.name = col.name;
        ccd.type_id = *type_result;
        ccd.nullable = col.nullable;
        ts.columns.push_back(std::move(ccd));
    }

    // Extract primary key from table-level constraints.
    for (const auto& c : stmt.constraints) {
        if (c.kind == TableConstraint::Kind::PRIMARY_KEY) {
            std::string pk;
            for (const auto& name : c.columns) {
                if (!pk.empty()) {
                    pk += ",";
                }
                pk += name;
            }
            ts.pk_columns = pk;
        }
    }

    // Register in catalog.
    auto table_id = catalog_.create_table(default_database_id, std::move(ts));
    if (!table_id) {
        if (stmt.if_not_exists && table_id.error().code == StatusCode::ALREADY_EXISTS) {
            QueryResult qr;
            qr.message = "CREATE TABLE";
            return ok(std::move(qr));
        }
        return make_error(table_id.error().code, table_id.error().message);
    }

    // Retrieve the created schema (now has assigned table_id).
    auto schema = catalog_.get_table(default_database_id, stmt.name);
    if (!schema) {
        return make_error(schema.error().code, schema.error().message);
    }

    // Create physical storage.
    auto storage_result = storage_.create_table_storage(*table_id, *schema);
    if (!storage_result) {
        return make_error(storage_result.error().code, storage_result.error().message);
    }

    QueryResult qr;
    qr.message = "CREATE TABLE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: DROP TABLE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_drop_table(const DropTableStmt& stmt) {
    auto schema = catalog_.get_table(default_database_id, stmt.name);
    if (!schema) {
        if (stmt.if_exists && schema.error().code == StatusCode::NOT_FOUND) {
            QueryResult qr;
            qr.message = "DROP TABLE";
            return ok(std::move(qr));
        }
        return make_error(schema.error().code, schema.error().message);
    }

    auto table_id = schema->table_id;

    // Drop storage first (flush + close + delete file).
    auto drop_storage = storage_.drop_table_storage(table_id);
    if (!drop_storage) {
        return make_error(drop_storage.error().code, drop_storage.error().message);
    }

    // Remove from catalog.
    auto drop_catalog = catalog_.drop_table(default_database_id, stmt.name);
    if (!drop_catalog) {
        return make_error(drop_catalog.error().code, drop_catalog.error().message);
    }

    QueryResult qr;
    qr.message = "DROP TABLE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DML / Query execution via Planner
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_plan(const BoundStatement& bound) {
    // Build iterator tree.
    Planner planner(catalog_, storage_);
    std::vector<ExprPtr> owned_exprs;
    auto iter = planner.plan(bound, owned_exprs);
    if (!iter) {
        return make_error(iter.error().code, iter.error().message);
    }

    // Open.
    auto open_result = (*iter)->open();
    if (!open_result) {
        return make_error(open_result.error().code, open_result.error().message);
    }

    // Build column metadata from the output schema.
    const auto& schema = (*iter)->output_schema();
    QueryResult qr;
    for (size_t i = 0; i < schema.column_count(); ++i) {
        qr.column_names.push_back(schema.column(i).name);
        qr.column_types.push_back(schema.column(i).type_id);
    }

    // Drain.
    while (true) {
        auto row = (*iter)->next();
        if (!row) {
            (*iter)->close();
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        qr.rows.push_back(std::move(row->value().values));
    }

    // Close.
    (*iter)->close();

    // Detect DML results (single "count" column).
    if (qr.column_names.size() == 1 && qr.column_names[0] == "count" && qr.rows.size() == 1) {
        qr.affected_rows = qr.rows[0][0].as_int64();
        qr.rows.clear();
        qr.column_names.clear();
        qr.column_types.clear();
    }

    return ok(std::move(qr));
}

} // namespace giodb
