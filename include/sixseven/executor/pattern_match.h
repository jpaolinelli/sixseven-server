#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

/// Node definition within a MATCH pattern.
struct MatchNodeDef {
    std::string variable;
    std::string label;
};

/// Edge definition within a MATCH pattern.
struct MatchEdgeDef {
    std::string variable;
    std::string edge_type;
    TraverseDirection direction = TraverseDirection::OUT;
};

/// Configuration for a MATCH pattern matching query.
struct MatchConfig {
    std::vector<MatchNodeDef> nodes;
    std::vector<MatchEdgeDef> edges;
};

/// MATCH pattern matching executor operator.
///
/// Compiles Cypher-inspired graph patterns into join plans:
///   - Single-hop: (a:T1)-[r:E]->(b:T2) → edge table lookup
///   - Multi-hop: chain of edge table joins
///
/// Materialises all matching tuples during open().
class PatternMatchOperator : public Iterator {
public:
    PatternMatchOperator(GraphEngine& graph_engine,
                         const Catalog& catalog,
                         StorageManager& storage,
                         database_id_t database_id,
                         MatchConfig config,
                         OutputSchema schema,
                         const Expr* where_expr,
                         const BoundStatement& bound);

    const OutputSchema& output_schema() const override;

    // -- Plan inspection (for EXPLAIN) ----------------------------------------
    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Execute single-hop pattern: (a)-[r]->(b).
    Result<void> execute_single_hop();

    /// Execute multi-hop pattern: chain of joins.
    Result<void> execute_multi_hop();

    /// Fetch full row data for a node PK from a table.
    Result<std::vector<Value>> fetch_node_data(const std::string& table_name,
                                               const Value& pk) const;

    GraphEngine& graph_engine_;
    const Catalog& catalog_;
    StorageManager& storage_;
    database_id_t database_id_;
    MatchConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
