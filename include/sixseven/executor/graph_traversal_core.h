#pragma once

/// @file graph_traversal_core.h
/// @brief Shared utilities for all graph traversal operators.
///
/// Consolidates the direction-aware neighbor-expansion, path-reconstruction,
/// and table-aware node-identity helpers that were previously duplicated
/// across edge_traversal, enriched_traversal, traversal, shortest_path,
/// match_shortest_path, pattern_match, and variable_length_match (GDB-894).
///
/// Bug fixes now live in exactly one place (AC2):
///   H9/H11 (GDB-694/GDB-696) — reconstruct_path depth guard + max_steps
///     cycle guard; BFS visited-set seeding conditional on heterogeneous flag.
///   C6/C7  — BOTH-direction expansion: get_edges_from for OUT neighbors,
///     get_edges_to for IN neighbors, resolved at collection time so that
///     target_pk / source_pk are always correct regardless of direction enum.

#include "sixseven/common/result.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// pk_to_int64
// ---------------------------------------------------------------------------

/// Widen an integer-typed Value PK to int64_t.
///
/// Returns an error if the PK is null or not an integer type (INT8..INT64 /
/// UINT8..UINT64). Previously duplicated as a file-local function in five
/// operator .cpp files; now lives here once (H9/H11, GDB-694/GDB-696).
///
/// GDB-1292: PathStep::node_pk is now a Value (not int64_t), so path-building
/// call sites no longer need this conversion -- STRING and other
/// non-integer PKs flow through as Value directly. This function is kept for
/// callers that specifically need a genuine int64_t (numeric-only contexts).
[[nodiscard]] Result<int64_t> pk_to_int64(const Value& pk);

// ---------------------------------------------------------------------------
// NodeId — table-aware node identity (C6/C7, GDB-842/GDB-851)
// ---------------------------------------------------------------------------

/// A table-aware node identity: (table_id, pk).
///
/// Keying BFS visited/cost maps by bare PK Value causes cross-table PK
/// collisions when source and target nodes belong to different tables.
/// Using NodeId prevents two nodes from different tables that share the same
/// PK value from being treated as the same node (GDB-842, GDB-851).
///
/// Previously duplicated in shortest_path.cpp and match_shortest_path.cpp;
/// now lives here once (GDB-894 AC2).
struct NodeId {
    table_id_t table_id = 0;
    Value pk;

    bool operator==(const NodeId& other) const {
        return table_id == other.table_id && ValueEqual{}(pk, other.pk);
    }
};

struct NodeIdHash {
    size_t operator()(const NodeId& n) const noexcept {
        size_t h = std::hash<table_id_t>{}(n.table_id);
        h ^= ValueHash{}(n.pk) + 0x9e3779b9U + (h << 6) + (h >> 2);
        return h;
    }
};

// ---------------------------------------------------------------------------
// Direction-aware neighbor expansion
// ---------------------------------------------------------------------------

/// ParentInfo — one entry in a BFS parent map used for path reconstruction.
struct ParentInfo {
    Value parent_pk;          ///< PK of the node this one was first reached from.
    int64_t edge_row_id = -1; ///< Row ID of the edge used to reach this node.
};

/// Parent map type used by BFS operators that support TRACE mode.
using ParentMap = std::unordered_map<Value, ParentInfo, ValueHash, ValueEqual>;

/// Expand neighbors of @p node_pk in @p direction, returning (neighbor_pk,
/// EdgeRow) pairs.  Callers that need edge properties (edge_traversal,
/// enriched_traversal, traversal) use this overload.
///
/// OUT → get_edges_from; IN → get_edges_to; BOTH → both, resolving the
/// neighbor PK from target_pk / source_pk respectively (C6/C7 fix — GDB-894).
[[nodiscard]] Result<std::vector<std::pair<Value, EdgeRow>>>
expand_neighbors(GraphEngine& graph_engine,
                 database_id_t database_id,
                 const std::string& edge_type,
                 const Value& node_pk,
                 TraverseDirection direction);

/// Expand neighbors returning (neighbor_pk, edge_row_id) pairs only.
/// Used by shortest_path, match_shortest_path, and variable_length_match,
/// which need edge IDs for path reconstruction but not full EdgeRow data.
[[nodiscard]] Result<std::vector<std::pair<Value, int64_t>>>
expand_neighbors_ids(GraphEngine& graph_engine,
                     database_id_t database_id,
                     const std::string& edge_type,
                     const Value& node_pk,
                     TraverseDirection direction);

// ---------------------------------------------------------------------------
// Path reconstruction
// ---------------------------------------------------------------------------

/// Reconstruct the full path from the start node to @p target by walking the
/// @p parent_map backward and reversing.  Only valid when TRACE mode is
/// enabled and @p parent_map has been populated by a BFS run.
///
/// @param target        The destination node PK.
/// @param target_depth  The BFS depth of @p target (= number of hops from
///                      start).  The walk stops after this many hops.
/// @param parent_map    Maps each visited node PK to the (parent_pk,
///                      edge_row_id) it was first reached from.
///
/// The depth guard (H9 — GDB-694): after target_depth hops the cursor must be
/// the start node; if not, the walk terminates anyway, preventing infinite
/// loops caused by cross-table PK collisions where a node in another table
/// shares the start node's PK and would appear as its own ancestor.
///
/// The max_steps guard (H11 — GDB-694): the total number of walk steps is
/// bounded by parent_map.size()+1.  If this is exceeded the parent map
/// contains a cycle and INTERNAL_ERROR is returned.
[[nodiscard]] Result<Path>
reconstruct_path(const Value& target, int32_t target_depth, const ParentMap& parent_map);

} // namespace sixseven
