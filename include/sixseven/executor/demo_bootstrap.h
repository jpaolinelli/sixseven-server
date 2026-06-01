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

} // namespace sixseven
