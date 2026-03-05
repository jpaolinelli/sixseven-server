#pragma once

#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

/// Configuration for a shortest path query.
struct ShortestPathConfig {
    std::string edge_type;
    Value from_key;
    Value to_key;
    TraverseDirection direction = TraverseDirection::OUT;
    int32_t max_depth = 100;
    size_t max_visited = 100000;
};

/// Shortest path executor operator.
///
/// Finds the minimum-hop path between two nodes using bidirectional BFS.
/// Returns an ordered sequence of (node_pk, hop) tuples representing the path.
///
/// The operator materialises all results during open(), then emits
/// them one at a time from next().
class ShortestPathOperator : public Iterator {
public:
    ShortestPathOperator(GraphEngine& graph_engine, ShortestPathConfig config, OutputSchema schema);

    const OutputSchema& output_schema() const override;

    // -- Plan inspection (for EXPLAIN) ----------------------------------------
    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Run bidirectional BFS and populate results_.
    Result<void> run_bidirectional_bfs();

    /// Get neighbor PKs in the given direction.
    Result<std::vector<Value>> get_neighbors(const Value& node_pk, TraverseDirection dir) const;

    GraphEngine& graph_engine_;
    ShortestPathConfig config_;
    OutputSchema schema_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
