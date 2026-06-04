#pragma once

#include "sixseven/common/result.h"

namespace sixseven {

// Forward declaration.
class QueryEngine;

/// Populate the default database with the "SixSeven Bookstore" demo dataset.
///
/// Called once on first server startup (after system bootstrap completes and
/// the default database is current).  Creates tables, edge types, indexes,
/// and seeds realistic data so new users can immediately explore relational,
/// graph, and vector (NEAREST) queries without any setup.
///
/// The function is non-fatal: callers should log a warning and continue if it
/// returns an error.
///
/// @param engine  QueryEngine pointing at the default user database.
/// @return        ok() on success, or an error describing the first failure.
[[nodiscard]] Result<void> create_demo_database(QueryEngine& engine);

/// Build the demo's secondary B-tree indexes over the already-seeded tables.
///
/// MUST be called only after (a) all rows are inserted and (b) all EMBEDDING
/// vectors have been generated. Embedding generation rewrites each row to add
/// its (large) vector, which moves the tuple to a new heap slot; building the
/// indexes beforehand would leave them pointing at deleted slots. INSERT/UPDATE
/// do not maintain secondary indexes, so creating them last — once the heap is
/// stable — is what makes them correct and populated.
///
/// @param engine  QueryEngine pointing at the default user database.
/// @return        ok() on success, or an error describing the first failure.
[[nodiscard]] Result<void> create_demo_indexes(QueryEngine& engine);

} // namespace sixseven
