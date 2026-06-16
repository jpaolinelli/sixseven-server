// QA tests for GDB-828: Catalog::is_virtual_table O(1) set optimization
// Verifies semantic equivalence with the old O(n) linear scan, set sync
// robustness, pre-assigned IDs, and all boundary/invalid inputs.

#include "sixseven/catalog/catalog.h"

#include <gtest/gtest.h>

#include <climits>
#include <limits>
#include <thread>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {

// ===========================================================================
// Helper: build a minimal VirtualTableDef with a given name
// ===========================================================================

static VirtualTableDef make_vtdef(const std::string& name) {
    VirtualTableDef def;
    def.name = name;
    def.columns = {{0, "oid", TypeId::INT32, false, ""}};
    def.generator = []() { return std::vector<std::vector<std::string>>{}; };
    return def;
}

// ===========================================================================
// AC1/AC2: Correctness / equivalence — registered tables return true
// ===========================================================================

// Each registered virtual table must be recognised immediately.
TEST(QA_GDB828, RegisteredTableIsVirtual) {
    Catalog catalog;
    init_test_catalog(catalog);

    catalog.register_virtual_table(make_vtdef("pg_database"));
    catalog.register_virtual_table(make_vtdef("pg_namespace"));
    catalog.register_virtual_table(make_vtdef("pg_type"));
    catalog.register_virtual_table(make_vtdef("pg_class"));
    catalog.register_virtual_table(make_vtdef("pg_attribute"));
    catalog.register_virtual_table(make_vtdef("pg_index"));

    for (const std::string& name :
         {"pg_database", "pg_namespace", "pg_type", "pg_class", "pg_attribute", "pg_index"}) {
        auto vt = catalog.get_virtual_table(name);
        ASSERT_TRUE(vt.has_value()) << "get_virtual_table failed for: " << name;
        EXPECT_TRUE(catalog.is_virtual_table(vt->table_id))
            << "is_virtual_table returned false for registered table: " << name;
    }
}

// ===========================================================================
// AC2: Regular user tables must return false
// ===========================================================================

TEST(QA_GDB828, UserTableIdIsNotVirtual) {
    Catalog catalog;
    init_test_catalog(catalog);

    TableSchema s;
    s.name = "users";
    s.columns = {{0, "id", TypeId::INT32, false, ""}};
    s.pk_columns = "id";
    auto r = catalog.create_table(default_database_id, std::move(s));
    ASSERT_TRUE(r.has_value());

    table_id_t uid = *r;
    EXPECT_FALSE(catalog.is_virtual_table(uid));
}

// ===========================================================================
// AC2(c): Non-existent / invalid / 0 / very large IDs must return false
// ===========================================================================

TEST(QA_GDB828, InvalidIdsReturnFalse) {
    Catalog catalog;
    init_test_catalog(catalog);

    // No virtual tables registered yet.
    EXPECT_FALSE(catalog.is_virtual_table(0));
    EXPECT_FALSE(catalog.is_virtual_table(1));
    EXPECT_FALSE(catalog.is_virtual_table(999));
    EXPECT_FALSE(catalog.is_virtual_table(-1));
    EXPECT_FALSE(catalog.is_virtual_table(-999));
    EXPECT_FALSE(catalog.is_virtual_table(std::numeric_limits<table_id_t>::max()));
    EXPECT_FALSE(catalog.is_virtual_table(std::numeric_limits<table_id_t>::min()));
}

// With some virtual tables registered, random unrelated IDs must still return
// false.
TEST(QA_GDB828, UnregisteredIdsReturnFalseAfterRegistrations) {
    Catalog catalog;
    init_test_catalog(catalog);

    catalog.register_virtual_table(make_vtdef("pg_database"));
    catalog.register_virtual_table(make_vtdef("pg_namespace"));

    EXPECT_FALSE(catalog.is_virtual_table(0));
    EXPECT_FALSE(catalog.is_virtual_table(1));
    EXPECT_FALSE(catalog.is_virtual_table(42));
    EXPECT_FALSE(catalog.is_virtual_table(std::numeric_limits<table_id_t>::max()));
    EXPECT_FALSE(catalog.is_virtual_table(std::numeric_limits<table_id_t>::min()));
    // IDs adjacent to first_virtual_table_id that were never assigned
    EXPECT_FALSE(catalog.is_virtual_table(first_virtual_table_id + 1));
    EXPECT_FALSE(catalog.is_virtual_table(first_virtual_table_id - 2));
}

// ===========================================================================
// AC2(d): Boundary — first_virtual_table_id itself
// ===========================================================================

// The very first auto-assigned virtual table ID is first_virtual_table_id.
// It must be visible immediately after registration and absent before.
TEST(QA_GDB828, BoundaryFirstVirtualTableId) {
    Catalog catalog;
    init_test_catalog(catalog);

    // Before registration the boundary ID is absent.
    EXPECT_FALSE(catalog.is_virtual_table(first_virtual_table_id));

    catalog.register_virtual_table(make_vtdef("pg_boundary"));

    auto vt = catalog.get_virtual_table("pg_boundary");
    ASSERT_TRUE(vt.has_value());
    EXPECT_EQ(vt->table_id, first_virtual_table_id);
    EXPECT_TRUE(catalog.is_virtual_table(first_virtual_table_id));
}

// ===========================================================================
// AC2: Pre-assigned table_id path
// register_virtual_table skips the counter when def.table_id != 0.
// The set must still be populated for the pre-assigned ID.
// ===========================================================================

TEST(QA_GDB828, PreAssignedTableIdIsTrackedInSet) {
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def = make_vtdef("pg_custom");
    def.table_id = -5000; // explicitly pre-assigned

    catalog.register_virtual_table(std::move(def));

    EXPECT_TRUE(catalog.is_virtual_table(-5000));
    // The counter's auto-assigned range must not contain the pre-assigned ID.
    EXPECT_FALSE(catalog.is_virtual_table(first_virtual_table_id)); // not yet assigned
}

// Pre-assigned ID == 0 falls through to auto-assign; must still work.
TEST(QA_GDB828, ZeroTableIdIsAutoAssigned) {
    Catalog catalog;
    init_test_catalog(catalog);

    VirtualTableDef def = make_vtdef("pg_zero_id");
    def.table_id = 0; // triggers auto-assign path

    catalog.register_virtual_table(std::move(def));

    auto vt = catalog.get_virtual_table("pg_zero_id");
    ASSERT_TRUE(vt.has_value());
    EXPECT_NE(vt->table_id, 0);
    EXPECT_LT(vt->table_id, 0); // should be in negative range
    EXPECT_TRUE(catalog.is_virtual_table(vt->table_id));
}

// ===========================================================================
// AC3: Sync robustness — each registration visible immediately
// ===========================================================================

TEST(QA_GDB828, SyncEachRegistrationImmediatelyVisible) {
    Catalog catalog;
    init_test_catalog(catalog);

    const std::vector<std::string> names = {"pg_a", "pg_b", "pg_c", "pg_d", "pg_e"};
    std::vector<table_id_t> ids;

    for (const auto& name : names) {
        catalog.register_virtual_table(make_vtdef(name));
        auto vt = catalog.get_virtual_table(name);
        ASSERT_TRUE(vt.has_value()) << "lookup failed for: " << name;
        // Immediately after registration the ID must be in the set.
        EXPECT_TRUE(catalog.is_virtual_table(vt->table_id))
            << "is_virtual_table false immediately after registration of: " << name;
        ids.push_back(vt->table_id);
    }

    // All previously registered IDs must still be there.
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_TRUE(catalog.is_virtual_table(ids[i]))
            << "is_virtual_table false for earlier ID at index " << i;
    }
}

// ===========================================================================
// AC3: The 6 pg_catalog bootstrap tables
// Simulates system_bootstrap: exactly those 6 IDs must be virtual, nothing else.
// ===========================================================================

TEST(QA_GDB828, BootstrapSixPgCatalogTables) {
    Catalog catalog;
    init_test_catalog(catalog);

    const std::vector<std::string> pg_tables = {
        "pg_database", "pg_namespace", "pg_type", "pg_class", "pg_attribute", "pg_index"};

    for (const auto& name : pg_tables) {
        catalog.register_virtual_table(make_vtdef(name));
    }

    std::vector<table_id_t> virtual_ids;
    for (const auto& name : pg_tables) {
        auto vt = catalog.get_virtual_table(name);
        ASSERT_TRUE(vt.has_value());
        EXPECT_TRUE(catalog.is_virtual_table(vt->table_id))
            << "is_virtual_table false for: " << name;
        virtual_ids.push_back(vt->table_id);
    }

    ASSERT_EQ(virtual_ids.size(), 6u);

    // All 6 IDs must be distinct.
    std::unordered_set<table_id_t> id_set(virtual_ids.begin(), virtual_ids.end());
    EXPECT_EQ(id_set.size(), 6u) << "Duplicate virtual table IDs detected";

    // User table IDs in [1..first_user_table_id+100] must all be false.
    for (table_id_t id = 0; id <= first_user_table_id + 100; ++id) {
        EXPECT_FALSE(catalog.is_virtual_table(id))
            << "User-range ID " << id << " incorrectly reported as virtual";
    }

    // IDs outside the 6 registered IDs in the virtual range must be false.
    // The last registered ID is first_virtual_table_id - 5 (6 decrements from -1000).
    table_id_t last_assigned = first_virtual_table_id - 5;
    EXPECT_FALSE(catalog.is_virtual_table(last_assigned - 1));
    EXPECT_FALSE(catalog.is_virtual_table(first_virtual_table_id + 1));
}

// ===========================================================================
// AC3: No staleness path — re-registration after catalog recreation
// A freshly constructed Catalog has an empty set; re-registering repopulates it.
// ===========================================================================

TEST(QA_GDB828, FreshCatalogHasEmptySet) {
    Catalog catalog;
    init_test_catalog(catalog);
    // No virtual tables registered; any ID must return false.
    EXPECT_FALSE(catalog.is_virtual_table(first_virtual_table_id));
    EXPECT_FALSE(catalog.is_virtual_table(0));
}

TEST(QA_GDB828, RecreatedCatalogRepopulatesSet) {
    // Simulate a server restart: destroy catalog, recreate, re-register.
    table_id_t saved_id;
    {
        Catalog catalog;
        init_test_catalog(catalog);
        catalog.register_virtual_table(make_vtdef("pg_database"));
        auto vt = catalog.get_virtual_table("pg_database");
        ASSERT_TRUE(vt.has_value());
        saved_id = vt->table_id;
        EXPECT_TRUE(catalog.is_virtual_table(saved_id));
    }

    // New catalog — virtual tables are not persisted, must be re-registered.
    Catalog catalog2;
    init_test_catalog(catalog2);
    // The old ID must NOT appear before re-registration.
    EXPECT_FALSE(catalog2.is_virtual_table(saved_id));

    // After re-registration the ID (auto-assigned anew, same value since
    // counter starts from the same point) must be visible.
    catalog2.register_virtual_table(make_vtdef("pg_database"));
    auto vt2 = catalog2.get_virtual_table("pg_database");
    ASSERT_TRUE(vt2.has_value());
    EXPECT_TRUE(catalog2.is_virtual_table(vt2->table_id));
}

// ===========================================================================
// AC4: Thread safety — concurrent register + is_virtual_table
// ===========================================================================

TEST(QA_GDB828, ConcurrentRegisterAndLookup) {
    Catalog catalog;
    init_test_catalog(catalog);

    // Pre-register a known table so readers have something to look up.
    catalog.register_virtual_table(make_vtdef("pg_baseline"));
    auto baseline = catalog.get_virtual_table("pg_baseline");
    ASSERT_TRUE(baseline.has_value());
    table_id_t baseline_id = baseline->table_id;

    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kIterations = 200;

    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    // Writers: register new tables concurrently.
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&catalog, w, &errors]() {
            for (int i = 0; i < kIterations; ++i) {
                std::string name =
                    "pg_concurrent_" + std::to_string(w) + "_" + std::to_string(i);
                catalog.register_virtual_table(make_vtdef(name));
                auto vt = catalog.get_virtual_table(name);
                if (!vt.has_value()) {
                    ++errors;
                    continue;
                }
                if (!catalog.is_virtual_table(vt->table_id)) {
                    ++errors;
                }
            }
        });
    }

    // Readers: concurrently query the baseline table.
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&catalog, baseline_id, &errors]() {
            for (int i = 0; i < kIterations * 10; ++i) {
                if (!catalog.is_virtual_table(baseline_id)) {
                    ++errors;
                }
                // User-range IDs must still be false under concurrent writes.
                if (catalog.is_virtual_table(1)) {
                    ++errors;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(errors.load(), 0) << "Thread-safety errors detected: " << errors.load();
}

// ===========================================================================
// Regression: GDB-564 compatibility — is_virtual_table still called correctly
// (keep the callers in test_qa_gdb_564.cpp happy by verifying the same inputs)
// ===========================================================================

TEST(QA_GDB828, Gdb564Compat_PhysicalIdsAreNotVirtual) {
    Catalog catalog;
    init_test_catalog(catalog);

    EXPECT_FALSE(catalog.is_virtual_table(0));
    EXPECT_FALSE(catalog.is_virtual_table(1));
    EXPECT_FALSE(catalog.is_virtual_table(first_user_table_id));
    EXPECT_FALSE(catalog.is_virtual_table(-999)); // adjacent to virtual range, not registered
}

} // namespace sixseven
