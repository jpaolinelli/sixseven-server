#pragma once

/// @file test_qa_helpers.h
/// @brief Shared bootstrap helpers for QA test fixtures (GDB-713).
///
/// Every QA fixture that constructs a bare sixseven::Catalog and then uses it
/// with QueryEngine (or calls catalog methods scoped to default_database_id)
/// MUST call bootstrap_qa_catalog() in SetUp() before executing any SQL.
///
/// Background: Catalog::Catalog() only registers the system database (id=2).
/// The default "demo" database (id=1) is normally created by
/// SystemBootstrap::bootstrap(). In unit/QA tests that skip the full bootstrap,
/// restore_database() must be called to register id=1 so that the binder and
/// storage manager can resolve default-database table references.

#include "sixseven/catalog/catalog.h"

namespace sixseven {

/// Register the default "demo" database (id=1) in a freshly constructed
/// Catalog. Call this from fixture SetUp() before any SQL execution.
///
/// Idempotent: if the database already exists the error is silently ignored,
/// so it is safe to call from fixtures that may also run SystemBootstrap.
inline void bootstrap_qa_catalog(Catalog& catalog) {
    (void)catalog.restore_database(default_database_id, default_database_name);
}

} // namespace sixseven
