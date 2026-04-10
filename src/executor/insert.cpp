#include "sixseven/executor/insert.h"

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/expr_evaluator.h"
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
            // Coerce to storage types when needed.
            for (size_t i = 0; i < values.size() && i < storage_schema_.column_count(); ++i) {
                auto target = storage_schema_.column(i).type;
                if (values[i].type_id() != target && !values[i].is_null()) {
                    auto fitted = fit_to_storage(values[i], target);
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
            auto rid = heap_.insert_tuple(*bytes);
            if (!rid) {
                return make_error(rid.error().code, rid.error().message);
            }
            enqueue_embedding_jobs(*rid, values);
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
                        auto fitted = fit_to_storage(*val, target);
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

        auto rids = heap_.insert_batch(tuple_spans);
        if (!rids) {
            return make_error(rids.error().code, rids.error().message);
        }

        // Enqueue embedding jobs for each inserted row.
        for (size_t i = 0; i < rids->size(); ++i) {
            enqueue_embedding_jobs((*rids)[i], all_values[i]);
        }
        count = static_cast<int64_t>(rids->size());
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

        // Resolve source column by name and extract text value.
        for (size_t i = 0; i < column_names_.size() && i < values.size(); ++i) {
            if (column_names_[i] == emb.source_expr) {
                if (!values[i].is_null() && values[i].type_id() == TypeId::STRING) {
                    job.source_text = values[i].as_string();
                }
                break;
            }
        }

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
        auto result = embedding_pool_->enqueue_batch(std::move(jobs));
        if (!result) {
            SIXSEVEN_LOG_WARN("failed to enqueue embedding jobs: {}", result.error().message);
        }
    }
}

} // namespace sixseven
