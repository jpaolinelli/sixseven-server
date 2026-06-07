#pragma once

#include "sixseven/catalog/catalog.h"

namespace sixseven {

/// Restore the default database in an existing Catalog for use in unit tests.
inline void init_test_catalog(Catalog& catalog) {
    auto r = catalog.restore_database(default_database_id, "demo");
    (void)r;
}

} // namespace sixseven
