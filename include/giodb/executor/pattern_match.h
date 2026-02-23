#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/tuple.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

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

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

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

} // namespace giodb
