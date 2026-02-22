#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <memory>
#include <vector>

namespace giodb {

/// Translates a BoundStatement into a physical operator (Iterator) tree.
///
/// The planner directly constructs Volcano iterators without an
/// intermediate logical plan layer.  When an optimizer is added in the
/// future, it would be inserted between the Binder and the Planner.
///
/// Supports: SELECT, INSERT, UPDATE, DELETE.
/// DDL statements are handled by QueryEngine directly since they don't
/// produce iterator trees.
class Planner {
public:
    /// @param catalog  System catalog for schema lookups.
    /// @param storage  StorageManager for TableHeap access.
    Planner(const Catalog& catalog, StorageManager& storage);

    /// Build an iterator tree for a DML/query statement.
    ///
    /// @param bound        The bound statement from the Binder.
    /// @param owned_exprs  Output: expressions created by the planner
    ///                     (e.g. ColumnRefExprs for SELECT *). The caller
    ///                     must keep this vector alive as long as the
    ///                     returned iterator is in use.
    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan(const BoundStatement& bound, std::vector<ExprPtr>& owned_exprs);

private:
    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan_select(const SelectStmt& stmt, const BoundStatement& bound,
                std::vector<ExprPtr>& owned_exprs);

    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan_insert(const InsertStmt& stmt, const BoundStatement& bound);

    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan_update(const UpdateStmt& stmt, const BoundStatement& bound);

    [[nodiscard]] Result<std::unique_ptr<Iterator>>
    plan_delete(const DeleteStmt& stmt, const BoundStatement& bound);

    /// Build an OutputSchema from BoundStatement::output_columns.
    [[nodiscard]] static OutputSchema
    build_output_schema(const std::vector<ResolvedColumn>& columns);

    /// Build an OutputSchema from a TableSchema (all columns of the table).
    /// Uses @p table_alias as the table name if non-empty.
    [[nodiscard]] static OutputSchema
    build_table_output_schema(const TableSchema& ts,
                              const std::string& table_alias = "");

    const Catalog& catalog_;
    StorageManager& storage_;
};

} // namespace giodb
