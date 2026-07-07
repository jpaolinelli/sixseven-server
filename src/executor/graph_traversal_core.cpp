#include "sixseven/executor/graph_traversal_core.h"

#include <algorithm>

namespace sixseven {

// ---------------------------------------------------------------------------
// pk_to_int64
// ---------------------------------------------------------------------------

Result<int64_t> pk_to_int64(const Value& pk) {
    // GDB-1224: widened to accept every integer PK width (INT8/INT16/INT32/
    // INT64, UINT8/UINT16/UINT32/UINT64), not just INT32/INT64. All of these
    // losslessly represent as int64_t except UINT64 values above INT64_MAX,
    // which wrap via static_cast the same way the rest of the codebase's
    // integer-to-int64 conversion path (to_int64() in
    // src/common/coercion.cpp) already accepts.
    //
    // GDB-1292: PathStep::node_pk is now a Value (see
    // include/sixseven/common/value.h), so this function is no longer used to
    // build PathStep entries -- STRING and other non-integer PK types flow
    // through as Value directly. pk_to_int64 is retained for call sites that
    // still need a genuine int64_t (e.g. table-aware NodeId hashing paths
    // that pre-date GDB-1292) and continues to reject non-integer PKs for
    // those numeric-only use sites.
    if (!pk.is_null()) {
        if (const auto* p = std::get_if<int64_t>(&pk.data())) {
            return ok(*p);
        }
        if (const auto* p32 = std::get_if<int32_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*p32));
        }
        if (const auto* p16 = std::get_if<int16_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*p16));
        }
        if (const auto* p8 = std::get_if<int8_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*p8));
        }
        if (const auto* u8 = std::get_if<uint8_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*u8));
        }
        if (const auto* u16 = std::get_if<uint16_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*u16));
        }
        if (const auto* u32 = std::get_if<uint32_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*u32));
        }
        if (const auto* u64 = std::get_if<uint64_t>(&pk.data())) {
            return ok(static_cast<int64_t>(*u64));
        }
    }
    return make_error(StatusCode::INVALID_ARGUMENT,
                      "graph traversal requires integer primary keys");
}

// ---------------------------------------------------------------------------
// expand_neighbors — (neighbor_pk, EdgeRow) overload
// ---------------------------------------------------------------------------

Result<std::vector<std::pair<Value, EdgeRow>>> expand_neighbors(GraphEngine& graph_engine,
                                                                database_id_t database_id,
                                                                const std::string& edge_type,
                                                                const Value& node_pk,
                                                                TraverseDirection direction) {
    std::vector<std::pair<Value, EdgeRow>> result;

    // OUT branch: edges where node_pk is the source; neighbor = target_pk.
    // This is the C6/C7 fix: for BOTH direction we call both branches and
    // resolve the neighbor PK at collection time from the correct field
    // (target_pk for outgoing rows, source_pk for incoming rows) rather than
    // re-deriving it later from the direction enum — which would be wrong when
    // the direction enum is BOTH.
    if (direction == TraverseDirection::OUT || direction == TraverseDirection::BOTH) {
        auto edges = graph_engine.get_edges_from(database_id, edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.emplace_back(edge.target_pk, std::move(edge));
        }
    }

    // IN branch: edges where node_pk is the target; neighbor = source_pk.
    if (direction == TraverseDirection::IN || direction == TraverseDirection::BOTH) {
        auto edges = graph_engine.get_edges_to(database_id, edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& edge : *edges) {
            result.emplace_back(edge.source_pk, std::move(edge));
        }
    }

    return ok(std::move(result));
}

// ---------------------------------------------------------------------------
// expand_neighbors_ids — (neighbor_pk, edge_row_id) overload
// ---------------------------------------------------------------------------

Result<std::vector<std::pair<Value, int64_t>>> expand_neighbors_ids(GraphEngine& graph_engine,
                                                                    database_id_t database_id,
                                                                    const std::string& edge_type,
                                                                    const Value& node_pk,
                                                                    TraverseDirection direction) {
    std::vector<std::pair<Value, int64_t>> result;

    if (direction == TraverseDirection::OUT || direction == TraverseDirection::BOTH) {
        auto edges = graph_engine.get_edges_from(database_id, edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& e : *edges) {
            result.emplace_back(std::move(e.target_pk), static_cast<int64_t>(e.edge_row_id));
        }
    }

    if (direction == TraverseDirection::IN || direction == TraverseDirection::BOTH) {
        auto edges = graph_engine.get_edges_to(database_id, edge_type, node_pk);
        if (!edges) {
            return tl::unexpected(edges.error());
        }
        for (auto& e : *edges) {
            result.emplace_back(std::move(e.source_pk), static_cast<int64_t>(e.edge_row_id));
        }
    }

    return ok(std::move(result));
}

// ---------------------------------------------------------------------------
// reconstruct_path
// ---------------------------------------------------------------------------

Result<Path>
reconstruct_path(const Value& target, int32_t target_depth, const ParentMap& parent_map) {
    // A node can never legitimately have a NULL primary key, so reject it up
    // front. Prior to GDB-1292 this validation happened implicitly as a side
    // effect of pk_to_int64() (which erred on NULL); since PathStep::node_pk
    // is now a Value and no longer requires that conversion, the NULL check
    // must be explicit here to preserve the existing error contract.
    if (target.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "TRACE path reconstruction requires a non-NULL target primary key");
    }

    // Walk parent pointers backward from target to the start node, collecting
    // (node_pk, incoming_edge_row_id) pairs, then reverse to get start->target.
    std::vector<PathStep> reversed;

    // H11 (GDB-694): a valid parent chain consumes each parent-map entry at
    // most once plus a terminal step for the start node.  If more steps are
    // taken the parent map contains a cycle and we return an error.
    const size_t max_steps = parent_map.size() + 1;

    Value cursor = target;
    int32_t remaining = target_depth;
    while (true) {
        if (reversed.size() >= max_steps) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "TRACE path reconstruction detected a cycle in the parent map");
        }

        // GDB-1292: PathStep::node_pk is a Value, so the PK is stored
        // directly -- no int64 conversion (and no integer-only restriction)
        // is needed to build a path step.

        // H9 (GDB-694): after target_depth hops the cursor must be the start
        // node.  The depth guard terminates the walk even when the parent map
        // contains a cross-table PK collision that would otherwise make the
        // start node appear to have a parent, causing an infinite loop.
        if (remaining <= 0) {
            reversed.push_back({cursor, -1});
            break;
        }

        auto it = parent_map.find(cursor);
        if (it == parent_map.end()) {
            // Reached the start node (no parent entry): no incoming edge.
            reversed.push_back({cursor, -1});
            break;
        }

        reversed.push_back({cursor, it->second.edge_row_id});
        cursor = it->second.parent_pk;
        if (cursor.is_null()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "TRACE path reconstruction encountered a NULL parent primary key");
        }
        --remaining;
    }

    // reversed is target..start.  Reverse to get start..target.
    std::reverse(reversed.begin(), reversed.end());

    // Each step carries its own *incoming* edge id.  Path semantics want the
    // *outgoing* edge to the next step, so shift edge ids one position toward
    // the start and clear the terminal step's edge.
    Path path;
    path.steps.reserve(reversed.size());
    for (size_t i = 0; i < reversed.size(); ++i) {
        int64_t outgoing = (i + 1 < reversed.size()) ? reversed[i + 1].edge_id : -1;
        path.steps.push_back({reversed[i].node_pk(), outgoing});
    }

    return ok(std::move(path));
}

} // namespace sixseven
