#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

/// Node definition within a MATCH pattern.
struct MatchNodeDef {
    std::string variable;
    std::string label;
    const Expr* filter_expr = nullptr; ///< Inline WHERE predicate for node filtering.
};

/// Edge definition within a MATCH pattern.
struct MatchEdgeDef {
    std::string variable;
    std::string edge_type;
    TraverseDirection direction = TraverseDirection::OUT;
    std::optional<int32_t> min_hops;   ///< Minimum hops (nullopt = fixed single hop).
    std::optional<int32_t> max_hops;   ///< Maximum hops (nullopt = unbounded).
    const Expr* filter_expr = nullptr; ///< Inline WHERE predicate for edge filtering.

    /// Default constructor.
    MatchEdgeDef() = default;

    /// Backward-compatible constructor (fixed single-hop edge).
    MatchEdgeDef(std::string var, std::string etype, TraverseDirection dir)
        : variable(std::move(var)), edge_type(std::move(etype)), direction(dir) {}

    /// Full constructor with hop quantifiers.
    MatchEdgeDef(std::string var,
                 std::string etype,
                 TraverseDirection dir,
                 std::optional<int32_t> min_h,
                 std::optional<int32_t> max_h)
        : variable(std::move(var)), edge_type(std::move(etype)), direction(dir), min_hops(min_h),
          max_hops(max_h) {}

    /// Return true if this edge has a variable-length quantifier.
    [[nodiscard]] bool is_variable_length() const { return min_hops.has_value(); }
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
                         const BoundStatement& bound,
                         const std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes = nullptr,
                         const std::unordered_map<index_id_t, HashIndex*>* hash_indexes = nullptr);

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

    /// Evaluate a node's inline predicate against its fetched data.
    Result<bool> evaluate_node_filter(const MatchNodeDef& node_def, const Value& pk) const;

    /// Evaluate an edge's inline predicate against edge properties.
    Result<bool> evaluate_edge_filter(const MatchEdgeDef& edge_def,
                                      const std::vector<Value>& properties) const;

    GraphEngine& graph_engine_;
    const Catalog& catalog_;
    StorageManager& storage_;
    database_id_t database_id_;
    MatchConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;
    const std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes_;
    const std::unordered_map<index_id_t, HashIndex*>* hash_indexes_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
