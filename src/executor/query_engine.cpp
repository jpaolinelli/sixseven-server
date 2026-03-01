#include "giodb/executor/query_engine.h"

#include "giodb/catalog/schema.h"
#include "giodb/common/logging.h"
#include "giodb/common/types.h"
#include "giodb/executor/catalog_persistence.h"
#include "giodb/executor/explain.h"
#include "giodb/executor/expr_evaluator.h"
#include "giodb/executor/planner.h"
#include "giodb/executor/provider_cache.h"
#include "giodb/executor/settings_cache.h"
#include "giodb/parser/ast.h"
#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"
#include "giodb/planner/binder.h"
#include "giodb/planner/type_resolver.h"
#include "giodb/server/auth.h"
#include "giodb/server/replication_slot.h"
#include "giodb/server/wal_receiver.h"
#include "giodb/server/wal_sender_manager.h"
#include "giodb/storage/wal.h"
#include "giodb/table/tuple.h"
#include "giodb/vector/embedding_column.h"
#include "giodb/vector/hnsw_index.h"
#include "giodb/vector/provider_registry.h"

#include <chrono>
#include <span>
#include <string>

namespace giodb {

QueryEngine::QueryEngine(Catalog& catalog, StorageManager& storage, GraphEngine* graph_engine)
    : catalog_(catalog), storage_(storage), graph_engine_(graph_engine) {}

void QueryEngine::set_current_database(database_id_t database_id) {
    current_database_id_ = database_id;
}

database_id_t QueryEngine::current_database_id() const {
    return current_database_id_;
}

void QueryEngine::set_provider_registry(ProviderRegistry* registry) {
    provider_registry_ = registry;
}

void QueryEngine::set_hnsw_indexes(std::unordered_map<std::string, HnswIndex*>* indexes) {
    hnsw_indexes_ = indexes;
}

void QueryEngine::set_settings_cache(SettingsCache* cache) {
    settings_cache_ = cache;
}

void QueryEngine::set_provider_cache(ProviderCache* cache) {
    provider_cache_ = cache;
}

void QueryEngine::set_slot_manager(ReplicationSlotManager* slot_mgr) {
    slot_mgr_ = slot_mgr;
}

void QueryEngine::set_wal_sender_manager(WalSenderManager* sender_mgr) {
    sender_mgr_ = sender_mgr;
}

void QueryEngine::set_wal_receiver(WalReceiver* receiver) {
    wal_receiver_ = receiver;
}

void QueryEngine::set_wal_writer(WalWriter* writer) {
    wal_writer_ = writer;
}

void QueryEngine::set_standby_mode(bool enabled) {
    standby_mode_ = enabled;
}

bool QueryEngine::is_standby_mode() const {
    return standby_mode_;
}

void QueryEngine::push_skip_masking() {
    ++skip_masking_depth_;
}

void QueryEngine::pop_skip_masking() {
    if (skip_masking_depth_ > 0) {
        --skip_masking_depth_;
    }
}

void QueryEngine::set_user_manager(UserManager* user_mgr) {
    user_mgr_ = user_mgr;
}

void QueryEngine::set_auth_method(AuthMethod method) {
    auth_method_ = method;
}

void QueryEngine::set_catalog_persistence(CatalogPersistence* persistence) {
    catalog_persistence_ = persistence;
}

// ---------------------------------------------------------------------------
// Full pipeline
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute(const std::string& sql) {
    // Set system function context for replication functions.
    SystemFunctionContext sys_fn_ctx;
    sys_fn_ctx.standby_mode = standby_mode_;
    sys_fn_ctx.wal_writer = wal_writer_;
    sys_fn_ctx.wal_receiver = wal_receiver_;
    set_system_function_context(&sys_fn_ctx);

    // Ensure context is cleared on exit.
    struct ContextGuard {
        ContextGuard() = default;
        ~ContextGuard() { set_system_function_context(nullptr); }
        ContextGuard(const ContextGuard&) = delete;
        ContextGuard& operator=(const ContextGuard&) = delete;
        ContextGuard(ContextGuard&&) = delete;
        ContextGuard& operator=(ContextGuard&&) = delete;
    } ctx_guard;

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

    // 3. Reject writes in standby mode.
    if (standby_mode_) {
        const auto* raw = stmt_ptr->get();
        bool is_write = dynamic_cast<const InsertStmt*>(raw) != nullptr ||
                        dynamic_cast<const UpdateStmt*>(raw) != nullptr ||
                        dynamic_cast<const DeleteStmt*>(raw) != nullptr ||
                        dynamic_cast<const CreateTableStmt*>(raw) != nullptr ||
                        dynamic_cast<const DropTableStmt*>(raw) != nullptr ||
                        dynamic_cast<const CreateDatabaseStmt*>(raw) != nullptr ||
                        dynamic_cast<const DropDatabaseStmt*>(raw) != nullptr ||
                        dynamic_cast<const CreateEdgeTypeStmt*>(raw) != nullptr ||
                        dynamic_cast<const DropEdgeTypeStmt*>(raw) != nullptr ||
                        dynamic_cast<const LinkStmt*>(raw) != nullptr ||
                        dynamic_cast<const UnlinkStmt*>(raw) != nullptr ||
                        dynamic_cast<const ReembedStmt*>(raw) != nullptr ||
                        dynamic_cast<const CreateUserStmt*>(raw) != nullptr ||
                        dynamic_cast<const DropUserStmt*>(raw) != nullptr ||
                        dynamic_cast<const AlterUserStmt*>(raw) != nullptr;

        if (is_write) {
            // Determine whether this is DML or DDL for the error message.
            bool is_ddl = dynamic_cast<const CreateTableStmt*>(raw) != nullptr ||
                          dynamic_cast<const DropTableStmt*>(raw) != nullptr ||
                          dynamic_cast<const CreateDatabaseStmt*>(raw) != nullptr ||
                          dynamic_cast<const DropDatabaseStmt*>(raw) != nullptr ||
                          dynamic_cast<const CreateEdgeTypeStmt*>(raw) != nullptr ||
                          dynamic_cast<const DropEdgeTypeStmt*>(raw) != nullptr ||
                          dynamic_cast<const CreateUserStmt*>(raw) != nullptr ||
                          dynamic_cast<const DropUserStmt*>(raw) != nullptr ||
                          dynamic_cast<const AlterUserStmt*>(raw) != nullptr;

            if (is_ddl) {
                return make_error(StatusCode::READ_ONLY,
                                  "cannot execute DDL on a read-only standby");
            }
            return make_error(StatusCode::READ_ONLY, "cannot execute DML on a read-only standby");
        }

        // Reject SET (global setting modification) in standby mode.
        if (dynamic_cast<const SetStmt*>(raw) != nullptr) {
            return make_error(StatusCode::READ_ONLY,
                              "cannot modify settings on a read-only standby");
        }
    }

    // 4. Dispatch DDL before binding (CREATE TABLE creates the table
    //    that the binder would try to look up).
    if (auto* create_db = dynamic_cast<const CreateDatabaseStmt*>(stmt_ptr->get())) {
        return execute_create_database(*create_db);
    }
    if (auto* drop_db = dynamic_cast<const DropDatabaseStmt*>(stmt_ptr->get())) {
        return execute_drop_database(*drop_db);
    }
    if (auto* create = dynamic_cast<const CreateTableStmt*>(stmt_ptr->get())) {
        return execute_create_table(*create);
    }
    if (auto* drop = dynamic_cast<const DropTableStmt*>(stmt_ptr->get())) {
        return execute_drop_table(*drop);
    }
    if (auto* create_edge = dynamic_cast<const CreateEdgeTypeStmt*>(stmt_ptr->get())) {
        return execute_create_edge_type(*create_edge);
    }
    if (auto* drop_edge = dynamic_cast<const DropEdgeTypeStmt*>(stmt_ptr->get())) {
        return execute_drop_edge_type(*drop_edge);
    }
    if (auto* set = dynamic_cast<const SetStmt*>(stmt_ptr->get())) {
        return execute_set(*set);
    }
    if (auto* show = dynamic_cast<const ShowStmt*>(stmt_ptr->get())) {
        return execute_show(*show);
    }
    if (auto* create_user = dynamic_cast<const CreateUserStmt*>(stmt_ptr->get())) {
        return execute_create_user(*create_user);
    }
    if (auto* drop_user = dynamic_cast<const DropUserStmt*>(stmt_ptr->get())) {
        return execute_drop_user(*drop_user);
    }
    if (auto* alter_user = dynamic_cast<const AlterUserStmt*>(stmt_ptr->get())) {
        return execute_alter_user(*alter_user);
    }

    // 4b. Handle SELECT <system_function()> without FROM clause.
    if (auto* select = dynamic_cast<const SelectStmt*>(stmt_ptr->get())) {
        if (select->from.empty() && !select->items.empty()) {
            // Check if all items are system function calls.
            bool all_system_fns = true;
            for (const auto& item : select->items) {
                auto* fn = dynamic_cast<const FunctionCallExpr*>(item.expr.get());
                if (!fn) {
                    all_system_fns = false;
                    break;
                }
                std::string upper;
                for (char c : fn->name) {
                    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                if (upper != "PG_CURRENT_WAL_LSN" && upper != "PG_IS_IN_RECOVERY" &&
                    upper != "PG_LAST_WAL_REPLAY_LSN") {
                    all_system_fns = false;
                    break;
                }
            }
            if (all_system_fns) {
                QueryResult qr;
                std::vector<Value> row;
                for (const auto& item : select->items) {
                    auto* fn = dynamic_cast<const FunctionCallExpr*>(item.expr.get());
                    std::string upper;
                    for (char c : fn->name) {
                        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    }
                    std::string col_name = item.alias.empty() ? fn->name + "()" : item.alias;
                    qr.column_names.push_back(col_name);

                    if (upper == "PG_CURRENT_WAL_LSN") {
                        qr.column_types.push_back(TypeId::INT64);
                        if (wal_writer_) {
                            row.emplace_back(static_cast<int64_t>(wal_writer_->current_lsn()));
                        } else {
                            row.emplace_back(Value::make_null());
                        }
                    } else if (upper == "PG_IS_IN_RECOVERY") {
                        qr.column_types.push_back(TypeId::BOOL);
                        row.emplace_back(standby_mode_);
                    } else if (upper == "PG_LAST_WAL_REPLAY_LSN") {
                        qr.column_types.push_back(TypeId::INT64);
                        if (wal_receiver_) {
                            auto state = wal_receiver_->get_state();
                            if (state.applied_lsn != invalid_lsn) {
                                row.emplace_back(static_cast<int64_t>(state.applied_lsn));
                            } else {
                                row.emplace_back(Value::make_null());
                            }
                        } else {
                            row.emplace_back(Value::make_null());
                        }
                    }
                }
                qr.rows.push_back(std::move(row));
                return ok(std::move(qr));
            }
        }
    }

    // 5. Bind.
    Binder binder(catalog_, current_database_id_);
    auto bound = binder.bind(**stmt_ptr);
    if (!bound) {
        return make_error(bound.error().code, bound.error().message);
    }

    // 6. Dispatch EXPLAIN after binding.
    if (auto* explain = dynamic_cast<const ExplainStmt*>(bound->stmt)) {
        return execute_explain(*explain, *bound);
    }

    // 6b. Dispatch graph DML after binding.
    if (auto* link = dynamic_cast<const LinkStmt*>(bound->stmt)) {
        return execute_link(*link, *bound);
    }
    if (auto* unlink = dynamic_cast<const UnlinkStmt*>(bound->stmt)) {
        return execute_unlink(*unlink, *bound);
    }

    // 6c. Dispatch admin commands after binding.
    if (auto* reembed = dynamic_cast<const ReembedStmt*>(bound->stmt)) {
        return execute_reembed(*reembed);
    }

    // 7. Plan + Execute.
    return execute_plan(*bound);
}

// ---------------------------------------------------------------------------
// DDL: CREATE DATABASE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_create_database(const CreateDatabaseStmt& stmt) {
    auto db_id = catalog_.create_database(stmt.database_name);
    if (!db_id) {
        if (stmt.if_not_exists && db_id.error().code == StatusCode::ALREADY_EXISTS) {
            QueryResult qr;
            qr.message = "CREATE DATABASE";
            return ok(std::move(qr));
        }
        return make_error(db_id.error().code, db_id.error().message);
    }

    // Create the database directory structure.
    auto storage_result = storage_.create_database_storage(*db_id);
    if (!storage_result) {
        return make_error(storage_result.error().code, storage_result.error().message);
    }

    QueryResult qr;
    qr.message = "CREATE DATABASE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: DROP DATABASE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_drop_database(const DropDatabaseStmt& stmt) {
    auto db = catalog_.get_database(stmt.database_name);
    if (!db) {
        if (stmt.if_exists && db.error().code == StatusCode::NOT_FOUND) {
            QueryResult qr;
            qr.message = "DROP DATABASE";
            return ok(std::move(qr));
        }
        return make_error(db.error().code, db.error().message);
    }

    auto db_id = db->database_id;

    // If cascade, drop storage for every table in this database first.
    if (stmt.cascade) {
        auto tables = catalog_.list_tables(db_id);
        for (const auto& table : tables) {
            auto drop_storage = storage_.drop_table_storage(db_id, table.table_id);
            if (!drop_storage) {
                return make_error(drop_storage.error().code, drop_storage.error().message);
            }
        }
    }

    auto result = catalog_.drop_database(db_id, stmt.cascade);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    // Remove the database directory.
    auto drop_dir = storage_.drop_database_storage(db_id);
    if (!drop_dir) {
        return make_error(drop_dir.error().code, drop_dir.error().message);
    }

    QueryResult qr;
    qr.message = "DROP DATABASE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: CREATE TABLE
// ---------------------------------------------------------------------------

/// Serialize an expression AST node to a SQL string for storage in the catalog.
/// Handles the common default expression forms: literals, function calls,
/// identifiers (e.g. CURRENT_TIMESTAMP), unary/binary ops, and casts.
static std::string expr_to_sql(const Expr& expr) {
    if (auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        switch (lit->kind) {
        case LiteralKind::STRING:
            return "'" + lit->value + "'";
        case LiteralKind::NULL_LITERAL:
            return "NULL";
        default:
            return lit->value;
        }
    }
    if (auto* fn = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        std::string s = fn->name + "(";
        for (size_t i = 0; i < fn->args.size(); ++i) {
            if (i > 0)
                s += ", ";
            s += expr_to_sql(*fn->args[i]);
        }
        s += ")";
        return s;
    }
    if (auto* col_ref = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        // Bare identifiers like CURRENT_TIMESTAMP are parsed as column refs.
        if (!col_ref->table.empty())
            return col_ref->table + "." + col_ref->column;
        return col_ref->column;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        static constexpr std::string_view ops[] = {
            "+", "-", "*", "/", "%", "=", "!=", "<", ">", "<=", ">=", "AND", "OR", "||"};
        auto idx = static_cast<size_t>(bin->op);
        std::string op_str = idx < std::size(ops) ? std::string(ops[idx]) : "?";
        return "(" + expr_to_sql(*bin->lhs) + " " + op_str + " " + expr_to_sql(*bin->rhs) + ")";
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (un->op == UnaryOp::NEGATE)
            return "-" + expr_to_sql(*un->operand);
        return "NOT " + expr_to_sql(*un->operand);
    }
    if (auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        return "CAST(" + expr_to_sql(*cast->expr) + " AS " + cast->target_type.name + ")";
    }
    return "NULL"; // Fallback for unsupported expression types.
}

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
        ccd.is_autoincrement = col.is_autoincrement;
        if (col.default_expr) {
            ccd.default_expr = expr_to_sql(*col.default_expr);
        }
        ts.columns.push_back(std::move(ccd));
    }

    // Validate EMBEDDING column provider references against the provider cache.
    if (provider_cache_) {
        for (const auto& col : stmt.columns) {
            if (!col.type.provider.empty()) {
                auto valid = provider_cache_->validate_provider_exists(col.type.provider);
                if (!valid) {
                    return make_error(StatusCode::NOT_FOUND,
                                      "column '" + col.name + "' references unknown provider '" +
                                          col.type.provider + "'");
                }
            }
        }
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

    // Validate AUTOINCREMENT constraints.
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const auto& col = stmt.columns[i];
        if (!col.is_autoincrement) {
            continue;
        }

        // Must be an integer type.
        if (!is_integer(ts.columns[i].type_id)) {
            return make_error(StatusCode::TYPE_ERROR,
                              "AUTOINCREMENT requires an integer type, column '" + col.name +
                                  "' has type " + col.type.name);
        }

        // Cannot have DEFAULT.
        if (col.default_expr) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT column '" + col.name + "' cannot have DEFAULT");
        }

        // Must be PRIMARY KEY. Check both inline PRIMARY KEY (sets is_unique
        // + !nullable on the column) and table-level constraints.
        bool is_pk = (col.is_unique && !col.nullable);
        if (!is_pk) {
            for (const auto& constraint : stmt.constraints) {
                if (constraint.kind == TableConstraint::Kind::PRIMARY_KEY) {
                    for (const auto& pk_col : constraint.columns) {
                        auto upper_pk = pk_col;
                        auto upper_name = col.name;
                        std::transform(
                            upper_pk.begin(),
                            upper_pk.end(),
                            upper_pk.begin(),
                            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        std::transform(
                            upper_name.begin(),
                            upper_name.end(),
                            upper_name.begin(),
                            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        if (upper_pk == upper_name) {
                            is_pk = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!is_pk) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "AUTOINCREMENT column '" + col.name + "' must be PRIMARY KEY");
        }
    }

    // Register in catalog.
    auto table_id = catalog_.create_table(current_database_id_, std::move(ts));
    if (!table_id) {
        if (stmt.if_not_exists && table_id.error().code == StatusCode::ALREADY_EXISTS) {
            QueryResult qr;
            qr.message = "CREATE TABLE";
            return ok(std::move(qr));
        }
        return make_error(table_id.error().code, table_id.error().message);
    }

    // Retrieve the created schema (now has assigned table_id).
    auto schema = catalog_.get_table(current_database_id_, stmt.name);
    if (!schema) {
        return make_error(schema.error().code, schema.error().message);
    }

    // Initialize auto-increment counter if any column has AUTOINCREMENT.
    for (const auto& col : schema->columns) {
        if (col.is_autoincrement) {
            catalog_.init_autoincrement(schema->table_id);
            break;
        }
    }

    // Create physical storage.
    auto storage_result = storage_.create_table_storage(current_database_id_, *table_id, *schema);
    if (!storage_result) {
        return make_error(storage_result.error().code, storage_result.error().message);
    }

    // Persist to system catalog tables.
    if (catalog_persistence_ != nullptr) {
        auto persist = catalog_persistence_->persist_table(current_database_id_, *schema);
        if (!persist) {
            GIODB_LOG_WARN("failed to persist table '{}': {}", stmt.name, persist.error().message);
        }
    }

    QueryResult qr;
    qr.message = "CREATE TABLE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: DROP TABLE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_drop_table(const DropTableStmt& stmt) {
    auto schema = catalog_.get_table(current_database_id_, stmt.name);
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
    auto drop_storage = storage_.drop_table_storage(current_database_id_, table_id);
    if (!drop_storage) {
        return make_error(drop_storage.error().code, drop_storage.error().message);
    }

    // Remove from persistence before dropping from catalog.
    if (catalog_persistence_ != nullptr) {
        auto remove = catalog_persistence_->remove_table(table_id);
        if (!remove) {
            GIODB_LOG_WARN("failed to remove table '{}' from persistence: {}",
                           stmt.name,
                           remove.error().message);
        }
    }

    // Remove from catalog.
    auto drop_catalog = catalog_.drop_table(current_database_id_, stmt.name);
    if (!drop_catalog) {
        return make_error(drop_catalog.error().code, drop_catalog.error().message);
    }

    QueryResult qr;
    qr.message = "DROP TABLE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: CREATE EDGE TYPE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_create_edge_type(const CreateEdgeTypeStmt& stmt) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "graph engine not available for CREATE EDGE TYPE");
    }

    // Resolve source and target tables.
    auto from_schema = catalog_.get_table(current_database_id_, stmt.from_table);
    if (!from_schema) {
        return make_error(from_schema.error().code, from_schema.error().message);
    }
    auto to_schema = catalog_.get_table(current_database_id_, stmt.to_table);
    if (!to_schema) {
        return make_error(to_schema.error().code, to_schema.error().message);
    }

    // Find PK types.
    TypeId from_pk_type = TypeId::INT64;
    TypeId to_pk_type = TypeId::INT64;
    for (const auto& col : from_schema->columns) {
        if (col.name == from_schema->pk_columns) {
            from_pk_type = col.type_id;
            break;
        }
    }
    for (const auto& col : to_schema->columns) {
        if (col.name == to_schema->pk_columns) {
            to_pk_type = col.type_id;
            break;
        }
    }

    // Convert property columns.
    std::vector<ColumnDef> prop_cols;
    for (const auto& prop : stmt.properties) {
        auto type_result = resolve_type_spec(prop.type);
        if (!type_result) {
            return make_error(type_result.error().code, type_result.error().message);
        }
        prop_cols.push_back({prop.name, *type_result});
    }

    auto result = graph_engine_->create_edge_type(
        stmt.name, from_schema->table_id, to_schema->table_id, from_pk_type, to_pk_type, prop_cols);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.message = "CREATE EDGE TYPE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: DROP EDGE TYPE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_drop_edge_type(const DropEdgeTypeStmt& stmt) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "graph engine not available for DROP EDGE TYPE");
    }

    auto result = graph_engine_->drop_edge_type(stmt.name);
    if (!result) {
        if (stmt.if_exists && result.error().code == StatusCode::NOT_FOUND) {
            QueryResult qr;
            qr.message = "DROP EDGE TYPE";
            return ok(std::move(qr));
        }
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.message = "DROP EDGE TYPE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// LINK
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_link(const LinkStmt& stmt, const BoundStatement& bound) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for LINK");
    }

    Tuple empty_tuple;
    OutputSchema empty_schema;

    auto src_key = evaluate_expr(*stmt.source_key, empty_tuple, empty_schema, bound);
    if (!src_key) {
        return make_error(src_key.error().code, src_key.error().message);
    }

    auto tgt_key = evaluate_expr(*stmt.target_key, empty_tuple, empty_schema, bound);
    if (!tgt_key) {
        return make_error(tgt_key.error().code, tgt_key.error().message);
    }

    // Evaluate property values.
    std::vector<Value> props;
    for (const auto& assign : stmt.properties) {
        auto val = evaluate_expr(*assign.value, empty_tuple, empty_schema, bound);
        if (!val) {
            return make_error(val.error().code, val.error().message);
        }
        props.push_back(std::move(*val));
    }

    auto result = graph_engine_->link(stmt.edge_type, *src_key, *tgt_key, props);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.affected_rows = 1;
    qr.message = "LINK";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// UNLINK
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_unlink(const UnlinkStmt& stmt,
                                                const BoundStatement& bound) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for UNLINK");
    }

    Tuple empty_tuple;
    OutputSchema empty_schema;

    auto src_key = evaluate_expr(*stmt.source_key, empty_tuple, empty_schema, bound);
    if (!src_key) {
        return make_error(src_key.error().code, src_key.error().message);
    }

    auto tgt_key = evaluate_expr(*stmt.target_key, empty_tuple, empty_schema, bound);
    if (!tgt_key) {
        return make_error(tgt_key.error().code, tgt_key.error().message);
    }

    auto result = graph_engine_->unlink(stmt.edge_type, *src_key, *tgt_key);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.affected_rows = 1;
    qr.message = "UNLINK";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// REEMBED TABLE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_reembed(const ReembedStmt& stmt) {
    // 1. Resolve the table.
    auto table_schema = catalog_.get_table(current_database_id_, stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    // 2. Get embedding column definitions.
    auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
    if (emb_cols.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "table '" + stmt.table_name + "' has no EMBEDDING columns");
    }

    // 3. Validate provider registry is available.
    if (provider_registry_ == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "REEMBED requires a ProviderRegistry");
    }

    // 4. Resolve providers and locate column indexes for each embedding column.
    struct EmbeddingTarget {
        EmbeddingColumnDef def;
        std::shared_ptr<EmbeddingProvider> provider;
        size_t column_index;
    };
    std::vector<EmbeddingTarget> targets;

    for (const auto& ec : emb_cols) {
        auto provider = provider_registry_->resolve(ec.provider);
        if (!provider) {
            return make_error(provider.error().code,
                              "failed to resolve provider '" + ec.provider +
                                  "': " + provider.error().message);
        }

        size_t col_idx = 0;
        for (size_t i = 0; i < table_schema->columns.size(); ++i) {
            if (table_schema->columns[i].ordinal == ec.column_id) {
                col_idx = i;
                break;
            }
        }

        targets.push_back({ec, std::move(*provider), col_idx});
    }

    // 5. Get table storage.
    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* table_storage = *ts;

    // 6. Sequential scan with batch embedding.
    auto scan_it = table_storage->heap->begin();
    if (!scan_it) {
        return make_error(scan_it.error().code, scan_it.error().message);
    }

    static constexpr size_t batch_size = 32;

    struct RowInfo {
        RID rid;
        std::vector<Value> values;
    };

    std::vector<RowInfo> batch;
    batch.reserve(batch_size);
    int64_t total_processed = 0;
    int64_t total_skipped = 0;
    auto start_time = std::chrono::steady_clock::now();

    // Lambda to process a batch of rows.
    auto process_batch = [&]() -> Result<void> {
        if (batch.empty()) {
            return ok();
        }

        for (auto& target : targets) {
            // Extract source texts from each row.
            std::vector<std::string> source_texts;
            source_texts.reserve(batch.size());

            for (const auto& row : batch) {
                std::string text;
                for (size_t i = 0; i < table_schema->columns.size(); ++i) {
                    if (table_schema->columns[i].name == target.def.source_expr) {
                        if (!row.values[i].is_null() && row.values[i].type_id() == TypeId::STRING) {
                            text = row.values[i].as_string();
                        }
                        break;
                    }
                }
                source_texts.push_back(std::move(text));
            }

            // Batch embed via the provider.
            auto embeddings = target.provider->embed_batch(source_texts);
            if (!embeddings) {
                GIODB_LOG_WARN("REEMBED: batch embed failed for provider '{}': {}",
                               target.def.provider,
                               embeddings.error().message);
                total_skipped += static_cast<int64_t>(batch.size());
                continue;
            }

            // Update each row's embedding and persist.
            for (size_t i = 0; i < batch.size(); ++i) {
                auto& row = batch[i];

                row.values[target.column_index] = Value(Embedding((*embeddings)[i]));

                auto serialized =
                    TupleSerializer::serialize(row.values, table_storage->storage_schema);
                if (!serialized) {
                    ++total_skipped;
                    continue;
                }

                auto update_result = table_storage->heap->update_tuple(row.rid, *serialized);
                if (!update_result) {
                    ++total_skipped;
                    continue;
                }
            }
        }

        return ok();
    };

    while (true) {
        auto row = scan_it->next();
        if (!row) {
            break;
        }

        auto& [rid, data] = *row;
        auto values = TupleSerializer::deserialize(data, table_storage->storage_schema);
        if (!values) {
            ++total_skipped;
            continue;
        }

        batch.push_back({rid, std::move(*values)});

        if (batch.size() >= batch_size) {
            auto result = process_batch();
            if (!result) {
                return make_error(result.error().code, result.error().message);
            }
            total_processed += static_cast<int64_t>(batch.size());
            batch.clear();

            if (total_processed % 1000 == 0) {
                GIODB_LOG_INFO("REEMBED: processed {} rows", total_processed);
            }
        }
    }

    // Process remaining rows.
    if (!batch.empty()) {
        auto result = process_batch();
        if (!result) {
            return make_error(result.error().code, result.error().message);
        }
        total_processed += static_cast<int64_t>(batch.size());
        batch.clear();
    }

    // 7. Rebuild HNSW indexes with the new embeddings.
    if (hnsw_indexes_ != nullptr) {
        for (const auto& target : targets) {
            auto index_name = EmbeddingColumnManager::make_index_name(
                stmt.table_name, table_schema->columns[target.column_index].name);
            auto idx_it = hnsw_indexes_->find(index_name);
            if (idx_it == hnsw_indexes_->end()) {
                continue;
            }
            auto* hnsw = idx_it->second;

            // Reset the index so new inserts start from node_id 0.
            auto reset_result = hnsw->reset();
            if (!reset_result) {
                GIODB_LOG_WARN("REEMBED: failed to reset HNSW index '{}': {}",
                               index_name,
                               reset_result.error().message);
                continue;
            }

            // Re-scan the table and insert all embeddings in row order.
            auto rebuild_it = table_storage->heap->begin();
            if (!rebuild_it) {
                GIODB_LOG_WARN("REEMBED: failed to scan table for HNSW rebuild: {}",
                               rebuild_it.error().message);
                continue;
            }

            uint32_t inserted = 0;
            while (true) {
                auto row = rebuild_it->next();
                if (!row) {
                    break;
                }
                auto& [rid, data] = *row;
                auto vals = TupleSerializer::deserialize(data, table_storage->storage_schema);
                if (!vals) {
                    continue;
                }
                const auto& emb_val = (*vals)[target.column_index];
                if (!emb_val.is_null() && emb_val.type_id() == TypeId::EMBEDDING) {
                    const auto& vec = emb_val.as_embedding();
                    auto ins = hnsw->insert(std::span<const float>(vec));
                    if (ins) {
                        ++inserted;
                    }
                }
            }

            GIODB_LOG_INFO(
                "REEMBED: rebuilt HNSW index '{}' with {} vectors", index_name, inserted);
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);

    GIODB_LOG_INFO("REEMBED: completed {} rows in {}ms ({} skipped)",
                   total_processed,
                   elapsed.count(),
                   total_skipped);

    QueryResult qr;
    qr.affected_rows = total_processed;
    qr.message = "REEMBED " + std::to_string(total_processed) + " rows";
    if (total_skipped > 0) {
        qr.message += " (" + std::to_string(total_skipped) + " skipped)";
    }
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// EXPLAIN / EXPLAIN ANALYZE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_explain(const ExplainStmt& stmt,
                                                 const BoundStatement& /*bound*/) {
    // Bind the inner statement.
    Binder inner_binder(catalog_, current_database_id_);
    auto inner_bound = inner_binder.bind(*stmt.statement);
    if (!inner_bound) {
        return make_error(inner_bound.error().code, inner_bound.error().message);
    }

    // Build iterator tree from the inner statement.
    Planner planner(catalog_, storage_, current_database_id_, graph_engine_);
    std::vector<ExprPtr> owned_exprs;
    auto iter = planner.plan(*inner_bound, owned_exprs);
    if (!iter) {
        return make_error(iter.error().code, iter.error().message);
    }

    if (stmt.analyze) {
        // EXPLAIN ANALYZE: enable instrumentation, execute, collect stats.
        (*iter)->enable_instrumentation();

        auto open_result = (*iter)->open();
        if (!open_result) {
            return make_error(open_result.error().code, open_result.error().message);
        }

        // Drain all rows.
        while (true) {
            auto row = (*iter)->next();
            if (!row) {
                (*iter)->close();
                return make_error(row.error().code, row.error().message);
            }
            if (!row->has_value()) {
                break;
            }
        }

        (*iter)->close();
    }

    return ok(ExplainFormatter::to_query_result(**iter, stmt.format, stmt.analyze));
}

// ---------------------------------------------------------------------------
// DML / Query execution via Planner
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_plan(const BoundStatement& bound) {
    // Pre-execution: enforce provider constraints for DML on sys_providers.
    std::vector<ProviderConfig> protected_providers;
    bool is_provider_delete = false;

    if (provider_cache_) {
        // Default uniqueness: auto-unset previous defaults before INSERT/UPDATE
        // that sets is_default = TRUE.
        if (auto* ins = dynamic_cast<const InsertStmt*>(bound.stmt)) {
            if (ins->table_name == "sys_providers") {
                // Find the is_default column index in the INSERT.
                int def_idx = -1;
                if (ins->columns.empty()) {
                    def_idx = 6; // is_default is the 7th column in schema order.
                } else {
                    for (size_t i = 0; i < ins->columns.size(); ++i) {
                        if (ins->columns[i] == "is_default") {
                            def_idx = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (def_idx >= 0) {
                    bool sets_default = false;
                    for (const auto& row : ins->values) {
                        if (def_idx < static_cast<int>(row.size())) {
                            if (auto* lit = dynamic_cast<const LiteralExpr*>(row[def_idx].get())) {
                                if (lit->kind == LiteralKind::BOOLEAN && lit->value == "true") {
                                    sets_default = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (sets_default) {
                        auto prev_db = current_database_id_;
                        set_current_database(system_database_id);
                        auto unset = execute("UPDATE sys_providers SET is_default = FALSE "
                                             "WHERE is_default = TRUE");
                        set_current_database(prev_db);
                        if (!unset) {
                            GIODB_LOG_WARN("failed to unset existing default provider: {}",
                                           unset.error().message);
                        }
                    }
                }
            }
        } else if (auto* upd = dynamic_cast<const UpdateStmt*>(bound.stmt)) {
            if (upd->table_name == "sys_providers") {
                for (const auto& assign : upd->assignments) {
                    if (assign.column == "is_default") {
                        if (auto* lit = dynamic_cast<const LiteralExpr*>(assign.value.get())) {
                            if (lit->kind == LiteralKind::BOOLEAN && lit->value == "true") {
                                auto prev_db = current_database_id_;
                                set_current_database(system_database_id);
                                auto unset = execute("UPDATE sys_providers SET is_default = FALSE "
                                                     "WHERE is_default = TRUE");
                                set_current_database(prev_db);
                                if (!unset) {
                                    GIODB_LOG_WARN("failed to unset existing default provider: {}",
                                                   unset.error().message);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        } else if (auto* del = dynamic_cast<const DeleteStmt*>(bound.stmt)) {
            // Referential integrity: snapshot in-use providers before DELETE.
            if (del->table_name == "sys_providers") {
                is_provider_delete = true;
                auto all = provider_cache_->get_all();
                for (const auto& p : all) {
                    if (provider_cache_->is_provider_in_use(p.name, catalog_)) {
                        protected_providers.push_back(p);
                    }
                }
            }
        }
    }

    // Build iterator tree.
    Planner planner(catalog_, storage_, current_database_id_, graph_engine_);
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

    // Mask api_key_encrypted in SELECT results from sys_providers.
    // Skip masking when internal queries need raw encrypted values
    // (e.g., ProviderCache::load uses push_skip_masking/pop_skip_masking).
    if (skip_masking_depth_ == 0) {
        for (size_t col = 0; col < qr.column_names.size(); ++col) {
            if (qr.column_names[col] == "api_key_encrypted") {
                for (auto& row : qr.rows) {
                    if (col < row.size() && !row[col].is_null()) {
                        row[col] = Value(std::string("********"));
                    }
                }
                break;
            }
        }
    }

    // Invalidate provider cache if DML targeted sys_providers.
    maybe_invalidate_provider_cache(bound);

    // Post-execution: verify no in-use providers were deleted.
    if (is_provider_delete && !protected_providers.empty()) {
        for (const auto& pp : protected_providers) {
            if (!provider_cache_->get(pp.name).has_value()) {
                // Compensate: re-insert the deleted in-use provider.
                auto prev_db = current_database_id_;
                set_current_database(system_database_id);
                std::string api_key_val =
                    pp.api_key.empty() ? "NULL" : "'" + pp.api_key.str() + "'";
                std::string sql =
                    "INSERT INTO sys_providers VALUES (" + std::to_string(pp.provider_id) + ", '" +
                    pp.name + "', '" + pp.type + "', '" + pp.endpoint + "', '" + pp.model + "', " +
                    api_key_val + ", " + (pp.is_default ? "TRUE" : "FALSE") + ", NULL)";
                auto re_insert = execute(sql);
                set_current_database(prev_db);

                if (!re_insert) {
                    GIODB_LOG_ERROR("failed to restore in-use provider '{}': {}",
                                    pp.name,
                                    re_insert.error().message);
                }
                // Reload cache to reflect the restored provider.
                auto reload = provider_cache_->load(*this);
                if (!reload) {
                    GIODB_LOG_WARN("failed to reload provider cache: {}", reload.error().message);
                }

                return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                  "cannot delete provider '" + pp.name +
                                      "': referenced by EMBEDDING columns");
            }
        }
    }

    return ok(std::move(qr));
}

void QueryEngine::maybe_invalidate_provider_cache(const BoundStatement& bound) {
    if (!provider_cache_ || provider_cache_->is_loading()) {
        return;
    }

    // Detect if the DML targeted the sys_providers table.
    std::string target_table;
    if (auto* ins = dynamic_cast<const InsertStmt*>(bound.stmt)) {
        target_table = ins->table_name;
    } else if (auto* upd = dynamic_cast<const UpdateStmt*>(bound.stmt)) {
        target_table = upd->table_name;
    } else if (auto* del = dynamic_cast<const DeleteStmt*>(bound.stmt)) {
        target_table = del->table_name;
    }

    if (target_table == "sys_providers") {
        auto reload = provider_cache_->load(*this);
        if (!reload) {
            GIODB_LOG_WARN("failed to reload provider cache after DML: {}", reload.error().message);
        }
    }
}

// ---------------------------------------------------------------------------
// SET parameter = value
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_set(const SetStmt& stmt) {
    if (!settings_cache_) {
        return make_error(
            StatusCode::INTERNAL_ERROR,
            "settings cache not initialized (system database may not be bootstrapped)");
    }

    // Extract the value from the expression (must be a literal).
    std::string value_str;
    if (auto* lit = dynamic_cast<const LiteralExpr*>(stmt.value.get())) {
        value_str = lit->value;
    } else {
        return make_error(StatusCode::INVALID_ARGUMENT, "SET value must be a literal");
    }

    // Validate and update the in-memory cache (checks existence + mutability).
    auto update_result = settings_cache_->update(stmt.parameter, value_str);
    if (!update_result) {
        return make_error(update_result.error().code, update_result.error().message);
    }

    // Persist to sys_settings table.
    auto prev_db = current_database_id_;
    set_current_database(system_database_id);

    // Escape single quotes in the value.
    std::string escaped_value;
    escaped_value.reserve(value_str.size());
    for (char c : value_str) {
        if (c == '\'') {
            escaped_value += "''";
        } else {
            escaped_value += c;
        }
    }

    auto persist = execute("UPDATE sys_settings SET value = '" + escaped_value + "' WHERE key = '" +
                           stmt.parameter + "'");
    set_current_database(prev_db);

    if (!persist) {
        return make_error(persist.error().code,
                          "SET succeeded in cache but failed to persist: " +
                              persist.error().message);
    }

    // Apply runtime side-effect.
    SettingsCache::apply_runtime_change(stmt.parameter, value_str);

    QueryResult qr;
    qr.message = "SET";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// SHOW
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_show(const ShowStmt& stmt) {
    switch (stmt.target) {

    case ShowTarget::PARAMETER: {
        if (!settings_cache_) {
            return make_error(
                StatusCode::INTERNAL_ERROR,
                "settings cache not initialized (system database may not be bootstrapped)");
        }
        auto entry = settings_cache_->get(stmt.name);
        if (!entry) {
            return make_error(StatusCode::NOT_FOUND, "unrecognized parameter '" + stmt.name + "'");
        }

        QueryResult qr;
        qr.column_names = {"key", "value"};
        qr.column_types = {TypeId::STRING, TypeId::STRING};
        qr.rows.push_back({Value(entry->key), Value(entry->value)});
        return ok(std::move(qr));
    }

    case ShowTarget::ALL: {
        if (!settings_cache_) {
            return make_error(
                StatusCode::INTERNAL_ERROR,
                "settings cache not initialized (system database may not be bootstrapped)");
        }
        auto entries = settings_cache_->get_all();

        QueryResult qr;
        qr.column_names = {"key", "value", "category", "is_runtime_mutable"};
        qr.column_types = {TypeId::STRING, TypeId::STRING, TypeId::STRING, TypeId::BOOL};
        for (const auto& e : entries) {
            qr.rows.push_back(
                {Value(e.key), Value(e.value), Value(e.category), Value(e.is_runtime_mutable)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::DATABASES: {
        auto databases = catalog_.list_databases();

        QueryResult qr;
        qr.column_names = {"database_name"};
        qr.column_types = {TypeId::STRING};
        for (const auto& db : databases) {
            qr.rows.push_back({Value(db.name)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::TABLES: {
        auto tables = catalog_.list_tables(current_database_id_);

        QueryResult qr;
        qr.column_names = {"table_name"};
        qr.column_types = {TypeId::STRING};
        for (const auto& ts : tables) {
            qr.rows.push_back({Value(ts.name)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::COLUMNS: {
        auto schema = catalog_.get_table(current_database_id_, stmt.name);
        if (!schema) {
            return make_error(schema.error().code, schema.error().message);
        }

        QueryResult qr;
        qr.column_names = {"column_name", "type", "nullable", "default", "autoincrement"};
        qr.column_types = {
            TypeId::STRING, TypeId::STRING, TypeId::BOOL, TypeId::STRING, TypeId::BOOL};
        for (const auto& col : schema->columns) {
            qr.rows.push_back({Value(col.name),
                               Value(std::string(type_name(col.type_id))),
                               Value(col.nullable),
                               col.default_expr.empty() ? Value() : Value(col.default_expr),
                               Value(col.is_autoincrement)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::INDEXES: {
        auto indexes = catalog_.list_all_indexes();

        QueryResult qr;
        qr.column_names = {"index_name", "table_name", "columns", "type", "unique"};
        qr.column_types = {
            TypeId::STRING, TypeId::STRING, TypeId::STRING, TypeId::STRING, TypeId::BOOL};
        for (const auto& idx : indexes) {
            auto table = catalog_.get_table_by_id(idx.table_id);
            std::string table_name = table ? table->name : "unknown";
            qr.rows.push_back({Value(idx.name),
                               Value(table_name),
                               Value(idx.columns),
                               Value(idx.index_type),
                               Value(idx.is_unique)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::EDGE_TYPES: {
        auto edge_types = catalog_.list_edge_types();

        QueryResult qr;
        qr.column_names = {"edge_type", "source_table", "target_table"};
        qr.column_types = {TypeId::STRING, TypeId::STRING, TypeId::STRING};
        for (const auto& et : edge_types) {
            auto src = catalog_.get_table_by_id(et.source_table_id);
            auto tgt = catalog_.get_table_by_id(et.target_table_id);
            qr.rows.push_back({Value(et.name),
                               Value(src ? src->name : std::string("unknown")),
                               Value(tgt ? tgt->name : std::string("unknown"))});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::EMBEDDINGS: {
        std::vector<EmbeddingColumnDef> embeddings;
        if (!stmt.name.empty()) {
            // SHOW EMBEDDINGS FROM table — filter by table name.
            auto schema = catalog_.get_table(current_database_id_, stmt.name);
            if (!schema) {
                return make_error(schema.error().code, schema.error().message);
            }
            embeddings = catalog_.list_embedding_columns(schema->table_id);
        } else {
            embeddings = catalog_.list_all_embedding_columns();
        }

        QueryResult qr;
        qr.column_names = {"table_name", "column_name", "dimension", "source_expr", "provider"};
        qr.column_types = {
            TypeId::STRING, TypeId::STRING, TypeId::INT32, TypeId::STRING, TypeId::STRING};
        for (const auto& emb : embeddings) {
            auto table = catalog_.get_table_by_id(emb.table_id);
            std::string table_name = table ? table->name : "unknown";
            std::string col_name = "unknown";
            if (table) {
                for (const auto& col : table->columns) {
                    if (col.ordinal == emb.column_id) {
                        col_name = col.name;
                        break;
                    }
                }
            }
            qr.rows.push_back({Value(table_name),
                               Value(col_name),
                               Value(emb.dimension),
                               Value(emb.source_expr),
                               Value(emb.provider)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::PROVIDERS: {
        if (!provider_cache_) {
            return make_error(
                StatusCode::INTERNAL_ERROR,
                "provider cache not initialized (system database may not be bootstrapped)");
        }
        auto providers = provider_cache_->get_all();

        QueryResult qr;
        qr.column_names = {"name", "type", "endpoint", "model", "is_default"};
        qr.column_types = {
            TypeId::STRING, TypeId::STRING, TypeId::STRING, TypeId::STRING, TypeId::BOOL};
        for (const auto& p : providers) {
            qr.rows.push_back({Value(p.name),
                               Value(p.type),
                               Value(p.endpoint),
                               Value(p.model),
                               Value(p.is_default)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::REPLICATION_SLOTS: {
        if (!slot_mgr_) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "replication slot manager not initialized");
        }
        auto slots = slot_mgr_->list_slots();

        QueryResult qr;
        qr.column_names = {
            "slot_name", "slot_type", "active", "restart_lsn", "confirmed_flush_lsn"};
        qr.column_types = {
            TypeId::STRING, TypeId::STRING, TypeId::BOOL, TypeId::INT64, TypeId::INT64};
        for (const auto& s : slots) {
            qr.rows.push_back({Value(s.slot_name),
                               Value(s.slot_type),
                               Value(s.active),
                               s.restart_lsn != invalid_lsn
                                   ? Value(static_cast<int64_t>(s.restart_lsn))
                                   : Value(),
                               s.confirmed_flush_lsn != invalid_lsn
                                   ? Value(static_cast<int64_t>(s.confirmed_flush_lsn))
                                   : Value()});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::REPLICATION_STATUS: {
        if (sender_mgr_ == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "WAL sender manager not initialized (not running as primary)");
        }
        auto statuses = sender_mgr_->get_sender_statuses();

        QueryResult qr;
        qr.column_names = {"slot_name",
                           "client_addr",
                           "state",
                           "sent_lsn",
                           "write_lsn",
                           "flush_lsn",
                           "replay_lsn",
                           "write_lag_ms",
                           "flush_lag_ms",
                           "replay_lag_ms",
                           "sync_state"};
        qr.column_types = {TypeId::STRING,
                           TypeId::STRING,
                           TypeId::STRING,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::STRING};

        // Get current WAL position for lag calculation.
        lsn_t current_lsn = wal_writer_ ? wal_writer_->current_lsn() : 0;

        for (const auto& s : statuses) {
            // State string.
            std::string state_str;
            switch (s.state) {
            case WalSender::State::CREATED:
                state_str = "startup";
                break;
            case WalSender::State::CATCHING_UP:
                state_str = "catchup";
                break;
            case WalSender::State::STREAMING:
                state_str = "streaming";
                break;
            case WalSender::State::STOPPED:
                state_str = "stopped";
                break;
            }

            // Lag is the difference between primary's current LSN and replica's
            // reported positions (in bytes, used as a proxy for milliseconds
            // since we don't track timestamps per-LSN).
            auto lag = [current_lsn](lsn_t replica_lsn) -> int64_t {
                if (replica_lsn == invalid_lsn || current_lsn == 0) {
                    return -1;
                }
                return static_cast<int64_t>(current_lsn) - static_cast<int64_t>(replica_lsn);
            };

            qr.rows.push_back(
                {Value(s.slot_name),
                 Value(s.peer),
                 Value(state_str),
                 s.sent_lsn != invalid_lsn ? Value(static_cast<int64_t>(s.sent_lsn)) : Value(),
                 s.received_lsn != invalid_lsn ? Value(static_cast<int64_t>(s.received_lsn))
                                               : Value(),
                 s.flushed_lsn != invalid_lsn ? Value(static_cast<int64_t>(s.flushed_lsn))
                                              : Value(),
                 s.applied_lsn != invalid_lsn ? Value(static_cast<int64_t>(s.applied_lsn))
                                              : Value(),
                 Value(lag(s.received_lsn)),
                 Value(lag(s.flushed_lsn)),
                 Value(lag(s.applied_lsn)),
                 Value(s.sync_state)});
        }
        return ok(std::move(qr));
    }

    case ShowTarget::STANDBY_STATUS: {
        if (wal_receiver_ == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "WAL receiver not initialized (not running as standby)");
        }
        auto state = wal_receiver_->get_state();

        QueryResult qr;
        qr.column_names = {
            "received_lsn", "applied_lsn", "flushed_lsn", "replication_lag_ms", "is_streaming"};
        qr.column_types = {
            TypeId::INT64, TypeId::INT64, TypeId::INT64, TypeId::INT64, TypeId::BOOL};
        qr.rows.push_back(
            {state.received_lsn != invalid_lsn ? Value(static_cast<int64_t>(state.received_lsn))
                                               : Value(),
             state.applied_lsn != invalid_lsn ? Value(static_cast<int64_t>(state.applied_lsn))
                                              : Value(),
             state.flushed_lsn != invalid_lsn ? Value(static_cast<int64_t>(state.flushed_lsn))
                                              : Value(),
             Value(static_cast<int64_t>(state.replication_lag.count())),
             Value(state.is_streaming)});
        return ok(std::move(qr));
    }

    } // switch

    return make_error(StatusCode::NOT_IMPLEMENTED, "unsupported SHOW target");
}

// ---------------------------------------------------------------------------
// DDL: CREATE USER
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_create_user(const CreateUserStmt& stmt) {
    if (!user_mgr_) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "user manager not initialized for CREATE USER");
    }

    auto result = user_mgr_->create_user(stmt.username, stmt.password, auth_method_);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    GIODB_LOG_INFO("created user '{}'", stmt.username);

    QueryResult qr;
    qr.message = "CREATE USER";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: DROP USER
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_drop_user(const DropUserStmt& stmt) {
    if (!user_mgr_) {
        return make_error(StatusCode::INTERNAL_ERROR, "user manager not initialized for DROP USER");
    }

    auto result = user_mgr_->drop_user(stmt.username, stmt.if_exists);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    GIODB_LOG_INFO("dropped user '{}'", stmt.username);

    QueryResult qr;
    qr.message = "DROP USER";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: ALTER USER
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_alter_user(const AlterUserStmt& stmt) {
    if (!user_mgr_) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "user manager not initialized for ALTER USER");
    }

    auto result = user_mgr_->alter_user(stmt.username, stmt.password, auth_method_);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    GIODB_LOG_INFO("altered user '{}'", stmt.username);

    QueryResult qr;
    qr.message = "ALTER USER";
    return ok(std::move(qr));
}

} // namespace giodb
