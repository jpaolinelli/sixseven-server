#include "sixseven/executor/shortest_path.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/executor/graph_traversal_core.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

ShortestPathOperator::ShortestPathOperator(GraphEngine& graph_engine,
                                           ShortestPathConfig config,
                                           OutputSchema schema)
    : graph_engine_(graph_engine), config_(std::move(config)), schema_(std::move(schema)) {}

std::string ShortestPathOperator::plan_node_name() const {
    return "Shortest Path";
}
std::string ShortestPathOperator::plan_node_detail() const {
    return "";
}

Result<void> ShortestPathOperator::do_open() {
    results_.clear();
    cursor_ = 0;
    return run_bidirectional_bfs();
}

Result<std::optional<Tuple>> ShortestPathOperator::do_next() {
    if (cursor_ < results_.size()) {
        return ok(std::make_optional(std::move(results_[cursor_++])));
    }
    return ok(std::optional<Tuple>(std::nullopt));
}

void ShortestPathOperator::do_close() {
    results_.clear();
    cursor_ = 0;
}

const OutputSchema& ShortestPathOperator::output_schema() const {
    return schema_;
}

Result<void> ShortestPathOperator::run_bidirectional_bfs() {
    // For heterogeneous edges (different source/target tables) we key the
    // visited/parent maps by (table_id, pk) to avoid conflating nodes from
    // different tables that share a PK value (GDB-842).  For homogeneous edges
    // the table_id is the same on both sides and the distinction is
    // irrelevant — bare-pk keying would be equivalent — so we unify on the
    // table-aware key in all cases.  The only observable difference vs. the
    // old code is the trivial-case check and the seeding below.

    using ParentMap = std::unordered_map<NodeId, NodeId, NodeIdHash>; // node -> its parent NodeId
    using VisitedSet = std::unordered_set<NodeId, NodeIdHash>;

    // For homogeneous edges source_table_id == target_table_id == the single
    // shared table.  For heterogeneous edges the table of from_key / to_key
    // depends on the query direction:
    //   OUT:  from_key lives in source_table, to_key lives in target_table.
    //   IN:   from_key lives in target_table, to_key lives in source_table.
    // BOTH on heterogeneous edges is rejected by the planner (GDB-842), so
    // we only need to handle OUT and IN here.
    const bool is_in = config_.direction == TraverseDirection::IN;
    const table_id_t fwd_table = is_in ? config_.target_table_id : config_.source_table_id;
    const table_id_t bwd_table = is_in ? config_.source_table_id : config_.target_table_id;

    NodeId from_node{fwd_table, config_.from_key};
    NodeId to_node{bwd_table, config_.to_key};

    // Trivial case: source == target.
    // For heterogeneous edges this is ONLY a zero-hop path when both the table
    // AND the pk match (GDB-842 defect 1).  For homogeneous edges
    // source_table_id == target_table_id so the table check is satisfied
    // automatically — the behaviour is unchanged.
    if (from_node == to_node) {
        Tuple t;
        t.values = {config_.from_key, Value(static_cast<int64_t>(0))};
        results_.push_back(std::move(t));
        return ok();
    }

    // Forward BFS from source.
    std::deque<NodeId> fwd_queue;
    ParentMap fwd_parent;
    VisitedSet fwd_visited;

    fwd_queue.push_back(from_node);
    fwd_visited.insert(from_node);
    fwd_parent[from_node] = NodeId{0, Value()}; // Sentinel: null parent.

    // Backward BFS from target.
    std::deque<NodeId> bwd_queue;
    ParentMap bwd_parent;
    VisitedSet bwd_visited;

    bwd_queue.push_back(to_node);
    bwd_visited.insert(to_node);
    bwd_parent[to_node] = NodeId{0, Value()}; // Sentinel.

    // Determine directions for forward and backward BFS.
    // Forward BFS follows the configured direction.
    // Backward BFS follows the reverse direction.
    TraverseDirection fwd_dir = config_.direction;
    TraverseDirection bwd_dir = TraverseDirection::IN;
    if (config_.direction == TraverseDirection::IN) {
        bwd_dir = TraverseDirection::OUT;
    } else if (config_.direction == TraverseDirection::BOTH) {
        bwd_dir = TraverseDirection::BOTH;
    }

    NodeId meeting_node;
    bool found = false;
    int32_t depth = 0;

    while (!fwd_queue.empty() && !bwd_queue.empty() && depth < config_.max_depth && !found) {
        ++depth;

        // Expand the smaller frontier.
        bool expand_forward = fwd_queue.size() <= bwd_queue.size();

        auto& queue = expand_forward ? fwd_queue : bwd_queue;
        auto& parent = expand_forward ? fwd_parent : bwd_parent;
        auto& visited = expand_forward ? fwd_visited : bwd_visited;
        auto& other_visited = expand_forward ? bwd_visited : fwd_visited;
        TraverseDirection dir = expand_forward ? fwd_dir : bwd_dir;

        // For heterogeneous edges the "other side" table is the opposite
        // table; for homogeneous it's the same table throughout.
        const table_id_t nbr_table = expand_forward ? bwd_table : fwd_table;

        size_t level_size = queue.size();
        for (size_t i = 0; i < level_size && !found; ++i) {
            auto current = std::move(queue.front());
            queue.pop_front();

            auto neighbors = get_neighbors(current.pk, dir);
            if (!neighbors) {
                return tl::unexpected(neighbors.error());
            }

            for (auto& nbr_pk : *neighbors) {
                // Neighbors discovered from the forward frontier live in
                // bwd_table (target table), and vice-versa, for heterogeneous
                // edges.  For homogeneous edges both sides share the same
                // table so nbr_table == current.table_id throughout.
                NodeId nbr{nbr_table, nbr_pk};

                if (visited.count(nbr) > 0) {
                    continue;
                }

                visited.insert(nbr);
                parent[nbr] = current;

                // Enforce max_visited budget (GDB-995).
                if (fwd_visited.size() + bwd_visited.size() > config_.max_visited) {
                    return tl::unexpected(make_error(StatusCode::INVALID_ARGUMENT,
                                                     "shortest path exceeded max_visited limit (" +
                                                         std::to_string(config_.max_visited) +
                                                         ")"));
                }

                // Check if this node was visited by the other BFS.
                // GDB-842 defect 2: with bare-pk keying a node from one table
                // could falsely match a node from the other table with the
                // same pk.  The NodeId comparison includes table_id so this
                // is now safe.
                if (other_visited.count(nbr) > 0) {
                    meeting_node = nbr;
                    found = true;
                    break;
                }

                queue.push_back(nbr);
            }
        }
    }

    if (!found) {
        // No path exists — return empty result set.
        return ok();
    }

    // Reconstruct path: forward half (source → meeting).
    std::vector<Value> fwd_path;
    {
        NodeId node = meeting_node;
        while (!node.pk.is_null()) {
            fwd_path.push_back(node.pk);
            auto it = fwd_parent.find(node);
            if (it == fwd_parent.end()) {
                break;
            }
            node = it->second;
        }
        std::reverse(fwd_path.begin(), fwd_path.end());
    }

    // Backward half (meeting → target), excluding meeting node.
    std::vector<Value> bwd_path;
    {
        auto it = bwd_parent.find(meeting_node);
        if (it != bwd_parent.end()) {
            NodeId node = it->second;
            while (!node.pk.is_null()) {
                bwd_path.push_back(node.pk);
                auto pit = bwd_parent.find(node);
                if (pit == bwd_parent.end()) {
                    break;
                }
                node = pit->second;
            }
        }
    }

    // Combine: fwd_path + bwd_path.
    int64_t hop = 0;
    for (auto& pk : fwd_path) {
        Tuple t;
        t.values = {std::move(pk), Value(hop)};
        results_.push_back(std::move(t));
        ++hop;
    }
    for (auto& pk : bwd_path) {
        Tuple t;
        t.values = {std::move(pk), Value(hop)};
        results_.push_back(std::move(t));
        ++hop;
    }

    return ok();
}

Result<std::vector<Value>> ShortestPathOperator::get_neighbors(const Value& node_pk,
                                                               TraverseDirection dir) const {
    auto pairs =
        expand_neighbors_ids(graph_engine_, config_.database_id, config_.edge_type, node_pk, dir);
    if (!pairs) {
        return tl::unexpected(pairs.error());
    }
    std::vector<Value> result;
    result.reserve(pairs->size());
    for (auto& [pk, /*edge_id*/ unused] : *pairs) {
        result.push_back(std::move(pk));
    }
    return ok(std::move(result));
}

} // namespace sixseven
