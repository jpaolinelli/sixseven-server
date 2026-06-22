#pragma once

#include "sixseven/catalog/schema.h"

namespace sixseven {

class Catalog;

VirtualTableDef make_pg_database(Catalog& catalog);
VirtualTableDef make_pg_namespace();
VirtualTableDef make_pg_type();
VirtualTableDef make_pg_class(Catalog& catalog);
VirtualTableDef make_pg_attribute(Catalog& catalog);
VirtualTableDef make_pg_index(Catalog& catalog);

void register_pg_catalog_tables(Catalog& catalog);

} // namespace sixseven
