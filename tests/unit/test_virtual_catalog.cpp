#include "sixseven/catalog/catalog.h"
#include "sixseven/executor/virtual_catalog_scan.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {

// ===========================================================================
// Helper: parse SQL to AST
// ===========================================================================

static StmtPtr parse(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmt = parser.parse();
    if (!stmt)
        return nullptr;
    return std::move(*stmt);
}

// ===========================================================================
// Catalog: virtual table registration and lookup
// ===========================================================================

TEST(VirtualCatalog, RegisterAndLookup) {
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def;
    def.name = "pg_database";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "datname", TypeId::STRING, false, ""},
    };
    def.generator = []() -> std::vector<std::vector<std::string>> {
        return {{"1", "demo"}, {"2", "sixseven_system"}};
    };
    catalog.register_virtual_table(std::move(def));

    auto vt = catalog.get_virtual_table("pg_database");
    ASSERT_TRUE(vt.has_value()) << vt.error().message;
    EXPECT_EQ(vt->name, "pg_database");
    EXPECT_EQ(vt->columns.size(), 2u);
    EXPECT_NE(vt->table_id, 0);
}

TEST(VirtualCatalog, LookupUnknownTableReturnsError) {
    Catalog catalog;
    init_test_catalog(catalog);

    auto vt = catalog.get_virtual_table("pg_nonexistent");
    ASSERT_FALSE(vt.has_value());
    EXPECT_EQ(vt.error().code, StatusCode::NOT_FOUND);
}

TEST(VirtualCatalog, IsVirtualSchema) {
    EXPECT_TRUE(Catalog::is_virtual_schema("pg_catalog"));
    EXPECT_FALSE(Catalog::is_virtual_schema("public"));
    EXPECT_FALSE(Catalog::is_virtual_schema(""));
}

TEST(VirtualCatalog, IsVirtualTable) {
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def;
    def.name = "pg_type";
    def.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def.generator = []() { return std::vector<std::vector<std::string>>{}; };
    catalog.register_virtual_table(std::move(def));

    auto vt = catalog.get_virtual_table("pg_type");
    ASSERT_TRUE(vt.has_value());
    EXPECT_TRUE(catalog.is_virtual_table(vt->table_id));
    EXPECT_FALSE(catalog.is_virtual_table(999));
}

TEST(VirtualCatalog, IsVirtualTableIdSetStaysInSync) {
    // Verify that each registered virtual table's ID is tracked in the O(1) set,
    // and that IDs not registered are absent — guarding against staleness bugs.
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def1;
    def1.name = "pg_tables";
    def1.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def1.generator = []() { return std::vector<std::vector<std::string>>{}; };
    catalog.register_virtual_table(std::move(def1));

    VirtualTableDef def2;
    def2.name = "pg_indexes";
    def2.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def2.generator = []() { return std::vector<std::vector<std::string>>{}; };
    catalog.register_virtual_table(std::move(def2));

    auto vt1 = catalog.get_virtual_table("pg_tables");
    auto vt2 = catalog.get_virtual_table("pg_indexes");
    ASSERT_TRUE(vt1.has_value());
    ASSERT_TRUE(vt2.has_value());

    // Both registered IDs must be found.
    EXPECT_TRUE(catalog.is_virtual_table(vt1->table_id));
    EXPECT_TRUE(catalog.is_virtual_table(vt2->table_id));

    // Unregistered or physical IDs must not be found.
    EXPECT_FALSE(catalog.is_virtual_table(0));
    EXPECT_FALSE(catalog.is_virtual_table(1));
    EXPECT_FALSE(catalog.is_virtual_table(42));
    // IDs in the virtual range but not registered must also be absent.
    EXPECT_FALSE(catalog.is_virtual_table(vt2->table_id - 1));
}

TEST(VirtualCatalog, ListVirtualTables) {
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def1;
    def1.name = "pg_database";
    def1.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def1.generator = []() { return std::vector<std::vector<std::string>>{}; };
    catalog.register_virtual_table(std::move(def1));

    VirtualTableDef def2;
    def2.name = "pg_type";
    def2.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def2.generator = []() { return std::vector<std::vector<std::string>>{}; };
    catalog.register_virtual_table(std::move(def2));

    auto tables = catalog.list_virtual_tables();
    EXPECT_EQ(tables.size(), 2u);
}

TEST(VirtualCatalog, VirtualTableIdsAreNegative) {
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def;
    def.name = "pg_database";
    def.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def.generator = []() { return std::vector<std::vector<std::string>>{}; };
    catalog.register_virtual_table(std::move(def));

    auto vt = catalog.get_virtual_table("pg_database");
    ASSERT_TRUE(vt.has_value());
    EXPECT_LT(vt->table_id, 0);
}

// ===========================================================================
// VirtualCatalogScan iterator
// ===========================================================================

TEST(VirtualCatalogScan, ProducesExpectedRows) {
    VirtualTableDef def;
    def.table_id = -1000;
    def.name = "pg_database";
    def.columns = {
        {0, "oid", TypeId::INT32, false, ""},
        {1, "datname", TypeId::STRING, false, ""},
    };
    def.generator = []() -> std::vector<std::vector<std::string>> {
        return {{"1", "db_one"}, {"2", "db_two"}, {"3", "db_three"}};
    };

    OutputSchema schema(std::vector<OutputColumn>{
        {"pg_database", "oid", TypeId::INT32, false, def.table_id},
        {"pg_database", "datname", TypeId::STRING, false, def.table_id},
    });

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));

    auto open_result = scan.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::pair<int32_t, std::string>> rows;
    while (true) {
        auto next_result = scan.next();
        ASSERT_TRUE(next_result.has_value()) << next_result.error().message;
        if (!next_result->has_value())
            break;
        auto& tuple = next_result->value();
        ASSERT_EQ(tuple.values.size(), 2u);
        rows.emplace_back(tuple.values[0].as_int32(), tuple.values[1].as_string());
    }

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[0].second, "db_one");
    EXPECT_EQ(rows[1].first, 2);
    EXPECT_EQ(rows[1].second, "db_two");
    EXPECT_EQ(rows[2].first, 3);
    EXPECT_EQ(rows[2].second, "db_three");

    scan.close();
}

TEST(VirtualCatalogScan, EmptyTable) {
    VirtualTableDef def;
    def.table_id = -1001;
    def.name = "pg_empty";
    def.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def.generator = []() { return std::vector<std::vector<std::string>>{}; };

    OutputSchema schema(
        std::vector<OutputColumn>{{"pg_empty", "oid", TypeId::INT32, false, def.table_id}});

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));

    auto open_result = scan.open();
    ASSERT_TRUE(open_result.has_value());

    auto next_result = scan.next();
    ASSERT_TRUE(next_result.has_value());
    EXPECT_FALSE(next_result->has_value());

    scan.close();
}

TEST(VirtualCatalogScan, PlanNodeMetadata) {
    VirtualTableDef def;
    def.table_id = -1000;
    def.name = "pg_database";
    def.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def.generator = []() { return std::vector<std::vector<std::string>>{}; };

    OutputSchema schema(
        std::vector<OutputColumn>{{"pg_database", "oid", TypeId::INT32, false, def.table_id}});

    VirtualCatalogScanOperator scan(std::move(def), std::move(schema));
    EXPECT_EQ(scan.plan_node_name(), "Virtual Catalog Scan");
    EXPECT_EQ(scan.plan_node_detail(), "pg_catalog.pg_database");
}

// ===========================================================================
// Binder: pg_catalog table resolution
// ===========================================================================

class VirtualCatalogBinderTest : public ::testing::Test {
protected:
    Catalog catalog;

    void SetUp() override {
        init_test_catalog(catalog);

        VirtualTableDef def;
        def.name = "pg_database";
        def.columns = {
            {0, "oid", TypeId::INT32, false, ""},
            {1, "datname", TypeId::STRING, false, ""},
        };
        def.generator = []() -> std::vector<std::vector<std::string>> { return {{"1", "demo"}}; };
        catalog.register_virtual_table(std::move(def));
    }
};

TEST_F(VirtualCatalogBinderTest, SelectFromPgCatalogBindsSuccessfully) {
    auto stmt = parse("SELECT oid, datname FROM pg_catalog.pg_database");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 2u);
    EXPECT_EQ(result->output_columns[0].column_name, "oid");
    EXPECT_EQ(result->output_columns[1].column_name, "datname");
}

TEST_F(VirtualCatalogBinderTest, SelectStarFromPgCatalog) {
    auto stmt = parse("SELECT * FROM pg_catalog.pg_database");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->output_columns.size(), 2u);
}

TEST_F(VirtualCatalogBinderTest, UnknownPgCatalogTableReturnsError) {
    auto stmt = parse("SELECT * FROM pg_catalog.pg_nonexistent");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(VirtualCatalogBinderTest, UnqualifiedTableStillResolvesFromPublic) {
    TableSchema s;
    s.name = "users";
    s.columns = {{0, "id", TypeId::INT32, false, ""}};
    s.pk_columns = "id";
    auto r = catalog.create_table(default_database_id, std::move(s));
    ASSERT_TRUE(r.has_value());

    auto stmt = parse("SELECT id FROM users");
    ASSERT_NE(stmt, nullptr);

    Binder binder(catalog);
    auto result = binder.bind(*stmt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// ===========================================================================
// Parser: schema-qualified table names
// ===========================================================================

TEST(VirtualCatalogParser, ParsesSchemaQualifiedTableRef) {
    auto stmt = parse("SELECT * FROM pg_catalog.pg_database");
    ASSERT_NE(stmt, nullptr);
    auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].schema, "pg_catalog");
    EXPECT_EQ(sel->from[0].name, "pg_database");
}

TEST(VirtualCatalogParser, UnqualifiedTableHasEmptySchema) {
    auto stmt = parse("SELECT * FROM users");
    ASSERT_NE(stmt, nullptr);
    auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_TRUE(sel->from[0].schema.empty());
    EXPECT_EQ(sel->from[0].name, "users");
}

TEST(VirtualCatalogParser, SchemaQualifiedWithAlias) {
    auto stmt = parse("SELECT d.datname FROM pg_catalog.pg_database AS d");
    ASSERT_NE(stmt, nullptr);
    auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    EXPECT_EQ(sel->from[0].schema, "pg_catalog");
    EXPECT_EQ(sel->from[0].name, "pg_database");
    EXPECT_EQ(sel->from[0].alias, "d");
}

} // namespace sixseven
