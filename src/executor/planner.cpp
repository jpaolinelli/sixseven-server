#include "giodb/executor/planner.h"

#include "giodb/executor/delete.h"
#include "giodb/executor/filter.h"
#include "giodb/executor/insert.h"
#include "giodb/executor/limit.h"
#include "giodb/executor/nested_loop_join.h"
#include "giodb/executor/project.h"
#include "giodb/executor/seq_scan.h"
#include "giodb/executor/sort.h"
#include "giodb/executor/update.h"

#include <string>

namespace giodb {

Planner::Planner(const Catalog& catalog, StorageManager& storage)
    : catalog_(catalog), storage_(storage) {}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan(const BoundStatement& bound,
                                                std::vector<ExprPtr>& owned_exprs) {
    if (auto* sel = dynamic_cast<const SelectStmt*>(bound.stmt)) {
        return plan_select(*sel, bound, owned_exprs);
    }
    if (auto* ins = dynamic_cast<const InsertStmt*>(bound.stmt)) {
        return plan_insert(*ins, bound);
    }
    if (auto* upd = dynamic_cast<const UpdateStmt*>(bound.stmt)) {
        return plan_update(*upd, bound);
    }
    if (auto* del = dynamic_cast<const DeleteStmt*>(bound.stmt)) {
        return plan_delete(*del, bound);
    }
    return make_error(StatusCode::NOT_IMPLEMENTED, "planner does not support this statement type");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

OutputSchema Planner::build_output_schema(const std::vector<ResolvedColumn>& columns) {
    std::vector<OutputColumn> out;
    out.reserve(columns.size());
    for (const auto& rc : columns) {
        out.push_back({rc.table_name, rc.column_name, rc.type_id, rc.nullable, rc.table_id});
    }
    return OutputSchema(std::move(out));
}

OutputSchema Planner::build_table_output_schema(const TableSchema& ts,
                                                const std::string& table_alias) {
    std::vector<OutputColumn> out;
    out.reserve(ts.columns.size());
    const auto& tname = table_alias.empty() ? ts.name : table_alias;
    for (const auto& col : ts.columns) {
        out.push_back({tname, col.name, col.type_id, col.nullable, ts.table_id});
    }
    return OutputSchema(std::move(out));
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_select(const SelectStmt& stmt,
                                                       const BoundStatement& bound,
                                                       std::vector<ExprPtr>& owned_exprs) {
    // -- 1. Resolve the source table -----------------------------------------
    if (stmt.from.empty()) {
        return make_error(StatusCode::NOT_IMPLEMENTED, "SELECT without FROM is not yet supported");
    }

    const auto& table_ref = stmt.from[0];
    auto table_schema = catalog_.get_table(table_ref.name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    // Use alias if provided.
    const auto& alias = table_ref.alias.empty() ? table_ref.name : table_ref.alias;
    auto table_output = build_table_output_schema(*table_schema, alias);

    const bool has_joins = !stmt.joins.empty();

    // -- 2. SeqScan -----------------------------------------------------------
    // When joins are present, don't push WHERE into the scan (it may reference
    // columns from join tables). Instead apply WHERE after joins as a filter.
    const Expr* scan_predicate = (!has_joins && stmt.where_expr) ? stmt.where_expr.get() : nullptr;

    auto scan = std::make_unique<SeqScanOperator>(
        *storage->heap, storage->storage_schema, table_output, scan_predicate, &bound);

    std::unique_ptr<Iterator> child = std::move(scan);

    // -- 2b. JOIN operators ---------------------------------------------------
    if (has_joins) {
        for (const auto& join_clause : stmt.joins) {
            const auto& jtref = join_clause.table;
            auto join_schema = catalog_.get_table(jtref.name);
            if (!join_schema) {
                return make_error(join_schema.error().code, join_schema.error().message);
            }

            auto jts = storage_.get_table_storage(join_schema->table_id);
            if (!jts) {
                return make_error(jts.error().code, jts.error().message);
            }
            auto* join_storage = *jts;

            const auto& join_alias = jtref.alias.empty() ? jtref.name : jtref.alias;
            auto join_table_output = build_table_output_schema(*join_schema, join_alias);

            auto join_scan = std::make_unique<SeqScanOperator>(*join_storage->heap,
                                                               join_storage->storage_schema,
                                                               join_table_output,
                                                               nullptr,
                                                               &bound);

            // Build combined output schema: left columns + right columns.
            const auto& left_schema = child->output_schema();
            const auto& right_schema_ref = join_scan->output_schema();

            std::vector<OutputColumn> combined_cols;
            combined_cols.reserve(left_schema.column_count() + right_schema_ref.column_count());
            for (size_t i = 0; i < left_schema.column_count(); ++i) {
                combined_cols.push_back(left_schema.column(i));
            }
            for (size_t i = 0; i < right_schema_ref.column_count(); ++i) {
                combined_cols.push_back(right_schema_ref.column(i));
            }
            auto combined = OutputSchema(std::move(combined_cols));

            const Expr* on_expr = join_clause.on_expr ? join_clause.on_expr.get() : nullptr;

            child = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                             std::move(join_scan),
                                                             join_clause.type,
                                                             on_expr,
                                                             bound,
                                                             std::move(combined));
        }

        // Apply WHERE as a filter after all joins.
        if (stmt.where_expr) {
            child = std::make_unique<FilterOperator>(std::move(child), *stmt.where_expr, bound);
        }
    }

    // -- 3. Projection -------------------------------------------------------
    std::vector<ProjectionExpr> projections;
    projections.reserve(stmt.items.size());

    for (const auto& item : stmt.items) {
        if (item.is_star || !item.table_star.empty()) {
            // Expand * or table.* using bound output columns (which the binder
            // already resolved for all tables including joins).
            for (const auto& rc : bound.output_columns) {
                if (!item.table_star.empty() && rc.table_name != item.table_star) {
                    continue;
                }
                auto cr = std::make_unique<ColumnRefExpr>();
                cr->table = rc.table_name;
                cr->column = rc.column_name;
                projections.push_back({cr.get(), rc.column_name});
                owned_exprs.push_back(std::move(cr));
            }
        } else {
            // Named expression.
            std::string col_alias = item.alias;
            if (col_alias.empty()) {
                // Try to derive a name from a column reference.
                if (auto* cr = dynamic_cast<const ColumnRefExpr*>(item.expr.get())) {
                    col_alias = cr->column;
                } else {
                    col_alias = "?column?";
                }
            }
            projections.push_back({item.expr.get(), col_alias});
        }
    }

    auto output_schema = build_output_schema(bound.output_columns);
    child = std::make_unique<ProjectOperator>(
        std::move(child), std::move(projections), std::move(output_schema), bound);

    // -- 4. ORDER BY ---------------------------------------------------------
    if (!stmt.order_by.empty()) {
        std::vector<SortKey> keys;
        keys.reserve(stmt.order_by.size());
        for (const auto& ob : stmt.order_by) {
            keys.push_back({ob.expr.get(), ob.direction});
        }
        child = std::make_unique<SortOperator>(std::move(child), std::move(keys), bound);
    }

    // -- 5. LIMIT / OFFSET ---------------------------------------------------
    if (stmt.limit) {
        int64_t limit_val = 0;
        if (auto* lit = dynamic_cast<const LiteralExpr*>(stmt.limit.get())) {
            limit_val = std::stoll(lit->value);
        } else {
            return make_error(StatusCode::NOT_IMPLEMENTED,
                              "non-literal LIMIT is not yet supported");
        }

        int64_t offset_val = 0;
        if (stmt.offset) {
            if (auto* olit = dynamic_cast<const LiteralExpr*>(stmt.offset.get())) {
                offset_val = std::stoll(olit->value);
            }
        }

        child = std::make_unique<LimitOperator>(std::move(child), limit_val, offset_val);
    }

    return ok(std::move(child));
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_insert(const InsertStmt& stmt,
                                                       const BoundStatement& bound) {
    auto table_schema = catalog_.get_table(stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    // Build value rows (raw pointers into the AST).
    // If explicit column list is provided, reorder to match storage schema.
    std::vector<std::vector<const Expr*>> value_rows;
    value_rows.reserve(stmt.values.size());

    if (stmt.columns.empty()) {
        // All columns in schema order.
        for (const auto& row : stmt.values) {
            std::vector<const Expr*> ptrs;
            ptrs.reserve(row.size());
            for (const auto& expr : row) {
                ptrs.push_back(expr.get());
            }
            value_rows.push_back(std::move(ptrs));
        }
    } else {
        // Build mapping: storage_col_index -> expression_index_in_values.
        size_t ncols = table_schema->columns.size();
        for (const auto& row : stmt.values) {
            std::vector<const Expr*> reordered(ncols, nullptr);
            for (size_t i = 0; i < stmt.columns.size(); ++i) {
                // Find the column's ordinal in the table schema.
                bool found = false;
                for (size_t j = 0; j < ncols; ++j) {
                    if (table_schema->columns[j].name == stmt.columns[i]) {
                        reordered[j] = row[i].get();
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return make_error(StatusCode::NOT_FOUND,
                                      "column not found: " + stmt.columns[i]);
                }
            }
            // Columns not mentioned get nullptr — InsertOperator evaluates
            // nullptr as NULL (the expression evaluator is not called).
            // For now we require all columns to be provided.
            for (size_t j = 0; j < ncols; ++j) {
                if (reordered[j] == nullptr) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "missing value for column: " + table_schema->columns[j].name);
                }
            }
            value_rows.push_back(std::move(reordered));
        }
    }

    auto iter = std::make_unique<InsertOperator>(
        *storage->heap, storage->storage_schema, std::move(value_rows), bound);
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_update(const UpdateStmt& stmt,
                                                       const BoundStatement& bound) {
    auto table_schema = catalog_.get_table(stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    auto table_output = build_table_output_schema(*table_schema);

    // SeqScan with optional WHERE predicate.
    const Expr* predicate = stmt.where_expr ? stmt.where_expr.get() : nullptr;
    auto scan = std::make_unique<SeqScanOperator>(
        *storage->heap, storage->storage_schema, table_output, predicate, &bound);

    // Build assignment vector.
    std::vector<UpdateAssignment> assignments;
    assignments.reserve(stmt.assignments.size());
    for (const auto& assign : stmt.assignments) {
        // Find column index in table schema.
        bool found = false;
        for (size_t i = 0; i < table_schema->columns.size(); ++i) {
            if (table_schema->columns[i].name == assign.column) {
                assignments.push_back({i, assign.value.get()});
                found = true;
                break;
            }
        }
        if (!found) {
            return make_error(StatusCode::NOT_FOUND, "column not found: " + assign.column);
        }
    }

    auto iter = std::make_unique<UpdateOperator>(
        *storage->heap, storage->storage_schema, std::move(scan), std::move(assignments), bound);
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_delete(const DeleteStmt& stmt,
                                                       const BoundStatement& bound) {
    auto table_schema = catalog_.get_table(stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    auto table_output = build_table_output_schema(*table_schema);

    const Expr* predicate = stmt.where_expr ? stmt.where_expr.get() : nullptr;
    auto scan = std::make_unique<SeqScanOperator>(
        *storage->heap, storage->storage_schema, table_output, predicate, &bound);

    auto iter = std::make_unique<DeleteOperator>(*storage->heap, std::move(scan));
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

} // namespace giodb
