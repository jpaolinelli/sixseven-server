/// @file test_qa_user_auth_persistence.cpp
/// @brief QA regression tests for persisted authentication via sys_users.
///
/// Covers the end-to-end wiring that was previously missing in production:
/// CREATE/ALTER/DROP USER through the QueryEngine, write-through to the
/// sys_users system table, survival across a simulated restart, default-admin
/// seeding, and the legacy data-directory table-id collision fallback.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/server/auth.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

namespace {

void init_default_db(Catalog& catalog) {
    auto r = catalog.restore_database(default_database_id, "sixseven");
    (void)r;
}

} // namespace

class QAUserAuthPersistence : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_user_auth_persistence";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        boot();
    }

    void TearDown() override {
        teardown_state();
        std::filesystem::remove_all(data_dir_);
    }

    void boot() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_default_db(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());

        auto config = Config::load_defaults();
        auto boot_result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config, data_dir_);
        ASSERT_TRUE(boot_result.has_value()) << boot_result.error().message;

        auto users_table = SystemBootstrap::ensure_users_table(*persistence_, *catalog_, *storage_);
        ASSERT_TRUE(users_table.has_value()) << users_table.error().message;
        ASSERT_TRUE(*users_table);

        users_ = std::make_unique<UserManager>();
        users_->set_persistence(
            [this](const UserRecord& u) { return persistence_->persist_user(u); },
            [this](const std::string& name) { return persistence_->remove_user(name); });
        auto loaded = persistence_->load_users();
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
        users_->load(std::move(*loaded));
        if (users_->empty()) {
            users_->ensure_default_admin(AuthMethod::SCRAM_SHA_256);
        }

        engine_->set_user_manager(users_.get());
        engine_->set_auth_method(AuthMethod::SCRAM_SHA_256);
    }

    void teardown_state() {
        users_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void restart() {
        teardown_state();
        boot();
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<UserManager> users_;
};

// The default admin is seeded on first boot and survives restart.
TEST_F(QAUserAuthPersistence, DefaultAdminPresentAndPersistent) {
    EXPECT_TRUE(users_->user_exists("sixseven"));
    restart();
    EXPECT_TRUE(users_->user_exists("sixseven"));
}

// CREATE USER via SQL no longer errors with "user manager not initialized".
TEST_F(QAUserAuthPersistence, CreateUserViaSqlSucceeds) {
    auto r = engine_->execute("CREATE USER alice WITH PASSWORD 'secret'");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(users_->user_exists("alice"));
}

// A user created via SQL is still present after restart.
TEST_F(QAUserAuthPersistence, CreatedUserSurvivesRestart) {
    ASSERT_TRUE(engine_->execute("CREATE USER bob WITH PASSWORD 'secret'").has_value());
    restart();
    EXPECT_TRUE(users_->user_exists("bob"));
}

// ALTER USER changes the stored credential and persists it.
TEST_F(QAUserAuthPersistence, AlterUserPersistsNewPassword) {
    ASSERT_TRUE(engine_->execute("CREATE USER carol WITH PASSWORD 'old'").has_value());
    auto before = users_->get_user("carol");
    ASSERT_TRUE(before.has_value());

    ASSERT_TRUE(engine_->execute("ALTER USER carol WITH PASSWORD 'new'").has_value());
    restart();

    auto after = users_->get_user("carol");
    ASSERT_TRUE(after.has_value());
    EXPECT_NE(after->password_hash, before->password_hash);
}

// DROP USER removes the credential, and it stays gone after restart.
TEST_F(QAUserAuthPersistence, DropUserIsPermanent) {
    ASSERT_TRUE(engine_->execute("CREATE USER dave WITH PASSWORD 'secret'").has_value());
    ASSERT_TRUE(engine_->execute("DROP USER dave").has_value());
    EXPECT_FALSE(users_->user_exists("dave"));
    restart();
    EXPECT_FALSE(users_->user_exists("dave"));
}

// Creating a duplicate user is rejected.
TEST_F(QAUserAuthPersistence, DuplicateUserRejected) {
    ASSERT_TRUE(engine_->execute("CREATE USER erin WITH PASSWORD 'secret'").has_value());
    auto dup = engine_->execute("CREATE USER erin WITH PASSWORD 'other'");
    EXPECT_FALSE(dup.has_value());
}

// Legacy data directory: if the reserved sys_users id is already taken by a
// user table, ensure_users_table reports persistence unavailable rather than
// clobbering the existing table.
TEST(QAUserAuthLegacyCollision, ReservedIdOccupiedFallsBack) {
    auto data_dir = std::filesystem::temp_directory_path() / "sixseven_qa_user_auth_collision";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    // Scope the storage stack so its file handles are released (Windows cannot
    // delete an open file) before the cleanup remove_all below.
    {
        DiskManager dm;
        Catalog catalog;
        ASSERT_TRUE(catalog.restore_database(default_database_id, "sixseven").has_value());
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);

        // Simulate a pre-existing user table occupying the sys_users reserved id.
        ASSERT_TRUE(storage.create_database_storage(default_database_id).has_value());
        TableSchema legacy;
        legacy.table_id = sys_users_table_id;
        legacy.name = "legacy_table";
        legacy.columns = {{0, "id", TypeId::INT32, false, ""}};
        legacy.pk_columns = "id";
        ASSERT_TRUE(catalog.restore_table(default_database_id, legacy).has_value());
        ASSERT_TRUE(storage.create_table_storage(default_database_id, sys_users_table_id, legacy)
                        .has_value());

        auto result = SystemBootstrap::ensure_users_table(persistence, catalog, storage);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_FALSE(*result); // Persistence unavailable; caller uses in-memory auth.

        // The legacy table is untouched.
        auto still_there = catalog.get_table_by_id(sys_users_table_id);
        ASSERT_TRUE(still_there.has_value());
        EXPECT_EQ(still_there->name, "legacy_table");
    }

    std::filesystem::remove_all(data_dir);
}
