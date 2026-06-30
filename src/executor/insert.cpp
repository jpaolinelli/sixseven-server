#include "sixseven/executor/insert.h"

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/embedding_worker.h"

#include <algorithm>
#include <cctype>

namespace sixseven {

namespace {

/// Extract an int64 from a Value for autoincrement counter advancement.
int64_t value_to_int64(const Value& val) {
    switch (val.type_id()) {
    case TypeId::INT8:
        return val.as_int8();
    case TypeId::INT16:
        return val.as_int16();
    case TypeId::INT32:
        return val.as_int32();
    case TypeId::INT64:
        return val.as_int64();
    case TypeId::UINT8:
        return val.as_uint8();
    case TypeId::UINT16:
        return val.as_uint16();
    case TypeId::UINT32:
        return val.as_uint32();
    case TypeId::UINT64:
        return static_cast<int64_t>(val.as_uint64());
    default:
        return 0;
    }
}

} // namespace

InsertOperator::InsertOperator(TableHeap& heap,
                               const Schema& storage_schema,
                               std::vector<std::vector<const Expr*>> value_rows,
                               const BoundStatement& bound)
    : heap_(heap), storage_schema_(storage_schema), value_rows_(std::move(value_rows)),
      bound_(bound), schema_(OutputSchema({OutputColumn{"", "count", TypeId::INT64, false, 0}})) {}

InsertOperator::InsertOperator(TableHeap& heap,
                               const Schema& storage_schema,
                               std::unique_ptr<Iterator> child)
    : heap_(heap), storage_schema_(storage_schema), child_(std::move(child)),
      schema_(OutputSchema({OutputColumn{"", "count", TypeId::INT64, false, 0}})) {}

Result<void> InsertOperator::do_open() {
    executed_ = false;
    if (child_) {
        return child_->open();
    }
    return ok();
}

Result<Value> InsertOperator::make_autoincrement_value(int64_t counter, TypeId type_id) {
    switch (type_id) {
    case TypeId::INT8:
        return ok(Value(static_cast<int8_t>(counter)));
    case TypeId::INT16:
        return ok(Value(static_cast<int16_t>(counter)));
    case TypeId::INT32:
        return ok(Value(static_cast<int32_t>(counter)));
    case TypeId::INT64:
        return ok(Value(counter));
    case TypeId::UINT8:
        return ok(Value(static_cast<uint8_t>(counter)));
    case TypeId::UINT16:
        return ok(Value(static_cast<uint16_t>(counter)));
    case TypeId::UINT32:
        return ok(Value(static_cast<uint32_t>(counter)));
    case TypeId::UINT64:
        return ok(Value(static_cast<uint64_t>(counter)));
    default:
        return make_error(StatusCode::TYPE_ERROR, "unsupported autoincrement type");
    }
}

Result<std::optional<Tuple>> InsertOperator::do_next() {
    if (executed_) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    executed_ = true;

    // Acquire IX (Intent Exclusive) table lock before inserting any rows (GDB-930).
    // An IX lock on the table signals intent to write individual rows; it is
    // compatible with other IX holders (multiple concurrent inserters) but blocks
    // an S or X table-level lock (e.g., a DDL ALTER TABLE).  A fresh row has no
    // prior version, so no row-level X lock is needed at insert time — only the
    // intent lock is required to participate in the locking protocol.
    if (lock_mgr_ != nullptr && txn_id_ != frozen_txn_id) {
        if (auto lr = lock_mgr_->lock_table(txn_id_, lock_table_id_, LockMode::IX); !lr) {
            return tl::unexpected(lr.error());
        }
    }

    int64_t count = 0;

    if (child_) {
        // INSERT ... SELECT — pull rows from child iterator.
        while (true) {
            auto row = child_->next();
            if (!row) {
                return make_error(row.error().code, row.error().message);
            }
            if (!row->has_value()) {
                break;
            }

            auto values = row->value().values;
            // If a column map was set (INSERT...SELECT with explicit column list),
            // reorder child values to storage schema order.
            if (!child_col_map_.empty()) {
                std::vector<Value> reordered(child_col_map_.size());
                for (size_t i = 0; i < child_col_map_.size(); ++i) {
                    if (child_col_map_[i] < values.size()) {
                        reordered[i] = values[child_col_map_[i]];
                    } else {
                        // Column is unmapped (child_col_map_[i] == SIZE_MAX).
                        // reordered[i] stays a default-constructed null Value.
                        // Reject if the target column is NOT NULL (no default applied here).
                        const bool is_nullable =
                            (i < col_nullable_.size()) ? col_nullable_[i] : true;
                        if (!is_nullable) {
                            const std::string col_name = (i < col_names_for_null_check_.size())
                                                             ? col_names_for_null_check_[i]
                                                             : std::to_string(i);
                            return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                              "NOT NULL constraint violated: column '" + col_name +
                                                  "' cannot be NULL");
                        }
                    }
                }
                values = std::move(reordered);
            }
            // Coerce to storage types when needed.
            for (size_t i = 0; i < values.size() && i < storage_schema_.column_count(); ++i) {
                auto target = storage_schema_.column(i).type;
                if (values[i].type_id() != target && !values[i].is_null()) {
                    int32_t scale = (i < col_scales_.size()) ? col_scales_[i] : 0;
                    auto fitted = fit_to_storage(values[i], target, scale);
                    if (!fitted) {
                        return make_error(fitted.error().code, fitted.error().message);
                    }
                    values[i] = std::move(*fitted);
                }
            }
            auto bytes = TupleSerializer::serialize(values, storage_schema_);
            if (!bytes) {
                return make_error(bytes.error().code, bytes.error().message);
            }
            auto rid = heap_.insert_tuple(*bytes, txn_id_);
            if (!rid) {
                return make_error(rid.error().code, rid.error().message);
            }
            enqueue_embedding_jobs(*rid, values);
            if (auto r = maintain_bm25(*rid, values); !r) {
                return tl::unexpected(r.error());
            }
            if (auto r = maintain_secondary_indexes(*rid, values); !r) {
                return tl::unexpected(r.error());
            }
            ++count;
        }
    } else {
        // INSERT ... VALUES — evaluate and serialize all rows, then batch insert.
        Tuple dummy{{}, std::nullopt};
        OutputSchema empty_schema;

        std::vector<std::vector<uint8_t>> serialized_rows;
        std::vector<std::vector<Value>> all_values;
        serialized_rows.reserve(value_rows_.size());
        all_values.reserve(value_rows_.size());

        for (const auto& row_exprs : value_rows_) {
            std::vector<Value> values;
            values.reserve(row_exprs.size());

            for (size_t i = 0; i < row_exprs.size(); ++i) {
                auto val = evaluate_expr(*row_exprs[i], dummy, empty_schema, bound_);
                if (!val) {
                    return make_error(val.error().code, val.error().message);
                }
                // Coerce to storage schema type so TupleSerializer sees the
                // expected variant alternative.
                if (i < storage_schema_.column_count()) {
                    auto target = storage_schema_.column(i).type;
                    if (val->type_id() != target && !val->is_null()) {
                        int32_t scale = (i < col_scales_.size()) ? col_scales_[i] : 0;
                        auto fitted = fit_to_storage(*val, target, scale);
                        if (!fitted) {
                            return make_error(fitted.error().code, fitted.error().message);
                        }
                        values.push_back(std::move(*fitted));
                        continue;
                    }
                }
                values.push_back(std::move(*val));
            }

            // Handle auto-increment columns.
            if (catalog_ != nullptr) {
                for (const auto& ai : autoincrement_cols_) {
                    if (ai.col_idx >= values.size()) {
                        continue;
                    }
                    if (ai.is_placeholder || values[ai.col_idx].is_null()) {
                        // Column was omitted — assign next auto-increment value.
                        auto next = catalog_->next_autoincrement(ai.table_id, ai.type_id);
                        if (!next) {
                            return make_error(next.error().code, next.error().message);
                        }
                        auto ai_val = make_autoincrement_value(*next, ai.type_id);
                        if (!ai_val) {
                            return make_error(ai_val.error().code, ai_val.error().message);
                        }
                        values[ai.col_idx] = std::move(*ai_val);
                    } else {
                        // Explicit value provided — advance counter past it.
                        int64_t explicit_val = value_to_int64(values[ai.col_idx]);
                        catalog_->advance_autoincrement(ai.table_id, explicit_val);
                    }
                }
            }

            auto bytes = TupleSerializer::serialize(values, storage_schema_);
            if (!bytes) {
                return make_error(bytes.error().code, bytes.error().message);
            }
            serialized_rows.push_back(std::move(*bytes));
            all_values.push_back(std::move(values));
        }

        // Build span vector and batch insert.
        std::vector<std::span<const uint8_t>> tuple_spans;
        tuple_spans.reserve(serialized_rows.size());
        for (const auto& row : serialized_rows) {
            tuple_spans.emplace_back(row);
        }

        auto rids = heap_.insert_batch(tuple_spans, txn_id_);
        if (!rids) {
            return make_error(rids.error().code, rids.error().message);
        }

        // Enqueue embedding jobs and maintain BM25 indexes for each inserted row.
        for (size_t i = 0; i < rids->size(); ++i) {
            enqueue_embedding_jobs((*rids)[i], all_values[i]);
            if (auto r = maintain_bm25((*rids)[i], all_values[i]); !r) {
                return tl::unexpected(r.error());
            }
            if (auto r = maintain_secondary_indexes((*rids)[i], all_values[i]); !r) {
                return tl::unexpected(r.error());
            }
        }
        count = static_cast<int64_t>(rids->size());
    }

    // Durably persist the auto-increment counter after every INSERT statement so
    // that a crash-like (no-flush) restart never reissues a previously-issued ID.
    // This mirrors the value written by the flush path (index_manager.cpp flush
    // loop), which also writes get_autoincrement_counter() -- the next-to-issue
    // value. Per-statement (rather than per-row) is sufficient: the statement is
    // atomic, so if any row fails the whole statement is rolled back and the
    // counter is not advanced.
    if (catalog_ != nullptr && storage_manager_ != nullptr && !autoincrement_cols_.empty()) {
        for (const auto& ai : autoincrement_cols_) {
            int64_t counter = catalog_->get_autoincrement_counter(ai.table_id);
            if (counter > 0) {
                (void)storage_manager_->write_autoincrement(ai.table_id, counter);
            }
            break; // one counter per table; all ai_cols share the same table_id
        }
    }

    Tuple result;
    result.values.push_back(Value(count));
    return ok(std::optional<Tuple>(std::move(result)));
}

void InsertOperator::do_close() {
    if (child_) {
        child_->close();
    }
}

const OutputSchema& InsertOperator::output_schema() const {
    return schema_;
}

std::vector<const Iterator*> InsertOperator::plan_children() const {
    if (child_)
        return {child_.get()};
    return {};
}

std::vector<Iterator*> InsertOperator::plan_children_mutable() {
    if (child_)
        return {child_.get()};
    return {};
}

void InsertOperator::enqueue_embedding_jobs(const RID& rid, const std::vector<Value>& values) {
    if (embedding_pool_ == nullptr || embedding_cols_.empty()) {
        if (embedding_pool_ == nullptr && !embedding_cols_.empty()) {
            SIXSEVEN_LOG_WARN("embedding pool is null but table has {} embedding columns",
                              embedding_cols_.size());
        }
        return;
    }

    // Encode RID as int64_t: page_id in upper 32 bits, slot_id in lower 16.
    auto row_id = (static_cast<int64_t>(rid.page_id) << 32) | static_cast<int64_t>(rid.slot_id);

    std::vector<EmbeddingJob> jobs;
    jobs.reserve(embedding_cols_.size());

    for (const auto& emb : embedding_cols_) {
        EmbeddingJob job;
        job.table_id = embedding_table_id_;
        job.row_id = row_id;
        job.column_id = emb.column_id;
        job.provider = emb.provider;
        job.dimension = emb.dimension;
        job.type = EmbeddingJob::Type::INSERT;

        // Resolve source text via the shared multi-column helper.
        // Build a lightweight schema view from the insert column list so that
        // build_source_text can match column names uniformly.
        std::vector<CatalogColumnDef> insert_schema;
        insert_schema.reserve(column_names_.size());
        for (size_t i = 0; i < column_names_.size(); ++i) {
            CatalogColumnDef cd;
            cd.name = column_names_[i];
            cd.ordinal = static_cast<int32_t>(i);
            insert_schema.push_back(std::move(cd));
        }
        auto src =
            EmbeddingColumnManager::build_source_text(emb.source_expr, insert_schema, values);
        job.source_text = std::move(src.text);

        // Skip jobs with empty or whitespace-only source text — NULL/blank source
        // means NULL embedding; no need to enqueue a job that will always fail.
        if (job.source_text.empty() ||
            std::all_of(job.source_text.begin(), job.source_text.end(), [](unsigned char c) {
                return std::isspace(c);
            })) {
            continue;
        }

        jobs.push_back(std::move(job));
    }

    if (!jobs.empty()) {
        // Non-blocking enqueue: 0ms timeout means we never stall the INSERT
        // thread.  With persistence wired, jobs are durably saved *before*
        // the in-memory enqueue attempt, so even if the queue is full the
        // jobs survive on disk and the background recovery sweep will
        // re-enqueue them once workers drain the queue.
        auto result =
            embedding_pool_->try_enqueue_batch(std::move(jobs), std::chrono::milliseconds(0));
        if (!result) {
            // Queue full is expected under load — jobs are persisted to disk
            // and will be recovered by the background sweep.  Use DEBUG to
            // avoid log spam during bulk inserts.
            SIXSEVEN_LOG_DEBUG("embedding enqueue deferred (persisted to disk): {}",
                               result.error().message);
        }
    }
}

Result<void> InsertOperator::maintain_secondary_indexes(const RID& rid,
                                                        const std::vector<Value>& values) {
    for (const auto& target : btree_targets_) {
        if (target.index == nullptr) {
            continue;
        }
        KeyType key;
        key.reserve(target.key_column_ordinals.size());
        for (size_t ordinal : target.key_column_ordinals) {
            if (ordinal < values.size()) {
                key.push_back(values[ordinal]);
            }
        }
        auto r = target.index->insert(key, rid);
        if (!r) {
            return tl::unexpected(r.error());
        }
    }

    for (const auto& target : hash_targets_) {
        if (target.index == nullptr) {
            continue;
        }
        KeyType key;
        key.reserve(target.key_column_ordinals.size());
        for (size_t ordinal : target.key_column_ordinals) {
            if (ordinal < values.size()) {
                key.push_back(values[ordinal]);
            }
        }
        auto r = target.index->insert(key, rid);
        if (!r) {
            return tl::unexpected(r.error());
        }
    }
    return ok();
}

Result<void> InsertOperator::maintain_bm25(const RID& rid, const std::vector<Value>& values) {
    for (const auto& target : bm25_targets_) {
        if (target.index == nullptr || target.text_column_index >= values.size()) {
            continue;
        }
        const auto& v = values[target.text_column_index];
        // NULL text means the row was never indexed for BM25 - benign skip.
        if (v.is_null()) {
            continue;
        }
        auto s = v.try_as_string();
        if (!s) {
            continue;
        }
        auto r = target.index->add_document(rid, **s);
        if (!r) {
            return tl::unexpected(r.error());
        }
    }
    return ok();
}

} // namespace sixseven
