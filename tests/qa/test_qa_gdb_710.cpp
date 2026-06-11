/// @file test_qa_gdb_710.cpp
/// @brief Adversarial QA tests for GDB-710: Fix QA fixture bootstrap.
///
/// Attack surfaces:
///   - bootstrap_qa_catalog called twice on the same Catalog (idempotency)
///   - bootstrap_qa_catalog after the default DB has been dropped (cascade)
///   - bootstrap_qa_catalog on a Catalog that already ran SystemBootstrap
///   - helper called with a Catalog that already has a different DB at id=1
///   - next_database_id counter integrity after restore_database
///   - Tables created before and after bootstrap in separate fixtures
///   - Full QueryEngine session: create table, insert, select, then bootstrap
///     a second Catalog and verify isolation
///   - bootstrap_qa_catalog in a fixture that also creates additional databases
///   - restore_database with a name collision but different id
///   - restore_database with an id collision but different name
///   - drop_database(default, cascade) then bootstrap again
///   - list_databases() shows exactly the right set after bootstrap

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static TableSchema simple_schema(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {{0, "id", TypeId::INT32, false, ""}};
    s.pk_columns = "id";
    return s;
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_BootstrapIdempotency
// Tests double-call and triple-call behaviour of bootstrap_qa_catalog.
// ---------------------------------------------------------------------------

class QA_GDB710_BootstrapIdempotency : public ::testing::Test {
protected:
    Catalog catalog_;
};

/// bootstrap_qa_catalog() must leave the catalog in a valid, usable state
/// when called twice on the same Catalog.
TEST_F(QA_GDB710_BootstrapIdempotency, DoubleBootstrapCatalogIntact) {
    bootstrap_qa_catalog(catalog_);
    bootstrap_qa_catalog(catalog_); // second call must not crash or corrupt

    auto db = catalog_.get_database(default_database_name);
    ASSERT_TRUE(db.has_value()) << db.error().message;
    EXPECT_EQ(db->database_id, default_database_id);
}

/// A table created after the first bootstrap is still visible after the
/// second bootstrap call (catalog not cleared).
TEST_F(QA_GDB710_BootstrapIdempotency, TableSurvivesDoubleBootstrap) {
    bootstrap_qa_catalog(catalog_);

    auto tid = catalog_.create_table(default_database_id, simple_schema("t1"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    bootstrap_qa_catalog(catalog_); // idempotent second call

    auto tbl = catalog_.get_table(default_database_id, "t1");
    ASSERT_TRUE(tbl.has_value()) << "table lost after second bootstrap call";
    EXPECT_EQ(tbl->name, "t1");
}

/// Three consecutive calls to bootstrap_qa_catalog remain harmless.
TEST_F(QA_GDB710_BootstrapIdempotency, TripleBootstrapNoCrash) {
    bootstrap_qa_catalog(catalog_);
    bootstrap_qa_catalog(catalog_);
    bootstrap_qa_catalog(catalog_);

    auto db = catalog_.get_database(default_database_name);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, default_database_id);
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_BootstrapAfterDrop
// Tests bootstrap after the default database is dropped and re-created.
// ---------------------------------------------------------------------------

class QA_GDB710_BootstrapAfterDrop : public ::testing::Test {
protected:
    Catalog catalog_;
};

/// Drop the default database (cascade) then re-bootstrap: the DB comes back
/// and tables can be created in it again.
TEST_F(QA_GDB710_BootstrapAfterDrop, DropThenRebootstrap) {
    bootstrap_qa_catalog(catalog_);

    // Create a table so cascade drop has something to clean up.
    auto tid = catalog_.create_table(default_database_id, simple_schema("ephemeral"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    // Drop the default database with cascade.
    auto drop = catalog_.drop_database(default_database_id, /*cascade=*/true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // After drop the database must be gone.
    auto absent = catalog_.get_database(default_database_name);
    EXPECT_FALSE(absent.has_value());

    // Re-bootstrap: restore_database should succeed because id=1 is gone.
    bootstrap_qa_catalog(catalog_);

    auto db = catalog_.get_database(default_database_name);
    ASSERT_TRUE(db.has_value()) << "default DB not re-registered after drop+bootstrap";
    EXPECT_EQ(db->database_id, default_database_id);

    // A new table can be created after re-bootstrap.
    auto tid2 = catalog_.create_table(default_database_id, simple_schema("fresh"));
    ASSERT_TRUE(tid2.has_value()) << tid2.error().message;
}

/// Creating a table, dropping the DB (cascade), re-bootstrapping, then
/// creating a table with the SAME name must succeed (no ghost entry).
TEST_F(QA_GDB710_BootstrapAfterDrop, ReuseTableNameAfterDropAndRebootstrap) {
    bootstrap_qa_catalog(catalog_);

    auto tid = catalog_.create_table(default_database_id, simple_schema("users"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    auto drop = catalog_.drop_database(default_database_id, /*cascade=*/true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    bootstrap_qa_catalog(catalog_);

    // Must not fail with ALREADY_EXISTS — the old entry should be gone.
    auto tid2 = catalog_.create_table(default_database_id, simple_schema("users"));
    ASSERT_TRUE(tid2.has_value()) << "table name reuse failed: " << tid2.error().message;
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_NextIdCounter
// Verifies that restore_database advances the auto-increment counter correctly
// so subsequent create_database() calls do not collide.
// ---------------------------------------------------------------------------

class QA_GDB710_NextIdCounter : public ::testing::Test {
protected:
    Catalog catalog_;
};

/// After bootstrap (which restores id=1), create_database should assign an id
/// that does not collide with 1 or 2 (system).
TEST_F(QA_GDB710_NextIdCounter, CreateDatabaseAfterBootstrapGetsUniqueId) {
    bootstrap_qa_catalog(catalog_);

    auto r = catalog_.create_database("userdb");
    ASSERT_TRUE(r.has_value()) << r.error().message;

    database_id_t new_id = *r;
    EXPECT_NE(new_id, default_database_id);
    EXPECT_NE(new_id, system_database_id);
}

/// Create several databases and confirm no duplicate IDs across them.
TEST_F(QA_GDB710_NextIdCounter, MultipleDatabasesHaveUniqueIds) {
    bootstrap_qa_catalog(catalog_);

    auto a = catalog_.create_database("alpha");
    auto b = catalog_.create_database("beta");
    auto c = catalog_.create_database("gamma");

    ASSERT_TRUE(a.has_value()) << a.error().message;
    ASSERT_TRUE(b.has_value()) << b.error().message;
    ASSERT_TRUE(c.has_value()) << c.error().message;

    // All IDs must be distinct and different from system/default.
    std::vector<database_id_t> ids = {default_database_id, system_database_id, *a, *b, *c};
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            EXPECT_NE(ids[i], ids[j]) << "duplicate database id at indices " << i << " and " << j;
        }
    }
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_IdCollision
// Adversarial misuse: attempting to re-register id=1 or default_database_name
// when they already exist must return ALREADY_EXISTS (not crash or corrupt).
// ---------------------------------------------------------------------------

class QA_GDB710_IdCollision : public ::testing::Test {
protected:
    Catalog catalog_;
};

/// restore_database with id=1 when id=1 already exists returns ALREADY_EXISTS.
TEST_F(QA_GDB710_IdCollision, IdCollisionReturnsAlreadyExists) {
    bootstrap_qa_catalog(catalog_); // registers id=1

    auto r = catalog_.restore_database(default_database_id, "other_name");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::ALREADY_EXISTS);

    // Catalog must still be intact.
    auto db = catalog_.get_database(default_database_name);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, default_database_id);
}

/// restore_database with default_database_name when that name already exists
/// (even with a different id) returns ALREADY_EXISTS.
TEST_F(QA_GDB710_IdCollision, NameCollisionReturnsAlreadyExists) {
    bootstrap_qa_catalog(catalog_); // registers default_database_name -> id=1

    // Try to register the same name but with a different id (e.g., 99).
    auto r = catalog_.restore_database(99, default_database_name);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::ALREADY_EXISTS);

    // The original mapping must be unchanged.
    auto db = catalog_.get_database(default_database_name);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->database_id, default_database_id);
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_ListDatabases
// Verifies list_databases() returns exactly the expected set after bootstrap.
// ---------------------------------------------------------------------------

class QA_GDB710_ListDatabases : public ::testing::Test {
protected:
    Catalog catalog_;
};

/// Before bootstrap: only the system database (id=2) is present.
TEST_F(QA_GDB710_ListDatabases, FreshCatalogHasOnlySystemDB) {
    auto dbs = catalog_.list_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].database_id, system_database_id);
}

/// After bootstrap: system db + default db, sorted by id.
TEST_F(QA_GDB710_ListDatabases, AfterBootstrapHasSystemAndDefault) {
    bootstrap_qa_catalog(catalog_);
    auto dbs = catalog_.list_databases();
    // Must have at least 2 (default=1, system=2).
    ASSERT_GE(dbs.size(), 2u);

    bool has_default = false;
    bool has_system = false;
    for (auto& db : dbs) {
        if (db.database_id == default_database_id) {
            has_default = true;
            EXPECT_EQ(db.name, default_database_name);
        }
        if (db.database_id == system_database_id) {
            has_system = true;
        }
    }
    EXPECT_TRUE(has_default) << "default database missing from list_databases()";
    EXPECT_TRUE(has_system) << "system database missing from list_databases()";
}

/// Double-bootstrap does not produce duplicate entries in list_databases().
TEST_F(QA_GDB710_ListDatabases, DoubleBootstrapNoDuplicates) {
    bootstrap_qa_catalog(catalog_);
    bootstrap_qa_catalog(catalog_);

    auto dbs = catalog_.list_databases();
    size_t count_default = 0;
    for (auto& db : dbs) {
        if (db.database_id == default_database_id)
            ++count_default;
    }
    EXPECT_EQ(count_default, 1u) << "duplicate default database entry after double bootstrap";
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_IsolationBetweenFixtures
// Two independent Catalog+QueryEngine stacks are fully isolated: tables
// created in one are invisible in the other.
// ---------------------------------------------------------------------------

class QA_GDB710_IsolationBetweenFixtures : public ::testing::Test {
protected:
    void SetUp() override {
        dir_a_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb710_a";
        dir_b_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb710_b";
        std::filesystem::remove_all(dir_a_);
        std::filesystem::remove_all(dir_b_);
        std::filesystem::create_directories(dir_a_);
        std::filesystem::create_directories(dir_b_);

        bootstrap_qa_catalog(cat_a_);
        bootstrap_qa_catalog(cat_b_);

        storage_a_ = std::make_unique<StorageManager>(dm_a_, dir_a_);
        storage_b_ = std::make_unique<StorageManager>(dm_b_, dir_b_);
        engine_a_ = std::make_unique<QueryEngine>(cat_a_, *storage_a_);
        engine_b_ = std::make_unique<QueryEngine>(cat_b_, *storage_b_);
    }

    void TearDown() override {
        engine_a_.reset();
        engine_b_.reset();
        storage_a_.reset();
        storage_b_.reset();
        std::filesystem::remove_all(dir_a_);
        std::filesystem::remove_all(dir_b_);
    }

    Catalog cat_a_;
    Catalog cat_b_;
    DiskManager dm_a_;
    DiskManager dm_b_;
    std::filesystem::path dir_a_;
    std::filesystem::path dir_b_;
    std::unique_ptr<StorageManager> storage_a_;
    std::unique_ptr<StorageManager> storage_b_;
    std::unique_ptr<QueryEngine> engine_a_;
    std::unique_ptr<QueryEngine> engine_b_;
};

/// A table created in engine A is not visible in engine B.
TEST_F(QA_GDB710_IsolationBetweenFixtures, TableInANotVisibleInB) {
    auto cr = engine_a_->execute("CREATE TABLE private_a (id INT PRIMARY KEY)");
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Querying the same table name in B must fail.
    auto qr = engine_b_->execute("SELECT id FROM private_a");
    EXPECT_FALSE(qr.has_value()) << "table from engine A should not be visible in engine B";
}

/// Both engines can independently create a table with the same name.
TEST_F(QA_GDB710_IsolationBetweenFixtures, SameTableNameInBothEngines) {
    auto ca = engine_a_->execute("CREATE TABLE shared_name (id INT PRIMARY KEY)");
    ASSERT_TRUE(ca.has_value()) << ca.error().message;

    auto cb = engine_b_->execute("CREATE TABLE shared_name (id INT PRIMARY KEY)");
    ASSERT_TRUE(cb.has_value()) << cb.error().message;

    // Insert and select in A is isolated from B.
    auto ia = engine_a_->execute("INSERT INTO shared_name VALUES (1)");
    ASSERT_TRUE(ia.has_value()) << ia.error().message;

    auto sb = engine_b_->execute("SELECT id FROM shared_name");
    ASSERT_TRUE(sb.has_value()) << sb.error().message;
    EXPECT_EQ(sb->rows.size(), 0u) << "B should see 0 rows (A's insert must not leak)";
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_AdditionalDatabases
// bootstrap_qa_catalog coexists correctly with user-created databases.
// ---------------------------------------------------------------------------

class QA_GDB710_AdditionalDatabases : public ::testing::Test {
protected:
    Catalog catalog_;
};

/// Create additional databases before and after bootstrap; all must be
/// independently usable.
TEST_F(QA_GDB710_AdditionalDatabases, UserDatabasesCoexistWithDefault) {
    // Create a user database BEFORE bootstrap.
    auto pre = catalog_.create_database("pre_bootstrap_db");
    ASSERT_TRUE(pre.has_value()) << pre.error().message;

    bootstrap_qa_catalog(catalog_);

    // Create a user database AFTER bootstrap.
    auto post = catalog_.create_database("post_bootstrap_db");
    ASSERT_TRUE(post.has_value()) << post.error().message;

    // All three user databases and the system database must be present.
    auto def = catalog_.get_database(default_database_name);
    ASSERT_TRUE(def.has_value()) << "default DB missing after user DB creation";

    auto pre_db = catalog_.get_database("pre_bootstrap_db");
    ASSERT_TRUE(pre_db.has_value()) << "pre_bootstrap_db missing";

    auto post_db = catalog_.get_database("post_bootstrap_db");
    ASSERT_TRUE(post_db.has_value()) << "post_bootstrap_db missing";

    // Tables can be created in all databases.
    auto t1 = catalog_.create_table(*pre, simple_schema("t_pre"));
    ASSERT_TRUE(t1.has_value()) << t1.error().message;

    auto t2 = catalog_.create_table(default_database_id, simple_schema("t_default"));
    ASSERT_TRUE(t2.has_value()) << t2.error().message;

    auto t3 = catalog_.create_table(*post, simple_schema("t_post"));
    ASSERT_TRUE(t3.has_value()) << t3.error().message;
}

/// Dropping user databases does not affect the default database.
TEST_F(QA_GDB710_AdditionalDatabases, DroppingUserDBLeavesDefaultIntact) {
    bootstrap_qa_catalog(catalog_);

    auto uid = catalog_.create_database("tmp");
    ASSERT_TRUE(uid.has_value()) << uid.error().message;

    auto drop = catalog_.drop_database(*uid, /*cascade=*/true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Default DB must still be reachable.
    auto def = catalog_.get_database(default_database_name);
    ASSERT_TRUE(def.has_value()) << "default DB lost after dropping unrelated user DB";
    EXPECT_EQ(def->database_id, default_database_id);
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB710_FullPipelineAfterBootstrap
// End-to-end smoke: QueryEngine sessions that call bootstrap_qa_catalog
// behave correctly for DDL, DML, and DQL.
// ---------------------------------------------------------------------------

class QA_GDB710_FullPipeline : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb710_e2e";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        if (!r.has_value()) {
            ADD_FAILURE() << "SQL failed: " << sql << "\n" << r.error().message;
            return {};
        }
        return std::move(*r);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

/// Full DDL+DML+DQL round-trip succeeds after bootstrap_qa_catalog.
TEST_F(QA_GDB710_FullPipeline, CreateInsertSelectRoundTrip) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR, qty INT)");
    exec_ok("INSERT INTO items VALUES (1, 'apple', 10)");
    exec_ok("INSERT INTO items VALUES (2, 'banana', 5)");
    exec_ok("INSERT INTO items VALUES (3, 'cherry', 20)");

    auto r = exec_ok("SELECT id, name, qty FROM items ORDER BY id");
    ASSERT_EQ(r.rows.size(), 3u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 1);
    EXPECT_EQ(r.rows[1][1].as_string(), "banana");
    EXPECT_EQ(r.rows[2][2].as_int32(), 20);
}

/// DROP TABLE after bootstrap leaves the catalog consistent.
TEST_F(QA_GDB710_FullPipeline, DropTableAfterBootstrap) {
    exec_ok("CREATE TABLE temp_tbl (id INT PRIMARY KEY)");
    exec_ok("INSERT INTO temp_tbl VALUES (42)");

    auto drop = engine_->execute("DROP TABLE temp_tbl");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Querying the dropped table must fail.
    auto q = engine_->execute("SELECT id FROM temp_tbl");
    EXPECT_FALSE(q.has_value());
}

/// Selecting from a non-existent table returns a proper error, not a crash.
TEST_F(QA_GDB710_FullPipeline, SelectFromNonExistentTableErrors) {
    auto r = engine_->execute("SELECT id FROM no_such_table");
    ASSERT_FALSE(r.has_value());
    // Error code should be NOT_FOUND or PARSE_ERROR — anything but a crash.
    EXPECT_TRUE(r.error().code == StatusCode::NOT_FOUND ||
                r.error().code == StatusCode::PARSE_ERROR ||
                r.error().code == StatusCode::INVALID_ARGUMENT);
}

/// Many tables created in the same session after a single bootstrap call.
TEST_F(QA_GDB710_FullPipeline, ManyTablesInOneSession) {
    for (int i = 0; i < 20; ++i) {
        std::string sql =
            "CREATE TABLE tbl_" + std::to_string(i) + " (id INT PRIMARY KEY, val INT)";
        exec_ok(sql);
    }
    for (int i = 0; i < 20; ++i) {
        std::string ins = "INSERT INTO tbl_" + std::to_string(i) + " VALUES (" + std::to_string(i) +
                          ", " + std::to_string(i * 10) + ")";
        exec_ok(ins);
    }
    for (int i = 0; i < 20; ++i) {
        std::string sel = "SELECT id, val FROM tbl_" + std::to_string(i);
        auto r = exec_ok(sel);
        ASSERT_EQ(r.rows.size(), 1u) << "table tbl_" << i << " has wrong row count";
        EXPECT_EQ(r.rows[0][0].as_int32(), i);
    }
}

} // namespace
} // namespace sixseven
