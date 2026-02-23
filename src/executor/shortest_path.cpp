#include "giodb/executor/shortest_path.h"

#include "giodb/common/coercion.h"
#include "giodb/common/value_hash.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace giodb {

ShortestPathOperator::ShortestPathOperator(GraphEngine& graph_engine,
                                           ShortestPathConfig config,
                                           OutputSchema schema)
    : graph_engine_(graph_engine), config_(std::move(config)), schema_(std::move(schema)) {}

Result<void> ShortestPathOperator::open() {
    results_.clear();
    cursor_ = 0;
    return run_bidirectional_bfs();
}

Result<std::optional<Tuple>> ShortestPathOperator::next() {
    if (cursor_ < results_.size()) {
        return ok(std::make_optional(std::move(results_[cursor_++])));
    }
    return ok(std::optional<Tuple>(std::nullopt));
}

void ShortestPathOperator::close() {
    results_.clear();
    cursor_ = 0;
}

const OutputSchema& ShortestPathOperator::output_schema() const {
    return schema_;
}

Result<void> ShortestPathOperator::run_bidirectional_bfs() {
    using ParentMap = std::unordered_map<Value, Value, ValueHash, ValueEqual>;
    using VisitedSet = std::unordered_set<Value, ValueHash, ValueEqual>;

    // Check trivial case: source == target.
    if (ValueEqual{}(config_.from_key, config_.to_key)) {
        Tuple t;
        t.values = {config_.from_key, Value(static_cast<int64_t>(0))};
        results_.push_back(std::move(t));
        return ok();
    }

    // Forward BFS from source.
    std::deque<Value> fwd_queue;
    ParentMap fwd_parent;
    VisitedSet fwd_visited;

    fwd_queue.push_back(config_.from_key);
    fwd_visited.insert(config_.from_key);
    fwd_parent[config_.from_key] = Value(); // Sentinel: null parent.

    // Backward BFS from target.
    std::deque<Value> bwd_queue;
    ParentMap bwd_parent;
    VisitedSet bwd_visited;

    bwd_queue.push_back(config_.to_key);
    bwd_visited.insert(config_.to_key);
    bwd_parent[config_.to_key] = Value(); // Sentinel.

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

    Value meeting_node;
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

        size_t level_size = queue.size();
        for (size_t i = 0; i < level_size && !found; ++i) {
            auto current = std::move(queue.front());
            queue.pop_front();

            auto neighbors = get_neighbors(current, dir);
            if (!neighbors) {
                return tl::unexpected(neighbors.error());
            }

            for (auto& nbr : *neighbors) {
                if (visited.count(nbr) > 0) {
                    continue;
                }

                visited.insert(nbr);
                parent[nbr] = current;

                // Check if this node was visited by the other BFS.
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
        Value node = meeting_node;
        while (!node.is_null()) {
            fwd_path.push_back(node);
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
            Value node = it->second;
            while (!node.is_null()) {
                bwd_path.push_back(node);
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
    std::vector<Value> result;

    if (dir == TraverseDirection::OUT || dir == TraverseDirection::BOTH) {
        auto edges = graph_engine_.get_edges_from(config_.edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.push_back(std::move(edge.target_pk));
        }
    }

    if (dir == TraverseDirection::IN || dir == TraverseDirection::BOTH) {
        auto edges = graph_engine_.get_edges_to(config_.edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.push_back(std::move(edge.source_pk));
        }
    }

    return ok(std::move(result));
}

} // namespace giodb
