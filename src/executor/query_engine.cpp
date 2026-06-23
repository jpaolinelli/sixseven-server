#include "sixseven/executor/query_engine.h"

#include "sixseven/catalog/schema.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/statement_deadline.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/delete.h"
#include "sixseven/executor/explain.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/insert.h"
#include "sixseven/executor/pk_value_string.h"
#include "sixseven/executor/planner.h"
#include "sixseven/executor/provider_cache.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/update.h"
#include "sixseven/index/rid.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/planner/statistics.h"
#include "sixseven/planner/type_resolver.h"
#include "sixseven/server/auth.h"
#include "sixseven/server/replication_slot.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/server/wal_sender_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/table/tuple.h"
#include "sixseven/txn/read_view.h"
#include "sixseven/vector/backfill_manager.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/provider_registry.h"

#include <chrono>
#include <span>
#include <sstream>
#include <string>
#include <unordered_set>

namespace sixseven {

namespace {

/// Convert a string to uppercase for case-insensitive comparisons.
std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Result of folding a constant expression for the SELECT-without-FROM fast path.
struct ConstFoldResult {
    Value value;
    TypeId type_id;
    std::string default_name; ///< Default column label when no alias is given.
};

/// Try to evaluate a SELECT-list expression as a compile-time constant.
///
/// Supports:
///   - LiteralExpr (INTEGER / FLOAT / STRING / BOOLEAN / NULL).
///   - UnaryExpr(NEGATE) wrapping a numeric literal or another foldable
///     numeric expression (e.g. `-5`, `--5`).
///   - UnaryExpr(NOT) wrapping a foldable boolean expression.
///
/// Returns std::nullopt for any expression shape that requires the planner.
std::optional<ConstFoldResult> try_fold_const_expr(const Expr& expr) {
    if (const auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        ConstFoldResult r;
        switch (lit->kind) {
        case LiteralKind::INTEGER:
            r.type_id = TypeId::INT32;
            try {
                r.value = Value(static_cast<int32_t>(std::stoi(lit->value)));
            } catch (...) {
                return std::nullopt;
            }
            r.default_name = lit->value;
            return r;
        case LiteralKind::FLOAT:
            r.type_id = TypeId::FLOAT64;
            try {
                r.value = Value(std::stod(lit->value));
            } catch (...) {
                return std::nullopt;
            }
            r.default_name = lit->value;
            return r;
        case LiteralKind::BOOLEAN:
            r.type_id = TypeId::BOOL;
            r.value = Value(lit->value == "true" || lit->value == "TRUE");
            r.default_name = lit->value;
            return r;
        case LiteralKind::NULL_LITERAL:
            r.type_id = TypeId::STRING;
            r.value = Value::make_null();
            r.default_name = "NULL";
            return r;
        case LiteralKind::STRING:
            r.type_id = TypeId::STRING;
            r.value = Value(lit->value);
            r.default_name = lit->value;
            return r;
        }
        return std::nullopt;
    }

    if (const auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (un->operand == nullptr) {
            return std::nullopt;
        }
        auto inner = try_fold_const_expr(*un->operand);
        if (!inner) {
            return std::nullopt;
        }

        if (un->op == UnaryOp::NEGATE) {
            ConstFoldResult r;
            r.default_name = "-" + inner->default_name;
            if (inner->value.is_null()) {
                // Negating NULL stays NULL; preserve numeric type when known.
                r.type_id = inner->type_id == TypeId::STRING ? TypeId::INT32 : inner->type_id;
                r.value = Value::make_null();
                return r;
            }
            switch (inner->type_id) {
            case TypeId::INT32: {
                int32_t v = inner->value.as_int32();
                r.type_id = TypeId::INT32;
                r.value = Value(static_cast<int32_t>(-v));
                return r;
            }
            case TypeId::INT64: {
                int64_t v = inner->value.as_int64();
                r.type_id = TypeId::INT64;
                r.value = Value(static_cast<int64_t>(-v));
                return r;
            }
            case TypeId::FLOAT64: {
                double v = inner->value.as_float64();
                r.type_id = TypeId::FLOAT64;
                r.value = Value(-v);
                return r;
            }
            default:
                return std::nullopt; // Unary minus on non-numeric is not foldable here.
            }
        }

        if (un->op == UnaryOp::NOT) {
            ConstFoldResult r;
            r.default_name = "NOT " + inner->default_name;
            if (inner->value.is_null()) {
                r.type_id = TypeId::BOOL;
                r.value = Value::make_null();
                return r;
            }
            if (inner->type_id != TypeId::BOOL) {
                return std::nullopt;
            }
            r.type_id = TypeId::BOOL;
            r.value = Value(!inner->value.as_bool());
            return r;
        }
    }

    return std::nullopt;
}

} // namespace

QueryEngine::QueryEngine(Catalog& catalog, StorageManager& storage, GraphEngine* graph_engine)
    : catalog_(catalog), storage_(storage), graph_engine_(graph_engine) {
    // Table heaps filter tuple versions through this engine's transaction
    // manager (GDB-747): DML stamps real xmin/xmax ids and scans hide
    // versions from aborted transactions.
    storage_.set_txn_manager(&txn_mgr_);
}

void QueryEngine::set_current_database(database_id_t database_id) {
    current_database_id_ = database_id;
}

database_id_t QueryEngine::current_database_id() const {
    return current_database_id_;
}

void QueryEngine::set_provider_registry(ProviderRegistry* registry) {
    provider_registry_ = registry;
}

void QueryEngine::set_hnsw_indexes(std::unordered_map<index_id_t, HnswIndex*>* indexes) {
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

void QueryEngine::set_embedding_worker_pool(EmbeddingWorkerPool* pool) {
    embedding_pool_ = pool;
    if (pool != nullptr) {
        pool->set_store_callback([this](table_id_t table_id,
                                        int64_t row_id,
                                        int32_t column_id,
                                        std::span<const float> embedding) -> Result<void> {
            // Decode RID from packed int64_t.
            RID rid{static_cast<PageId>(static_cast<uint64_t>(row_id) >> 32),
                    static_cast<SlotId>(row_id & 0xFFFF)};

            auto ts = storage_.get_table_storage(table_id);
            if (!ts) {
                return make_error(ts.error().code, ts.error().message);
            }
            auto* table_storage = *ts;

            // Read existing tuple.
            auto data = table_storage->heap->get_tuple(rid);
            if (!data) {
                return make_error(data.error().code, data.error().message);
            }

            auto values = TupleSerializer::deserialize(*data, table_storage->storage_schema);
            if (!values) {
                return make_error(values.error().code, values.error().message);
            }

            // Find the embedding column by ordinal and update it.
            auto table_schema = catalog_.get_table_by_id(table_id);
            if (!table_schema) {
                return make_error(table_schema.error().code, table_schema.error().message);
            }

            for (size_t i = 0; i < table_schema->columns.size(); ++i) {
                if (table_schema->columns[i].ordinal == column_id) {
                    (*values)[i] =
                        Value(Embedding(std::vector<float>(embedding.begin(), embedding.end())));
                    break;
                }
            }

            // Re-serialize and update tuple.
            auto serialized = TupleSerializer::serialize(*values, table_storage->storage_schema);
            if (!serialized) {
                return make_error(serialized.error().code, serialized.error().message);
            }

            RID effective_rid = rid;
            auto update = table_storage->heap->update_tuple(rid, *serialized);
            if (!update) {
                // If update fails due to space, delete and reinsert on a new page.
                auto del = table_storage->heap->delete_tuple(rid);
                if (!del) {
                    return make_error(del.error().code,
                                      "embedding store: delete failed: " + del.error().message);
                }
                auto new_rid = table_storage->heap->insert_tuple(*serialized);
                if (!new_rid) {
                    return make_error(new_rid.error().code,
                                      "embedding store: reinsert failed: " +
                                          new_rid.error().message);
                }
                effective_rid = *new_rid;
            }

            // Insert the new embedding into the HNSW index.
            if (hnsw_indexes_ != nullptr) {
                std::string col_name;
                for (const auto& col : table_schema->columns) {
                    if (col.ordinal == column_id) {
                        col_name = col.name;
                        break;
                    }
                }
                if (!col_name.empty()) {
                    // Find the HNSW index for this table/column by index_id.
                    auto indexes = catalog_.list_indexes(table_id);
                    for (const auto& idx : indexes) {
                        if (idx.index_type == "hnsw" && idx.columns == col_name) {
                            auto it = hnsw_indexes_->find(idx.index_id);
                            if (it != hnsw_indexes_->end()) {
                                auto ins = it->second->insert(embedding);
                                if (!ins) {
                                    SIXSEVEN_LOG_WARN(
                                        "embedding store: HNSW insert failed for '{}': {}",
                                        idx.name,
                                        ins.error().message);
                                } else if (index_manager_) {
                                    index_manager_->append_hnsw_rid(idx.index_id, effective_rid);
                                }
                            }
                            break;
                        }
                    }
                }
            }

            return ok();
        });

        // -- Batch store callback: groups writes by page to cut latch contention --
        pool->set_batch_store_callback([this](std::vector<EmbeddingStoreRequest> requests)
                                           -> Result<std::vector<int64_t>> {
            // Group requests by table_id to minimise get_table_storage calls.
            std::unordered_map<table_id_t, std::vector<size_t>> table_groups;
            for (size_t i = 0; i < requests.size(); ++i) {
                table_groups[requests[i].table_id].push_back(i);
            }

            std::vector<int64_t> failed_row_ids;

            for (auto& [table_id, indices] : table_groups) {
                auto ts = storage_.get_table_storage(table_id);
                if (!ts) {
                    SIXSEVEN_LOG_WARN("batch store: get_table_storage({}) failed: {}",
                                      table_id,
                                      ts.error().message);
                    for (auto idx : indices) {
                        failed_row_ids.push_back(requests[idx].row_id);
                    }
                    continue;
                }
                auto* table_storage = *ts;

                auto table_schema = catalog_.get_table_by_id(table_id);
                if (!table_schema) {
                    SIXSEVEN_LOG_WARN("batch store: get_table_by_id({}) failed: {}",
                                      table_id,
                                      table_schema.error().message);
                    for (auto idx : indices) {
                        failed_row_ids.push_back(requests[idx].row_id);
                    }
                    continue;
                }

                // Pre-compute serialized bytes for all embeddings OUTSIDE
                // any exclusive page lock. get_tuple uses a shared latch.
                std::vector<TableHeap::TupleUpdate> updates;
                std::vector<std::vector<uint8_t>> serialized_bufs;
                updates.reserve(indices.size());
                serialized_bufs.reserve(indices.size());

                for (auto idx : indices) {
                    auto& req = requests[idx];
                    RID rid{static_cast<PageId>(static_cast<uint64_t>(req.row_id) >> 32),
                            static_cast<SlotId>(req.row_id & 0xFFFF)};

                    auto data = table_storage->heap->get_tuple(rid);
                    if (!data) {
                        failed_row_ids.push_back(req.row_id);
                        continue;
                    }

                    auto values =
                        TupleSerializer::deserialize(*data, table_storage->storage_schema);
                    if (!values) {
                        failed_row_ids.push_back(req.row_id);
                        continue;
                    }

                    // Patch the embedding column.
                    for (size_t c = 0; c < table_schema->columns.size(); ++c) {
                        if (table_schema->columns[c].ordinal == req.column_id) {
                            (*values)[c] = Value(Embedding(std::move(req.embedding)));
                            break;
                        }
                    }

                    auto serialized =
                        TupleSerializer::serialize(*values, table_storage->storage_schema);
                    if (!serialized) {
                        failed_row_ids.push_back(req.row_id);
                        continue;
                    }

                    serialized_bufs.push_back(std::move(*serialized));
                    updates.push_back(TableHeap::TupleUpdate{
                        rid, std::span<const uint8_t>(serialized_bufs.back())});
                }

                // Batch update: one page pin per page instead of per tuple.
                auto batch_result = table_storage->heap->update_tuples_batch(updates);
                if (!batch_result) {
                    SIXSEVEN_LOG_WARN("batch store: update_tuples_batch failed: {}",
                                      batch_result.error().message);
                    for (auto idx : indices) {
                        failed_row_ids.push_back(requests[idx].row_id);
                    }
                    continue;
                }

                // Handle per-tuple failures: delete + reinsert on a
                // new page (tuple grew due to embedding and no longer
                // fits on the original page).
                for (auto& failed_rid : *batch_result) {
                    // Find the serialized buffer for this RID.
                    std::span<const uint8_t> new_data;
                    for (size_t u = 0; u < updates.size(); ++u) {
                        if (updates[u].rid.page_id == failed_rid.page_id &&
                            updates[u].rid.slot_id == failed_rid.slot_id) {
                            new_data = updates[u].data;
                            break;
                        }
                    }
                    if (new_data.empty()) {
                        int64_t packed =
                            (static_cast<int64_t>(static_cast<uint64_t>(failed_rid.page_id) << 32) |
                             failed_rid.slot_id);
                        failed_row_ids.push_back(packed);
                        continue;
                    }

                    // Delete old tuple and reinsert on a page with room.
                    auto del = table_storage->heap->delete_tuple(failed_rid);
                    if (!del) {
                        int64_t packed =
                            (static_cast<int64_t>(static_cast<uint64_t>(failed_rid.page_id) << 32) |
                             failed_rid.slot_id);
                        failed_row_ids.push_back(packed);
                        continue;
                    }
                    auto new_rid = table_storage->heap->insert_tuple(new_data);
                    if (!new_rid) {
                        int64_t packed =
                            (static_cast<int64_t>(static_cast<uint64_t>(failed_rid.page_id) << 32) |
                             failed_rid.slot_id);
                        failed_row_ids.push_back(packed);
                        continue;
                    }
                    // Successfully moved to a new page — not a failure.
                }

                // Insert embeddings into HNSW indexes.
                if (hnsw_indexes_ != nullptr) {
                    for (auto idx : indices) {
                        auto& req = requests[idx];
                        std::string col_name;
                        for (const auto& col : table_schema->columns) {
                            if (col.ordinal == req.column_id) {
                                col_name = col.name;
                                break;
                            }
                        }
                        if (col_name.empty())
                            continue;
                        // Find HNSW index by table_id + column name.
                        auto batch_indexes = catalog_.list_indexes(table_id);
                        for (const auto& bidx : batch_indexes) {
                            if (bidx.index_type != "hnsw" || bidx.columns != col_name)
                                continue;
                            auto hit = hnsw_indexes_->find(bidx.index_id);
                            if (hit == hnsw_indexes_->end())
                                break;
                            // req.embedding was moved into the Value earlier;
                            // re-read from the tuple on disk.
                            RID rid{static_cast<PageId>(static_cast<uint64_t>(req.row_id) >> 32),
                                    static_cast<SlotId>(req.row_id & 0xFFFF)};
                            auto data = table_storage->heap->get_tuple(rid);
                            if (!data)
                                break;
                            auto values =
                                TupleSerializer::deserialize(*data, table_storage->storage_schema);
                            if (!values)
                                break;
                            for (size_t c = 0; c < table_schema->columns.size(); ++c) {
                                if (table_schema->columns[c].ordinal == req.column_id &&
                                    !(*values)[c].is_null() &&
                                    (*values)[c].type_id() == TypeId::EMBEDDING) {
                                    const auto& vec = (*values)[c].as_embedding();
                                    auto ins = hit->second->insert(std::span<const float>(vec));
                                    if (ins && index_manager_) {
                                        index_manager_->append_hnsw_rid(bidx.index_id, rid);
                                    }
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }

            return ok(std::move(failed_row_ids));
        });
    }
}

void QueryEngine::set_algorithm_registry(AlgorithmRegistry* registry) {
    algorithm_registry_ = registry;
}

void QueryEngine::set_index_manager(IndexManager* mgr) {
    index_manager_ = mgr;
}

void QueryEngine::set_backfill_manager(BackfillManager* mgr) {
    backfill_manager_ = mgr;
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
                        dynamic_cast<const BackfillStmt*>(raw) != nullptr ||
                        dynamic_cast<const ReembedStmt*>(raw) != nullptr ||
                        dynamic_cast<const ReindexStmt*>(raw) != nullptr ||
                        dynamic_cast<const CreateUserStmt*>(raw) != nullptr ||
                        dynamic_cast<const DropUserStmt*>(raw) != nullptr ||
                        dynamic_cast<const AlterUserStmt*>(raw) != nullptr ||
                        dynamic_cast<const AlterTableStmt*>(raw) != nullptr ||
                        dynamic_cast<const CreateIndexStmt*>(raw) != nullptr ||
                        dynamic_cast<const DropIndexStmt*>(raw) != nullptr;

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
                          dynamic_cast<const AlterUserStmt*>(raw) != nullptr ||
                          dynamic_cast<const AlterTableStmt*>(raw) != nullptr ||
                          dynamic_cast<const CreateIndexStmt*>(raw) != nullptr ||
                          dynamic_cast<const DropIndexStmt*>(raw) != nullptr;

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
    // TCL (GDB-747): explicit transaction control. ROLLBACK TO <savepoint>
    // keeps its existing session-level handling and is not intercepted here.
    if (dynamic_cast<const BeginStmt*>(stmt_ptr->get()) != nullptr) {
        return execute_begin();
    }
    if (dynamic_cast<const CommitStmt*>(stmt_ptr->get()) != nullptr) {
        return execute_commit();
    }
    if (auto* rollback = dynamic_cast<const RollbackStmt*>(stmt_ptr->get());
        rollback != nullptr && rollback->savepoint.empty()) {
        return execute_rollback();
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
    if (auto* alter_table = dynamic_cast<const AlterTableStmt*>(stmt_ptr->get())) {
        return execute_alter_table(*alter_table);
    }
    if (auto* alter_user = dynamic_cast<const AlterUserStmt*>(stmt_ptr->get())) {
        return execute_alter_user(*alter_user);
    }
    if (auto* create_index = dynamic_cast<const CreateIndexStmt*>(stmt_ptr->get())) {
        return execute_create_index(*create_index);
    }
    if (auto* drop_index = dynamic_cast<const DropIndexStmt*>(stmt_ptr->get())) {
        return execute_drop_index(*drop_index);
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

            // Handle SELECT <constant_expr> without FROM (e.g. SELECT 1, SELECT 'hello',
            // SELECT -5 AS neg). Folds literal and unary-wrapped-literal expressions so
            // PostgreSQL clients (psqlODBC, etc.) can evaluate trivial constant queries
            // without requiring the full planner. (GDB-661)
            std::vector<ConstFoldResult> folded;
            folded.reserve(select->items.size());
            bool all_const = true;
            for (const auto& item : select->items) {
                if (item.expr == nullptr) {
                    all_const = false;
                    break;
                }
                auto r = try_fold_const_expr(*item.expr);
                if (!r) {
                    all_const = false;
                    break;
                }
                folded.push_back(std::move(*r));
            }
            if (all_const) {
                QueryResult qr;
                std::vector<Value> row;
                for (size_t i = 0; i < select->items.size(); ++i) {
                    const auto& item = select->items[i];
                    auto& f = folded[i];
                    qr.column_names.push_back(item.alias.empty() ? f.default_name : item.alias);
                    qr.column_types.push_back(f.type_id);
                    row.emplace_back(std::move(f.value));
                }
                qr.rows.push_back(std::move(row));
                return ok(std::move(qr));
            }
        }
    }

    // 5. Bind.
    Binder binder(catalog_, current_database_id_, algorithm_registry_);
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
    if (auto* bulk_link = dynamic_cast<const BulkLinkStmt*>(bound->stmt)) {
        return execute_bulk_link(*bulk_link, *bound);
    }
    if (auto* unlink = dynamic_cast<const UnlinkStmt*>(bound->stmt)) {
        return execute_unlink(*unlink, *bound);
    }

    // 6c. Dispatch admin commands after binding.
    if (auto* backfill = dynamic_cast<const BackfillStmt*>(bound->stmt)) {
        return execute_backfill(*backfill);
    }
    if (auto* reembed = dynamic_cast<const ReembedStmt*>(bound->stmt)) {
        return execute_reembed(*reembed);
    }
    if (auto* reindex = dynamic_cast<const ReindexStmt*>(bound->stmt)) {
        return execute_reindex(*reindex);
    }
    if (auto* vacuum = dynamic_cast<const VacuumStmt*>(bound->stmt)) {
        return execute_vacuum(*vacuum);
    }
    if (auto* analyze = dynamic_cast<const AnalyzeStmt*>(bound->stmt)) {
        return execute_analyze(*analyze);
    }

    // 7. Plan + Execute.
    return execute_plan(*bound);
}

// ---------------------------------------------------------------------------
// Describe (column metadata without execution)
// ---------------------------------------------------------------------------

Result<std::vector<ColumnDescription>> QueryEngine::describe(const std::string& sql) {
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

    // 3. Only SELECT statements produce result columns.
    if (dynamic_cast<const SelectStmt*>(stmt_ptr->get()) == nullptr) {
        return ok(std::vector<ColumnDescription>{});
    }

    // 4. Bind to resolve column names and types.
    Binder binder(catalog_, current_database_id_, algorithm_registry_);
    auto bound = binder.bind(**stmt_ptr);
    if (!bound) {
        return make_error(bound.error().code, bound.error().message);
    }

    // 5. Convert resolved columns to ColumnDescription.
    std::vector<ColumnDescription> columns;
    columns.reserve(bound->output_columns.size());
    for (const auto& col : bound->output_columns) {
        columns.push_back({col.column_name, col.type_id});
    }
    return ok(std::move(columns));
}

// ---------------------------------------------------------------------------
// Database-scoped overloads (thread-safe)
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute(const std::string& sql, database_id_t database_id) {
    auto saved = current_database_id_;
    current_database_id_ = database_id;
    auto result = execute(sql);
    current_database_id_ = saved;
    return result;
}

Result<std::vector<ColumnDescription>> QueryEngine::describe(const std::string& sql,
                                                             database_id_t database_id) {
    auto saved = current_database_id_;
    current_database_id_ = database_id;
    auto result = describe(sql);
    current_database_id_ = saved;
    return result;
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

    // Persist to system catalog tables.
    if (catalog_persistence_ != nullptr) {
        auto persist = catalog_persistence_->persist_database(*db_id, stmt.database_name);
        if (!persist) {
            SIXSEVEN_LOG_WARN(
                "failed to persist database '{}': {}", stmt.database_name, persist.error().message);
        }
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

    // If cascade, clean up graph engine edge types and drop table storage.
    if (stmt.cascade) {
        auto tables = catalog_.list_tables(db_id);
        for (const auto& table : tables) {
            if (graph_engine_) {
                auto drop_edges = graph_engine_->drop_edge_types_for_table(db_id, table.table_id);
                if (!drop_edges) {
                    SIXSEVEN_LOG_WARN("failed to clean up edge types for table {}: {}",
                                      table.table_id,
                                      drop_edges.error().message);
                }
            }
            auto drop_storage = storage_.drop_table_storage(db_id, table.table_id);
            if (!drop_storage) {
                return make_error(drop_storage.error().code, drop_storage.error().message);
            }

            if (catalog_persistence_ != nullptr) {
                auto remove = catalog_persistence_->remove_table(table.table_id);
                if (!remove) {
                    SIXSEVEN_LOG_WARN("failed to remove table '{}' from persistence: {}",
                                      table.name,
                                      remove.error().message);
                }
            }
        }
    }

    auto result = catalog_.drop_database(db_id, stmt.cascade);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    // Remove from persistence.
    if (catalog_persistence_ != nullptr) {
        auto remove = catalog_persistence_->remove_database(db_id);
        if (!remove) {
            SIXSEVEN_LOG_WARN("failed to remove database '{}' from persistence: {}",
                              stmt.database_name,
                              remove.error().message);
        }
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
        case LiteralKind::STRING: {
            // Escape embedded single quotes by doubling them so the output
            // can be re-lexed as a valid SQL string literal.
            std::string escaped;
            escaped.reserve(lit->value.size());
            for (char c : lit->value) {
                if (c == '\'')
                    escaped += "''";
                else
                    escaped += c;
            }
            return "'" + escaped + "'";
        }
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

    // Also detect inline column-level PRIMARY KEY declarations.
    if (ts.pk_columns.empty()) {
        for (const auto& col : stmt.columns) {
            if (col.is_primary_key) {
                ts.pk_columns = col.name;
                break;
            }
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
            SIXSEVEN_LOG_WARN(
                "failed to persist table '{}': {}", stmt.name, persist.error().message);
        }
    }

    // Register EMBEDDING column metadata and auto-create HNSW indexes.
    std::vector<EmbeddingColumnDef> emb_defs;
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const auto& col = stmt.columns[i];
        if (!col.type.source.empty()) { // EMBEDDING column
            EmbeddingColumnDef def;
            def.table_id = *table_id;
            def.column_id = static_cast<int32_t>(i);
            def.dimension = col.type.param1.value_or(0);
            def.source_expr = col.type.source;
            def.provider = col.type.provider;
            emb_defs.push_back(std::move(def));
        }
    }
    if (!emb_defs.empty()) {
        EmbeddingColumnManager emb_mgr(catalog_);
        auto reg = emb_mgr.register_table_embeddings(*table_id, emb_defs);
        if (!reg) {
            return make_error(reg.error().code, reg.error().message);
        }
        // Persist each embedding column to sys_embedding_columns.
        if (catalog_persistence_ != nullptr) {
            for (const auto& def : emb_defs) {
                auto p = catalog_persistence_->persist_embedding_column(def);
                if (!p) {
                    SIXSEVEN_LOG_WARN("failed to persist embedding column: {}", p.error().message);
                }
            }
        }

        // Persist HNSW indexes to sys_indexes and instantiate them.
        {
            auto table_schema = catalog_.get_table_by_id(*table_id);
            if (table_schema) {
                auto hnsw_defs = catalog_.list_indexes(*table_id);
                for (const auto& idx : hnsw_defs) {
                    if (idx.index_type == "hnsw") {
                        if (catalog_persistence_ != nullptr) {
                            auto p = catalog_persistence_->persist_index(idx);
                            if (!p) {
                                SIXSEVEN_LOG_WARN("failed to persist HNSW index '{}': {}",
                                                  idx.name,
                                                  p.error().message);
                            }
                        }
                        if (index_manager_ != nullptr) {
                            auto r = index_manager_->create_and_populate_index(idx, *table_schema);
                            if (!r) {
                                SIXSEVEN_LOG_WARN("failed to create HNSW index '{}': {}",
                                                  idx.name,
                                                  r.error().message);
                            }
                        }
                    }
                }
            }
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

    // Clean up graph engine edge types that reference this table (CASCADE).
    if (graph_engine_) {
        auto drop_edges = graph_engine_->drop_edge_types_for_table(current_database_id_, table_id);
        if (!drop_edges) {
            SIXSEVEN_LOG_WARN("failed to clean up edge types for table '{}': {}",
                              stmt.name,
                              drop_edges.error().message);
        }
    }

    // Drop storage first (flush + close + delete file).
    auto drop_storage = storage_.drop_table_storage(current_database_id_, table_id);
    if (!drop_storage) {
        return make_error(drop_storage.error().code, drop_storage.error().message);
    }

    // Remove from persistence before dropping from catalog.
    if (catalog_persistence_ != nullptr) {
        auto remove = catalog_persistence_->remove_table(table_id);
        if (!remove) {
            SIXSEVEN_LOG_WARN("failed to remove table '{}' from persistence: {}",
                              stmt.name,
                              remove.error().message);
        }
    }

    // Remove from catalog.
    auto drop_catalog = catalog_.drop_table(current_database_id_, stmt.name);
    if (!drop_catalog) {
        return make_error(drop_catalog.error().code, drop_catalog.error().message);
    }

    // Drop any ANALYZE statistics gathered for the table.
    statistics_store_.remove_table(table_id);

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

    auto result = graph_engine_->create_edge_type(current_database_id_,
                                                  stmt.name,
                                                  from_schema->table_id,
                                                  to_schema->table_id,
                                                  from_pk_type,
                                                  to_pk_type,
                                                  prop_cols);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    // Persist edge type metadata to sys_edge_types.
    if (catalog_persistence_ != nullptr) {
        auto et = catalog_.get_edge_type(current_database_id_, stmt.name);
        if (et) {
            auto persist = catalog_persistence_->persist_edge_type(*et);
            if (!persist) {
                SIXSEVEN_LOG_WARN(
                    "failed to persist edge type '{}': {}", stmt.name, persist.error().message);
            }
        }
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

    // Look up edge_id before dropping (needed for persistence removal).
    edge_id_t edge_id = 0;
    auto et = catalog_.get_edge_type(current_database_id_, stmt.name);
    if (et) {
        edge_id = et->edge_id;
    }

    auto result = graph_engine_->drop_edge_type(current_database_id_, stmt.name);
    if (!result) {
        if (stmt.if_exists && result.error().code == StatusCode::NOT_FOUND) {
            QueryResult qr;
            qr.message = "DROP EDGE TYPE";
            return ok(std::move(qr));
        }
        return make_error(result.error().code, result.error().message);
    }

    // Remove edge type metadata from sys_edge_types.
    if (catalog_persistence_ != nullptr && edge_id != 0) {
        auto persist = catalog_persistence_->remove_edge_type(edge_id);
        if (!persist) {
            SIXSEVEN_LOG_WARN("failed to remove persisted edge type '{}': {}",
                              stmt.name,
                              persist.error().message);
        }
    }

    QueryResult qr;
    qr.message = "DROP EDGE TYPE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// LINK / UNLINK key coercion
// ---------------------------------------------------------------------------

Result<std::pair<Value, Value>> QueryEngine::coerce_link_keys(const std::string& edge_type,
                                                              const Value& src_key,
                                                              const Value& tgt_key) {
    auto edge = catalog_.get_edge_type(current_database_id_, edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }

    auto src_schema = catalog_.get_table_by_id(edge->source_table_id);
    if (!src_schema) {
        return tl::unexpected(src_schema.error());
    }

    auto tgt_schema = catalog_.get_table_by_id(edge->target_table_id);
    if (!tgt_schema) {
        return tl::unexpected(tgt_schema.error());
    }

    // Resolve PK type from schema.
    auto resolve_pk_type = [](const TableSchema& schema) {
        TypeId pk = TypeId::INT64;
        if (!schema.pk_columns.empty()) {
            for (const auto& col : schema.columns) {
                if (col.name == schema.pk_columns) {
                    pk = col.type_id;
                    break;
                }
            }
        }
        return pk;
    };

    TypeId src_pk_type = resolve_pk_type(*src_schema);
    TypeId tgt_pk_type = resolve_pk_type(*tgt_schema);

    auto coerced_src = fit_to_storage(src_key, src_pk_type);
    if (!coerced_src) {
        return make_error(coerced_src.error().code,
                          "LINK source key: " + coerced_src.error().message);
    }

    auto coerced_tgt = fit_to_storage(tgt_key, tgt_pk_type);
    if (!coerced_tgt) {
        return make_error(coerced_tgt.error().code,
                          "LINK target key: " + coerced_tgt.error().message);
    }

    return ok(std::make_pair(std::move(*coerced_src), std::move(*coerced_tgt)));
}

// ---------------------------------------------------------------------------
// LINK / UNLINK PK existence check (with hash set cache)
// ---------------------------------------------------------------------------

Result<void> QueryEngine::ensure_pk_cache(table_id_t table_id) {
    if (pk_cache_.contains(table_id)) {
        return ok();
    }

    auto ts = storage_.get_table_storage(table_id);
    if (!ts) {
        return tl::unexpected(ts.error());
    }
    auto* table_storage = *ts;

    auto schema = catalog_.get_table_by_id(table_id);
    if (!schema) {
        return tl::unexpected(schema.error());
    }

    // Find PK column index.
    size_t pk_col_idx = 0;
    bool pk_found = false;
    for (size_t i = 0; i < schema->columns.size(); ++i) {
        if (schema->columns[i].name == schema->pk_columns) {
            pk_col_idx = i;
            pk_found = true;
            break;
        }
    }
    if (!pk_found) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "PK column '" + schema->pk_columns +
                              "' not found in table schema for PK cache");
    }

    std::unordered_set<std::string> pk_set;
    auto iter = table_storage->heap->begin();
    if (!iter) {
        return tl::unexpected(iter.error());
    }

    while (true) {
        auto row = iter->next();
        if (!row) {
            break;
        }
        auto [rid, data] = *row;
        auto values = TupleSerializer::deserialize(data, table_storage->storage_schema);
        if (!values || pk_col_idx >= values->size()) {
            continue;
        }
        const auto& pk_val = (*values)[pk_col_idx];
        // Defensive: skip rows whose deserialized PK is NULL. This should never
        // happen for a healthy PK column, but a corrupt/half-written tail in the
        // table heap can produce NULLs after a crash recovery. Skipping is safer
        // than indexing them — a NULL PK in the cache would either crash older
        // callers or produce false-positive existence checks.
        if (pk_val.is_null()) {
            SIXSEVEN_LOG_WARN(
                "skipping row with NULL PK while building pk_cache for table {} (rid={}:{})",
                table_id,
                rid.page_id,
                rid.slot_id);
            continue;
        }
        pk_set.insert(pk_value_to_string(pk_val));
    }

    pk_cache_[table_id] = std::move(pk_set);
    return ok();
}

void QueryEngine::invalidate_pk_cache(table_id_t table_id) {
    pk_cache_.erase(table_id);
}

Result<bool> QueryEngine::verify_pk_exists(table_id_t table_id, const Value& pk_value) {
    auto cache_result = ensure_pk_cache(table_id);
    if (!cache_result) {
        return tl::unexpected(cache_result.error());
    }

    auto it = pk_cache_.find(table_id);
    if (it == pk_cache_.end()) {
        return ok(false);
    }

    return ok(it->second.contains(pk_value_to_string(pk_value)));
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

    // Coerce key values to match the PK types of the source/target tables.
    auto coerced = coerce_link_keys(stmt.edge_type, *src_key, *tgt_key);
    if (!coerced) {
        return make_error(coerced.error().code, coerced.error().message);
    }
    auto& [coerced_src, coerced_tgt] = *coerced;

    // Verify source and target rows exist.
    auto edge = catalog_.get_edge_type(current_database_id_, stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }

    auto src_exists = verify_pk_exists(edge->source_table_id, coerced_src);
    if (!src_exists) {
        return make_error(src_exists.error().code, src_exists.error().message);
    }
    if (!*src_exists) {
        return make_error(StatusCode::NOT_FOUND,
                          "LINK source row not found in '" + stmt.source_table + "'");
    }

    auto tgt_exists = verify_pk_exists(edge->target_table_id, coerced_tgt);
    if (!tgt_exists) {
        return make_error(tgt_exists.error().code, tgt_exists.error().message);
    }
    if (!*tgt_exists) {
        return make_error(StatusCode::NOT_FOUND,
                          "LINK target row not found in '" + stmt.target_table + "'");
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

    auto result =
        graph_engine_->link(current_database_id_, stmt.edge_type, coerced_src, coerced_tgt, props);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.affected_rows = 1;
    qr.message = "LINK";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// BULK LINK
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_bulk_link(const BulkLinkStmt& stmt,
                                                   const BoundStatement& bound) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for LINK");
    }
    if (stmt.rows.empty()) {
        QueryResult qr;
        qr.affected_rows = 0;
        qr.message = "LINK";
        return ok(std::move(qr));
    }

    // Resolve edge type for PK coercion and verification.
    auto edge = catalog_.get_edge_type(current_database_id_, stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }

    Tuple empty_tuple;
    OutputSchema empty_schema;

    // Evaluate all row expressions and build EdgeInsertRequests.
    std::vector<EdgeInsertRequest> requests;
    requests.reserve(stmt.rows.size());

    // Track unique PKs for batch verification (avoid redundant lookups
    // when the same user links to many targets, for example).
    std::unordered_set<std::string> seen_src_pks;
    std::unordered_set<std::string> seen_tgt_pks;

    for (size_t i = 0; i < stmt.rows.size(); ++i) {
        const auto& row = stmt.rows[i];

        // First two values: source_key, target_key.
        auto src_val = evaluate_expr(*row[0], empty_tuple, empty_schema, bound);
        if (!src_val) {
            return make_error(src_val.error().code,
                              "row " + std::to_string(i + 1) +
                                  " source_key: " + src_val.error().message);
        }
        auto tgt_val = evaluate_expr(*row[1], empty_tuple, empty_schema, bound);
        if (!tgt_val) {
            return make_error(tgt_val.error().code,
                              "row " + std::to_string(i + 1) +
                                  " target_key: " + tgt_val.error().message);
        }

        // Coerce keys.
        auto coerced = coerce_link_keys(stmt.edge_type, *src_val, *tgt_val);
        if (!coerced) {
            return make_error(coerced.error().code,
                              "row " + std::to_string(i + 1) + ": " + coerced.error().message);
        }
        auto& [coerced_src, coerced_tgt] = *coerced;

        // Track for dedup'd PK verification.
        seen_src_pks.insert(pk_value_to_string(coerced_src));
        seen_tgt_pks.insert(pk_value_to_string(coerced_tgt));

        // Remaining values are properties (positional order).
        std::vector<Value> props;
        props.reserve(row.size() - 2);
        for (size_t j = 2; j < row.size(); ++j) {
            auto pval = evaluate_expr(*row[j], empty_tuple, empty_schema, bound);
            if (!pval) {
                return make_error(pval.error().code,
                                  "row " + std::to_string(i + 1) + " property " +
                                      std::to_string(j - 1) + ": " + pval.error().message);
            }
            props.push_back(std::move(*pval));
        }

        requests.push_back(
            EdgeInsertRequest{std::move(coerced_src), std::move(coerced_tgt), std::move(props)});
    }

    // Batch PK verification (outside graph lock). For each unique source/
    // target PK, verify the row exists. This is a read-only table lookup.
    for (const auto& req : requests) {
        // Only verify if we haven't verified this exact PK yet.
        auto src_str = pk_value_to_string(req.source_pk);
        if (seen_src_pks.count(src_str)) {
            auto src_exists = verify_pk_exists(edge->source_table_id, req.source_pk);
            if (!src_exists) {
                return make_error(src_exists.error().code, src_exists.error().message);
            }
            if (!*src_exists) {
                return make_error(StatusCode::NOT_FOUND,
                                  "LINK source row not found in '" + stmt.source_table +
                                      "' for key " + src_str);
            }
            seen_src_pks.erase(src_str);
        }
        auto tgt_str = pk_value_to_string(req.target_pk);
        if (seen_tgt_pks.count(tgt_str)) {
            auto tgt_exists = verify_pk_exists(edge->target_table_id, req.target_pk);
            if (!tgt_exists) {
                return make_error(tgt_exists.error().code, tgt_exists.error().message);
            }
            if (!*tgt_exists) {
                return make_error(StatusCode::NOT_FOUND,
                                  "LINK target row not found in '" + stmt.target_table +
                                      "' for key " + tgt_str);
            }
            seen_tgt_pks.erase(tgt_str);
        }
    }

    // Single-lock batch insert.
    auto result = graph_engine_->link_batch(current_database_id_, stmt.edge_type, requests);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.affected_rows = *result;
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

    // Coerce key values to match the PK types of the source/target tables.
    auto coerced = coerce_link_keys(stmt.edge_type, *src_key, *tgt_key);
    if (!coerced) {
        return make_error(coerced.error().code, coerced.error().message);
    }
    auto& [coerced_src, coerced_tgt] = *coerced;

    // Verify source and target rows exist.
    auto edge = catalog_.get_edge_type(current_database_id_, stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }

    auto src_exists = verify_pk_exists(edge->source_table_id, coerced_src);
    if (!src_exists) {
        return make_error(src_exists.error().code, src_exists.error().message);
    }
    if (!*src_exists) {
        return make_error(StatusCode::NOT_FOUND,
                          "UNLINK source row not found in '" + stmt.source_table + "'");
    }

    auto tgt_exists = verify_pk_exists(edge->target_table_id, coerced_tgt);
    if (!tgt_exists) {
        return make_error(tgt_exists.error().code, tgt_exists.error().message);
    }
    if (!*tgt_exists) {
        return make_error(StatusCode::NOT_FOUND,
                          "UNLINK target row not found in '" + stmt.target_table + "'");
    }

    if (stmt.where_expr) {
        // Build an OutputSchema from edge property columns so evaluate_predicate
        // can resolve bare column names in the WHERE clause.
        auto edge_table = graph_engine_->get_edge_table(current_database_id_, stmt.edge_type);
        if (!edge_table) {
            return make_error(edge_table.error().code, edge_table.error().message);
        }
        std::vector<OutputColumn> out_cols;
        for (const auto& pc : (*edge_table)->config().property_columns) {
            out_cols.push_back({stmt.edge_type, pc.name, pc.type, true, 0});
        }
        OutputSchema edge_schema(std::move(out_cols));

        // Capture first predicate error so we can propagate it after unlink_where
        // returns (the predicate lambda can only return bool).
        Error predicate_error{StatusCode::OK, ""};
        bool has_predicate_error = false;

        auto result = graph_engine_->unlink_where(
            current_database_id_,
            stmt.edge_type,
            coerced_src,
            coerced_tgt,
            [&](const EdgeRow& row) -> bool {
                if (has_predicate_error) {
                    return false; // skip remaining edges after first error
                }
                Tuple edge_tuple;
                edge_tuple.values = row.properties;
                auto pred = evaluate_predicate(*stmt.where_expr, edge_tuple, edge_schema, bound);
                if (!pred) {
                    predicate_error = pred.error();
                    has_predicate_error = true;
                    return false;
                }
                return *pred;
            });
        if (has_predicate_error) {
            return make_error(predicate_error.code, predicate_error.message);
        }
        if (!result) {
            return make_error(result.error().code, result.error().message);
        }

        QueryResult qr;
        qr.affected_rows = static_cast<int64_t>(*result);
        qr.message = "UNLINK";
        return ok(std::move(qr));
    }

    auto result =
        graph_engine_->unlink(current_database_id_, stmt.edge_type, coerced_src, coerced_tgt);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.affected_rows = 1;
    qr.message = "UNLINK";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// REINDEX
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_reindex(const ReindexStmt& stmt) {
    if (index_manager_ == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "no index manager available");
    }

    auto r = index_manager_->reindex(stmt.name, current_database_id_);
    if (!r) {
        return make_error(r.error().code, r.error().message);
    }

    SIXSEVEN_LOG_INFO("reindexed '{}'", stmt.name);

    QueryResult qr;
    qr.message = "REINDEX";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// VACUUM
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_vacuum(const VacuumStmt& stmt) {
    // Resolve the set of tables to vacuum: the named table, or every user
    // table in the current database when no name is given (bare VACUUM).
    std::vector<TableSchema> targets;
    if (!stmt.table_name.empty()) {
        auto schema = catalog_.get_table(current_database_id_, stmt.table_name);
        if (!schema) {
            return make_error(schema.error().code, schema.error().message);
        }
        targets.push_back(std::move(*schema));
    } else {
        targets = catalog_.list_tables(current_database_id_);
    }

    // Validated no-op (GDB-1230): txn::Vacuum must not run over the executor's
    // heap pages yet. Tuples now carry MVCC headers (GDB-714), but there is
    // still no shared TransactionManager to supply a vacuum horizon — the
    // executor stamps frozen_txn_id, and a fresh manager resolves real txn
    // ids as ABORTED, so running Vacuum could still reclaim live rows.
    // Targets are resolved above so VACUUM keeps PostgreSQL-compatible
    // error behavior.
    SIXSEVEN_LOG_INFO("VACUUM: validated {} table(s); reclamation deferred until MVCC "
                      "integration (GDB-1230)",
                      targets.size());

    QueryResult qr;
    qr.message = "VACUUM";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// ANALYZE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_analyze(const AnalyzeStmt& stmt) {
    // Resolve the set of tables to analyze: the named table, or every user
    // table in the current database when no name is given (bare ANALYZE).
    std::vector<TableSchema> targets;
    if (!stmt.table_name.empty()) {
        auto schema = catalog_.get_table(current_database_id_, stmt.table_name);
        if (!schema) {
            return make_error(schema.error().code, schema.error().message);
        }
        targets.push_back(std::move(*schema));
    } else {
        targets = catalog_.list_tables(current_database_id_);
    }

    uint32_t tables_analyzed = 0;

    for (const auto& schema : targets) {
        // Ensure the table's storage is open before analyzing.
        auto ts = storage_.get_table_storage(schema.table_id);
        if (!ts) {
            if (storage_.table_file_exists(current_database_id_, schema.table_id)) {
                auto opened =
                    storage_.open_table_storage(current_database_id_, schema.table_id, schema);
                if (!opened) {
                    return make_error(opened.error().code, opened.error().message);
                }
                ts = storage_.get_table_storage(schema.table_id);
            }
            if (!ts) {
                return make_error(ts.error().code, ts.error().message);
            }
        }
        auto* table_storage = *ts;

        auto analyzed = analyze_table(schema.table_id,
                                      schema,
                                      *table_storage->heap,
                                      table_storage->storage_schema,
                                      statistics_store_);
        if (!analyzed) {
            return make_error(analyzed.error().code, analyzed.error().message);
        }
        ++tables_analyzed;
    }

    SIXSEVEN_LOG_INFO("analyzed {} table(s)", tables_analyzed);

    QueryResult qr;
    qr.message = "ANALYZE";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// BACKFILL EMBEDDINGS
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_backfill(const BackfillStmt& stmt) {
    if (!backfill_manager_) {
        return make_error(StatusCode::INTERNAL_ERROR, "backfill manager not initialized");
    }

    auto result = backfill_manager_->start(stmt, current_database_id_);
    if (!result) {
        return make_error(result.error().code, result.error().message);
    }

    QueryResult qr;
    qr.message = "BACKFILL started for table " + stmt.table_name;
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
            // Extract source texts from each row, skipping rows where source_expr
            // resolves no schema columns (misconfigured EMBEDDING definition).
            std::vector<std::string> source_texts;
            std::vector<size_t> active_indices;
            source_texts.reserve(batch.size());
            active_indices.reserve(batch.size());

            for (size_t row_i = 0; row_i < batch.size(); ++row_i) {
                const auto& row = batch[row_i];
                auto src = EmbeddingColumnManager::build_source_text(
                    target.def.source_expr, table_schema->columns, row.values);
                if (src.resolved_count == 0) {
                    SIXSEVEN_LOG_ERROR(
                        "reembed: source_expr '{}' for table '{}' column_id={} resolved no "
                        "schema columns - skipping row (check EMBEDDING column definition)",
                        target.def.source_expr,
                        stmt.table_name,
                        target.def.column_id);
                    ++total_skipped;
                    continue;
                }
                active_indices.push_back(row_i);
                source_texts.push_back(std::move(src.text));
            }

            if (source_texts.empty()) {
                continue;
            }

            // Batch embed via the provider.
            auto embeddings = target.provider->embed_batch(source_texts);
            if (!embeddings) {
                SIXSEVEN_LOG_WARN("REEMBED: batch embed failed for provider '{}': {}",
                                  target.def.provider,
                                  embeddings.error().message);
                total_skipped += static_cast<int64_t>(active_indices.size());
                continue;
            }

            // Update each active row's embedding and persist.
            for (size_t i = 0; i < active_indices.size(); ++i) {
                auto& row = batch[active_indices[i]];

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
                SIXSEVEN_LOG_INFO("REEMBED: processed {} rows", total_processed);
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
            // Find HNSW index for this column by index_id.
            auto reembed_col_name = table_schema->columns[target.column_index].name;
            auto reembed_indexes = catalog_.list_indexes(table_schema->table_id);
            index_id_t reembed_idx_id = 0;
            for (const auto& ridx : reembed_indexes) {
                if (ridx.index_type == "hnsw" && ridx.columns == reembed_col_name) {
                    reembed_idx_id = ridx.index_id;
                    break;
                }
            }
            if (reembed_idx_id == 0)
                continue;
            auto idx_it = hnsw_indexes_->find(reembed_idx_id);
            if (idx_it == hnsw_indexes_->end()) {
                continue;
            }
            auto* hnsw = idx_it->second;

            // Reset the index so new inserts start from node_id 0.
            auto reset_result = hnsw->reset();
            if (!reset_result) {
                SIXSEVEN_LOG_WARN("REEMBED: failed to reset HNSW index (id={}): {}",
                                  reembed_idx_id,
                                  reset_result.error().message);
                continue;
            }

            // Re-scan the table and insert all embeddings in row order.
            // Also rebuild the node_id → RID map.
            auto rebuild_it = table_storage->heap->begin();
            if (!rebuild_it) {
                SIXSEVEN_LOG_WARN("REEMBED: failed to scan table for HNSW rebuild: {}",
                                  rebuild_it.error().message);
                continue;
            }

            std::vector<RID> rid_map;
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
                        rid_map.push_back(rid);
                        ++inserted;
                    }
                }
            }

            if (index_manager_) {
                auto* maps = index_manager_->hnsw_rid_maps();
                if (maps) {
                    (*maps)[reembed_idx_id] = std::move(rid_map);
                }
            }

            SIXSEVEN_LOG_INFO(
                "REEMBED: rebuilt HNSW index (id={}) with {} vectors", reembed_idx_id, inserted);
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);

    SIXSEVEN_LOG_INFO("REEMBED: completed {} rows in {}ms ({} skipped)",
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
    Planner planner(catalog_,
                    storage_,
                    current_database_id_,
                    graph_engine_,
                    provider_registry_,
                    hnsw_indexes_,
                    index_manager_ ? index_manager_->btree_map() : nullptr,
                    index_manager_ ? index_manager_->hash_map() : nullptr,
                    embedding_pool_,
                    algorithm_registry_,
                    index_manager_ ? index_manager_->hnsw_rid_maps() : nullptr,
                    index_manager_ ? index_manager_->bm25_map() : nullptr);
    planner.set_statistics(&statistics_store_);
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
            if (StatementDeadline::expired()) {
                (*iter)->close();
                return make_error(StatusCode::QUERY_CANCELED,
                                  "canceling statement due to statement timeout");
            }
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
// Transaction control (GDB-747)
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_begin() {
    QueryResult qr;
    qr.message = "BEGIN";
    if (active_txn_id_ != invalid_txn_id) {
        // PostgreSQL warns and keeps the current transaction.
        SIXSEVEN_LOG_WARN("BEGIN: there is already a transaction in progress (txn {})",
                          active_txn_id_);
        return ok(std::move(qr));
    }
    auto txn = txn_mgr_.begin();
    if (!txn) {
        return make_error(txn.error().code, txn.error().message);
    }
    active_txn_id_ = (*txn)->txn_id;
    active_txn_row_deltas_.clear();
    return ok(std::move(qr));
}

Result<QueryResult> QueryEngine::execute_commit() {
    QueryResult qr;
    qr.message = "COMMIT";
    if (active_txn_id_ == invalid_txn_id) {
        // PostgreSQL warns; committing outside a transaction is a no-op.
        return ok(std::move(qr));
    }
    auto committed = txn_mgr_.commit(active_txn_id_);
    active_txn_id_ = invalid_txn_id;
    active_txn_row_deltas_.clear();
    if (!committed) {
        return make_error(committed.error().code, committed.error().message);
    }
    return ok(std::move(qr));
}

Result<QueryResult> QueryEngine::execute_rollback() {
    QueryResult qr;
    qr.message = "ROLLBACK";
    if (active_txn_id_ == invalid_txn_id) {
        return ok(std::move(qr));
    }
    auto aborted = txn_mgr_.abort(active_txn_id_);
    if (!aborted) {
        SIXSEVEN_LOG_WARN(
            "ROLLBACK: abort of txn {} failed: {}", active_txn_id_, aborted.error().message);
    }
    // Compensate live-row counters: the transaction's logical inserts and
    // deletes moved the per-heap counters, but its versions are now invisible.
    for (auto& [heap, delta] : active_txn_row_deltas_) {
        if (heap != nullptr && delta != 0) {
            heap->adjust_row_count(-delta);
        }
    }
    active_txn_id_ = invalid_txn_id;
    active_txn_row_deltas_.clear();
    return ok(std::move(qr));
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
                            SIXSEVEN_LOG_WARN("failed to unset existing default provider: {}",
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
                                    SIXSEVEN_LOG_WARN(
                                        "failed to unset existing default provider: {}",
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
    Planner planner(catalog_,
                    storage_,
                    current_database_id_,
                    graph_engine_,
                    provider_registry_,
                    hnsw_indexes_,
                    index_manager_ ? index_manager_->btree_map() : nullptr,
                    index_manager_ ? index_manager_->hash_map() : nullptr,
                    embedding_pool_,
                    algorithm_registry_,
                    index_manager_ ? index_manager_->hnsw_rid_maps() : nullptr,
                    index_manager_ ? index_manager_->bm25_map() : nullptr);
    planner.set_statistics(&statistics_store_);
    std::vector<ExprPtr> owned_exprs;
    auto iter = planner.plan(bound, owned_exprs);
    if (!iter) {
        return make_error(iter.error().code, iter.error().message);
    }

    // Transaction stamping (GDB-747): DML statements run under the explicit
    // transaction opened by BEGIN, or under an implicit single-statement
    // transaction (begin → execute → commit on success / abort on failure).
    txn_id_t stmt_txn_id = invalid_txn_id;
    bool implicit_txn = false;
    TableHeap* dml_heap = nullptr;
    int64_t dml_row_delta_sign = 0; // +1 INSERT, -1 DELETE, 0 UPDATE/queries.

    InsertOperator* insert_op = dynamic_cast<InsertOperator*>(iter->get());
    UpdateOperator* update_op = dynamic_cast<UpdateOperator*>(iter->get());
    DeleteOperator* delete_op = dynamic_cast<DeleteOperator*>(iter->get());
    if (insert_op != nullptr || update_op != nullptr || delete_op != nullptr) {
        if (active_txn_id_ != invalid_txn_id) {
            stmt_txn_id = active_txn_id_;
        } else {
            auto txn = txn_mgr_.begin();
            if (!txn) {
                return make_error(txn.error().code, txn.error().message);
            }
            stmt_txn_id = (*txn)->txn_id;
            implicit_txn = true;
        }
        if (insert_op != nullptr) {
            insert_op->set_txn_id(stmt_txn_id);
            dml_heap = &insert_op->target_heap();
            dml_row_delta_sign = 1;
        } else if (update_op != nullptr) {
            update_op->set_txn_id(stmt_txn_id);
            dml_heap = &update_op->target_heap();
        } else {
            delete_op->set_txn_id(stmt_txn_id);
            dml_heap = &delete_op->target_heap();
            dml_row_delta_sign = -1;
        }
    }

    // Abort the implicit statement transaction when execution fails so its
    // partial writes become invisible (xmin aborted / xmax aborted).
    auto abort_implicit_txn = [&]() {
        if (implicit_txn) {
            auto aborted = txn_mgr_.abort(stmt_txn_id);
            if (!aborted) {
                SIXSEVEN_LOG_WARN(
                    "abort of implicit txn {} failed: {}", stmt_txn_id, aborted.error().message);
            }
        }
    };

    // Snapshot read view (GDB-777): every statement reads under a snapshot.
    // Inside a transaction the viewer is that transaction (it sees its own
    // uncommitted changes via is_visible's self-visibility rule); autocommit
    // SELECTs read under a fresh snapshot with no viewer, so uncommitted
    // changes from in-flight transactions are invisible. TableHeap reads and
    // the scan operators consult this thread-local view.
    const txn_id_t viewer_txn_id = stmt_txn_id != invalid_txn_id ? stmt_txn_id : active_txn_id_;
    MvccReadViewGuard read_view_guard(MvccReadView{
        viewer_txn_id != invalid_txn_id ? txn_mgr_.get_statement_snapshot(viewer_txn_id)
                                        : txn_mgr_.take_snapshot(),
        viewer_txn_id});

    // Open.
    auto open_result = (*iter)->open();
    if (!open_result) {
        abort_implicit_txn();
        return make_error(open_result.error().code, open_result.error().message);
    }

    // Build column metadata from the output schema.
    const auto& schema = (*iter)->output_schema();
    QueryResult qr;
    for (size_t i = 0; i < schema.column_count(); ++i) {
        qr.column_names.push_back(schema.column(i).name);
        qr.column_types.push_back(schema.column(i).type_id);
    }

    // Drain. Between tuple pulls, enforce the session statement_timeout
    // deadline armed by the protocol layer (GDB-721).
    while (true) {
        if (StatementDeadline::expired()) {
            (*iter)->close();
            abort_implicit_txn();
            return make_error(StatusCode::QUERY_CANCELED,
                              "canceling statement due to statement timeout");
        }
        auto row = (*iter)->next();
        if (!row) {
            (*iter)->close();
            abort_implicit_txn();
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        qr.rows.push_back(std::move(row->value().values));
    }

    // Close.
    (*iter)->close();

    // Finish the statement transaction (GDB-747).
    if (stmt_txn_id != invalid_txn_id) {
        if (implicit_txn) {
            auto committed = txn_mgr_.commit(stmt_txn_id);
            if (!committed) {
                auto aborted = txn_mgr_.abort(stmt_txn_id);
                if (!aborted) {
                    SIXSEVEN_LOG_WARN("abort after failed commit of txn {} failed: {}",
                                      stmt_txn_id,
                                      aborted.error().message);
                }
                return make_error(committed.error().code, committed.error().message);
            }
        } else if (dml_heap != nullptr && dml_row_delta_sign != 0 && qr.rows.size() == 1 &&
                   !qr.rows[0].empty()) {
            // Explicit transaction: remember the live-row-count delta so
            // ROLLBACK can compensate the heap counter.
            active_txn_row_deltas_[dml_heap] += dml_row_delta_sign * qr.rows[0][0].as_int64();
        }
    }

    // Detect DML results (single "count" column).
    if (qr.column_names.size() == 1 && qr.column_names[0] == "count" && qr.rows.size() == 1) {
        qr.affected_rows = qr.rows[0][0].as_int64();
        qr.rows.clear();
        qr.column_names.clear();
        qr.column_types.clear();

        // Invalidate PK cache for the affected table after INSERT/DELETE/UPDATE.
        if (auto* ins = dynamic_cast<const InsertStmt*>(bound.stmt)) {
            auto schema = catalog_.get_table(current_database_id_, ins->table_name);
            if (schema) {
                invalidate_pk_cache(schema->table_id);
            }
        } else if (auto* del = dynamic_cast<const DeleteStmt*>(bound.stmt)) {
            auto schema = catalog_.get_table(current_database_id_, del->table_name);
            if (schema) {
                invalidate_pk_cache(schema->table_id);
            }
        }
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
                    SIXSEVEN_LOG_ERROR("failed to restore in-use provider '{}': {}",
                                       pp.name,
                                       re_insert.error().message);
                }
                // Reload cache to reflect the restored provider.
                auto reload = provider_cache_->load(*this);
                if (!reload) {
                    SIXSEVEN_LOG_WARN("failed to reload provider cache: {}",
                                      reload.error().message);
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
            SIXSEVEN_LOG_WARN("failed to reload provider cache after DML: {}",
                              reload.error().message);
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
        auto edge_types = catalog_.list_edge_types(current_database_id_);

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
            // Filter to only embeddings for tables in the current database.
            auto all = catalog_.list_all_embedding_columns();
            for (const auto& emb : all) {
                if (catalog_.get_table_database_id(emb.table_id) == current_database_id_) {
                    embeddings.push_back(emb);
                }
            }
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

    case ShowTarget::BACKFILL: {
        if (!backfill_manager_) {
            QueryResult qr;
            qr.column_names = {
                "table_name", "status", "processed", "generated", "skipped", "rows_per_sec"};
            qr.column_types = {TypeId::STRING,
                               TypeId::STRING,
                               TypeId::INT64,
                               TypeId::INT64,
                               TypeId::INT64,
                               TypeId::FLOAT64};
            return ok(std::move(qr));
        }
        auto statuses = backfill_manager_->all_statuses();
        QueryResult qr;
        qr.column_names = {
            "table_name", "status", "processed", "generated", "skipped", "rows_per_sec"};
        qr.column_types = {TypeId::STRING,
                           TypeId::STRING,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::INT64,
                           TypeId::FLOAT64};
        for (const auto& s : statuses) {
            qr.rows.push_back({Value(s.table_name),
                               Value(std::string(s.running ? "running" : "completed")),
                               Value(s.processed),
                               Value(s.generated),
                               Value(s.skipped),
                               Value(s.rows_per_sec)});
        }
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

    SIXSEVEN_LOG_INFO("created user '{}'", stmt.username);

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

    SIXSEVEN_LOG_INFO("dropped user '{}'", stmt.username);

    QueryResult qr;
    qr.message = "DROP USER";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: ALTER TABLE
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_alter_table(const AlterTableStmt& stmt) {
    // Look up the table schema.
    auto schema = catalog_.get_table(current_database_id_, stmt.table_name);
    if (!schema) {
        return make_error(schema.error().code, schema.error().message);
    }

    auto table_id = schema->table_id;

    switch (stmt.action) {
    case AlterAction::ADD_COLUMN: {
        // Resolve the column type.
        auto type_result = resolve_type_spec(stmt.column.type);
        if (!type_result) {
            return make_error(type_result.error().code, type_result.error().message);
        }

        // Build the catalog column definition.
        CatalogColumnDef ccd;
        ccd.name = stmt.column.name;
        ccd.type_id = *type_result;
        ccd.nullable = stmt.column.nullable;
        if (stmt.column.default_expr) {
            ccd.default_expr = expr_to_sql(*stmt.column.default_expr);
        }

        // If NOT NULL and no DEFAULT, check that the table is empty.
        if (!ccd.nullable && ccd.default_expr.empty()) {
            auto ts = storage_.get_table_storage(table_id);
            if (ts) {
                auto it = (*ts)->heap->begin();
                if (it) {
                    if (it->next().has_value()) {
                        return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                          "cannot add NOT NULL column '" + ccd.name +
                                              "' without DEFAULT to a table with existing rows");
                    }
                }
            }
        }

        // Build old storage schema for data migration.
        auto old_storage_schema = StorageManager::build_storage_schema(*schema);

        // Add the column to the catalog.
        auto add_result = catalog_.add_column(table_id, ccd);
        if (!add_result) {
            return make_error(add_result.error().code, add_result.error().message);
        }

        // Migrate existing tuples: append NULL for the new column.
        auto ts = storage_.get_table_storage(table_id);
        if (ts) {
            auto updated_schema = catalog_.get_table_by_id(table_id);
            if (!updated_schema) {
                return make_error(updated_schema.error().code, updated_schema.error().message);
            }
            auto new_storage_schema = StorageManager::build_storage_schema(*updated_schema);

            // Collect all tuples first to avoid mutating during iteration.
            std::vector<std::pair<RID, std::vector<uint8_t>>> tuples;
            auto it = (*ts)->heap->begin();
            if (it) {
                while (auto row = it->next()) {
                    tuples.push_back(std::move(*row));
                }
            }

            // Evaluate the DEFAULT expression once if one is provided.
            Value default_value; // NULL by default.
            if (!ccd.default_expr.empty()) {
                Lexer def_lexer(ccd.default_expr);
                auto def_tokens = def_lexer.tokenize();
                if (!def_tokens) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "failed to parse default for column: " + ccd.name);
                }
                Parser def_parser(std::move(*def_tokens));
                auto def_expr = def_parser.parse_expression();
                if (!def_expr) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "failed to parse default for column: " + ccd.name);
                }
                Tuple dummy_tuple;
                OutputSchema dummy_schema;
                BoundStatement dummy_bound;
                auto eval_result =
                    evaluate_expr(**def_expr, dummy_tuple, dummy_schema, dummy_bound);
                if (!eval_result) {
                    return make_error(eval_result.error().code, eval_result.error().message);
                }
                // Fit the evaluated value to the column's declared type.
                // The expression evaluator may return a wider type (e.g., INT64
                // for an integer literal) than the column expects (e.g., INT32).
                // Standard coerce() only allows widening; DML needs narrowing too.
                auto fitted = fit_to_storage(*eval_result, ccd.type_id);
                if (!fitted) {
                    return make_error(fitted.error().code, fitted.error().message);
                }
                default_value = std::move(*fitted);
            }

            // Rewrite each tuple with the new column.
            for (auto& [rid, data] : tuples) {
                auto values = TupleSerializer::deserialize(data, old_storage_schema);
                if (!values) {
                    continue;
                }
                values->push_back(default_value);

                auto new_data = TupleSerializer::serialize(*values, new_storage_schema);
                if (!new_data) {
                    return make_error(new_data.error().code, new_data.error().message);
                }
                auto upd = (*ts)->heap->update_tuple(rid, *new_data);
                if (!upd) {
                    return make_error(upd.error().code, upd.error().message);
                }
            }

            // Update the storage schema.
            (*ts)->storage_schema = new_storage_schema;
        }
        break;
    }

    case AlterAction::DROP_COLUMN: {
        // Find the column index to drop.
        int32_t drop_index = -1;
        auto upper_target = to_upper(stmt.column_name);
        for (int32_t i = 0; i < static_cast<int32_t>(schema->columns.size()); ++i) {
            if (to_upper(schema->columns[static_cast<size_t>(i)].name) == upper_target) {
                drop_index = i;
                break;
            }
        }

        if (drop_index < 0) {
            return make_error(StatusCode::NOT_FOUND,
                              "column '" + stmt.column_name + "' not found in table '" +
                                  stmt.table_name + "'");
        }

        // Prevent dropping PK columns.
        if (!schema->pk_columns.empty()) {
            std::istringstream ss(schema->pk_columns);
            std::string part;
            while (std::getline(ss, part, ',')) {
                if (to_upper(part) == upper_target) {
                    return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                      "cannot drop primary key column '" + stmt.column_name + "'");
                }
            }
        }

        // Cannot drop the last column.
        if (schema->columns.size() <= 1) {
            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                              "cannot drop the only column in table '" + stmt.table_name + "'");
        }

        // Build old storage schema for data migration.
        auto old_storage_schema = StorageManager::build_storage_schema(*schema);

        // Drop the column from the catalog.
        auto drop_result = catalog_.drop_column(table_id, stmt.column_name);
        if (!drop_result) {
            return make_error(drop_result.error().code, drop_result.error().message);
        }

        // Migrate existing tuples: remove the dropped column.
        auto ts = storage_.get_table_storage(table_id);
        if (ts) {
            auto updated_schema = catalog_.get_table_by_id(table_id);
            if (!updated_schema) {
                return make_error(updated_schema.error().code, updated_schema.error().message);
            }
            auto new_storage_schema = StorageManager::build_storage_schema(*updated_schema);

            // Collect all tuples first.
            std::vector<std::pair<RID, std::vector<uint8_t>>> tuples;
            auto it = (*ts)->heap->begin();
            if (it) {
                while (auto row = it->next()) {
                    tuples.push_back(std::move(*row));
                }
            }

            // Rewrite each tuple without the dropped column.
            for (auto& [rid, data] : tuples) {
                auto values = TupleSerializer::deserialize(data, old_storage_schema);
                if (!values) {
                    continue;
                }
                values->erase(values->begin() + drop_index);

                auto new_data = TupleSerializer::serialize(*values, new_storage_schema);
                if (!new_data) {
                    return make_error(new_data.error().code, new_data.error().message);
                }
                auto upd = (*ts)->heap->update_tuple(rid, *new_data);
                if (!upd) {
                    return make_error(upd.error().code, upd.error().message);
                }
            }

            // Update the storage schema.
            (*ts)->storage_schema = new_storage_schema;
        }
        break;
    }

    case AlterAction::RENAME_COLUMN: {
        // Validate column exists.
        bool found = false;
        auto upper_rename_target = to_upper(stmt.column_name);
        for (const auto& col : schema->columns) {
            if (to_upper(col.name) == upper_rename_target) {
                found = true;
                break;
            }
        }

        if (!found) {
            return make_error(StatusCode::NOT_FOUND,
                              "column '" + stmt.column_name + "' not found in table '" +
                                  stmt.table_name + "'");
        }

        auto rename_result =
            catalog_.rename_column(table_id, stmt.column_name, stmt.new_column_name);
        if (!rename_result) {
            return make_error(rename_result.error().code, rename_result.error().message);
        }

        // Update storage schema (column names changed, no data migration needed).
        auto ts = storage_.get_table_storage(table_id);
        if (ts) {
            auto updated_schema = catalog_.get_table_by_id(table_id);
            if (updated_schema) {
                (*ts)->storage_schema = StorageManager::build_storage_schema(*updated_schema);
            }
        }
        break;
    }
    }

    // Persist updated columns to sys_columns.
    if (catalog_persistence_ != nullptr) {
        auto updated_schema = catalog_.get_table_by_id(table_id);
        if (updated_schema) {
            auto persist = catalog_persistence_->persist_columns_update(*updated_schema);
            if (!persist) {
                SIXSEVEN_LOG_WARN("failed to persist ALTER TABLE for '{}': {}",
                                  stmt.table_name,
                                  persist.error().message);
            }
        }
    }

    SIXSEVEN_LOG_INFO("altered table '{}'", stmt.table_name);

    QueryResult qr;
    qr.message = "ALTER TABLE";
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

    SIXSEVEN_LOG_INFO("altered user '{}'", stmt.username);

    QueryResult qr;
    qr.message = "ALTER USER";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: CREATE INDEX
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_create_index(const CreateIndexStmt& stmt) {
    // Resolve the table.
    auto schema = catalog_.get_table(current_database_id_, stmt.table_name);
    if (!schema) {
        return make_error(schema.error().code, schema.error().message);
    }

    // Check for duplicate index name.
    auto existing = catalog_.get_index(current_database_id_, stmt.name);
    if (existing) {
        if (stmt.if_not_exists) {
            QueryResult qr;
            qr.message = "CREATE INDEX";
            return ok(std::move(qr));
        }
        return make_error(StatusCode::ALREADY_EXISTS, "index '" + stmt.name + "' already exists");
    }

    // Validate all indexed columns exist in the table.
    for (const auto& col_name : stmt.columns) {
        bool found = false;
        auto upper_col = to_upper(col_name);
        for (const auto& col : schema->columns) {
            if (to_upper(col.name) == upper_col) {
                found = true;
                break;
            }
        }
        if (!found) {
            return make_error(StatusCode::NOT_FOUND,
                              "column '" + col_name + "' not found in table '" + stmt.table_name +
                                  "'");
        }
    }

    // Normalize the index method to lowercase so "USING BM25" and "USING bm25"
    // resolve to the same index_type the IndexManager dispatches on.
    std::string method_lower = stmt.method;
    std::transform(method_lower.begin(),
                   method_lower.end(),
                   method_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // BM25 full-text indexes require exactly one STRING column.
    if (method_lower == "bm25") {
        if (stmt.columns.size() != 1) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "BM25 index requires exactly one text column");
        }
        auto upper_col = to_upper(stmt.columns[0]);
        for (const auto& col : schema->columns) {
            if (to_upper(col.name) == upper_col && col.type_id != TypeId::STRING) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "BM25 index requires a STRING column, but '" + stmt.columns[0] +
                                      "' is " + std::string(type_name(col.type_id)));
            }
        }
    }

    // Build the index definition.
    IndexDef def;
    def.table_id = schema->table_id;
    def.name = stmt.name;
    def.index_type = stmt.method.empty() ? "btree" : method_lower;
    def.is_unique = stmt.is_unique;

    // Build comma-separated column list.
    std::string cols;
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        if (i > 0) {
            cols += ",";
        }
        cols += stmt.columns[i];
    }
    def.columns = cols;

    // Register in catalog.
    auto index_id = catalog_.create_index(std::move(def));
    if (!index_id) {
        return make_error(index_id.error().code, index_id.error().message);
    }

    // Persist to system catalog tables.
    if (catalog_persistence_ != nullptr) {
        auto created_def = catalog_.get_index(current_database_id_, stmt.name);
        if (created_def) {
            auto persist = catalog_persistence_->persist_index(*created_def);
            if (!persist) {
                SIXSEVEN_LOG_WARN(
                    "failed to persist index '{}': {}", stmt.name, persist.error().message);
            }
        }
    }

    // Build and populate the in-memory index structure.
    if (index_manager_ != nullptr) {
        auto created_def2 = catalog_.get_index(current_database_id_, stmt.name);
        if (created_def2) {
            auto populate = index_manager_->create_and_populate_index(*created_def2, *schema);
            if (!populate) {
                SIXSEVEN_LOG_WARN(
                    "failed to populate index '{}': {}", stmt.name, populate.error().message);
            }
        }
    }

    SIXSEVEN_LOG_INFO("created index '{}' on table '{}'", stmt.name, stmt.table_name);

    QueryResult qr;
    qr.message = "CREATE INDEX";
    return ok(std::move(qr));
}

// ---------------------------------------------------------------------------
// DDL: DROP INDEX
// ---------------------------------------------------------------------------

Result<QueryResult> QueryEngine::execute_drop_index(const DropIndexStmt& stmt) {
    // Look up the index to get its ID for persistence removal.
    auto idx = catalog_.get_index(current_database_id_, stmt.name);
    if (!idx) {
        if (stmt.if_exists) {
            QueryResult qr;
            qr.message = "DROP INDEX";
            return ok(std::move(qr));
        }
        return make_error(idx.error().code, idx.error().message);
    }

    auto index_id = idx->index_id;

    // Remove from persistence before dropping from catalog.
    if (catalog_persistence_ != nullptr) {
        auto remove = catalog_persistence_->remove_index(index_id);
        if (!remove) {
            SIXSEVEN_LOG_WARN("failed to remove index '{}' from persistence: {}",
                              stmt.name,
                              remove.error().message);
        }
    }

    // Remove from in-memory index structures.
    if (index_manager_ != nullptr) {
        index_manager_->drop_index(index_id, idx->table_id);
    }

    // Remove from catalog.
    auto drop_result = catalog_.drop_index(current_database_id_, stmt.name);
    if (!drop_result) {
        return make_error(drop_result.error().code, drop_result.error().message);
    }

    SIXSEVEN_LOG_INFO("dropped index '{}'", stmt.name);

    QueryResult qr;
    qr.message = "DROP INDEX";
    return ok(std::move(qr));
}

} // namespace sixseven
