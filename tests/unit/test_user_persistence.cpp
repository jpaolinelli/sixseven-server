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

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Schema guard — sys_users reserved id must stay stable across restarts.
// =============================================================================

TEST(SysUsersSchema, ReservedIds) {
    EXPECT_EQ(sys_users_table_id, 11);
    EXPECT_EQ(first_user_table_id, 12);

    auto schema = sys_users_schema();
    EXPECT_EQ(schema.table_id, sys_users_table_id);
    EXPECT_EQ(schema.name, "sys_users");
    EXPECT_EQ(schema.pk_columns, "username");
    ASSERT_EQ(schema.columns.size(), 6u);
    EXPECT_EQ(schema.columns[0].name, "username");
    EXPECT_EQ(schema.columns[4].name, "auth_method");
}

TEST(AuthMethodToString, RoundTripsThroughParse) {
    for (auto method : {AuthMethod::TRUST, AuthMethod::MD5, AuthMethod::SCRAM_SHA_256}) {
        auto parsed = parse_auth_method(auth_method_to_string(method));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, method);
    }
}

// =============================================================================
// Fixture: full persistence stack + UserManager wired through sys_users.
// =============================================================================

class UserPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_user_persistence";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        boot();
    }

    void TearDown() override {
        teardown_state();
        std::filesystem::remove_all(data_dir_);
    }

    // Bring up a fresh in-memory stack against the on-disk data dir, run
    // bootstrap, ensure sys_users, and wire write-through persistence into a
    // new UserManager (mirroring main.cpp).
    void boot() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
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
    }

    void teardown_state() {
        users_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    // Simulate a server restart: drop all in-memory state and rebuild from disk.
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

// -- Write-through ------------------------------------------------------------

TEST_F(UserPersistenceTest, CreateUserWritesThroughToDisk) {
    auto created = users_->create_user("alice", "secret", AuthMethod::SCRAM_SHA_256);
    ASSERT_TRUE(created.has_value()) << created.error().message;

    auto rows = persistence_->load_users();
    ASSERT_TRUE(rows.has_value()) << rows.error().message;
    bool found = false;
    for (const auto& r : *rows) {
        if (r.username == "alice") {
            found = true;
            EXPECT_EQ(r.method, AuthMethod::SCRAM_SHA_256);
            EXPECT_FALSE(r.password_hash.empty());
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(UserPersistenceTest, AlterUserUpsertsSingleRow) {
    ASSERT_TRUE(users_->create_user("bob", "pw1", AuthMethod::SCRAM_SHA_256).has_value());
    auto before = users_->get_user("bob");
    ASSERT_TRUE(before.has_value());

    ASSERT_TRUE(users_->alter_user("bob", "pw2", AuthMethod::SCRAM_SHA_256).has_value());

    // Exactly one persisted row for bob, and its hash changed.
    auto rows = persistence_->load_users();
    ASSERT_TRUE(rows.has_value());
    int count = 0;
    std::string persisted_hash;
    for (const auto& r : *rows) {
        if (r.username == "bob") {
            ++count;
            persisted_hash = r.password_hash;
        }
    }
    EXPECT_EQ(count, 1);
    EXPECT_NE(persisted_hash, before->password_hash);
}

TEST_F(UserPersistenceTest, DropUserRemovesPersistedRow) {
    ASSERT_TRUE(users_->create_user("carol", "secret", AuthMethod::SCRAM_SHA_256).has_value());
    ASSERT_TRUE(users_->drop_user("carol", /*if_exists=*/false).has_value());

    auto rows = persistence_->load_users();
    ASSERT_TRUE(rows.has_value());
    for (const auto& r : *rows) {
        EXPECT_NE(r.username, "carol");
    }
}

// -- Restart round-trip -------------------------------------------------------

TEST_F(UserPersistenceTest, UsersSurviveRestart) {
    ASSERT_TRUE(users_->create_user("dave", "hunter2", AuthMethod::SCRAM_SHA_256).has_value());
    auto original = users_->get_user("dave");
    ASSERT_TRUE(original.has_value());

    restart();

    auto reloaded = users_->get_user("dave");
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->password_hash, original->password_hash);
    EXPECT_EQ(reloaded->salt, original->salt);
    EXPECT_EQ(reloaded->iterations, original->iterations);
    EXPECT_EQ(reloaded->method, original->method);
}

TEST_F(UserPersistenceTest, DroppedUserStaysGoneAfterRestart) {
    ASSERT_TRUE(users_->create_user("erin", "secret", AuthMethod::SCRAM_SHA_256).has_value());
    ASSERT_TRUE(users_->drop_user("erin", false).has_value());

    restart();

    EXPECT_FALSE(users_->user_exists("erin"));
}

// -- Default admin ------------------------------------------------------------

TEST_F(UserPersistenceTest, DefaultAdminSeedsAndPersists) {
    // Store starts empty; seed and verify it lands on disk.
    EXPECT_TRUE(users_->empty());
    users_->ensure_default_admin(AuthMethod::SCRAM_SHA_256);
    EXPECT_TRUE(users_->user_exists("sixseven"));

    restart();

    // Reloaded from disk without re-seeding.
    EXPECT_TRUE(users_->user_exists("sixseven"));
}

TEST_F(UserPersistenceTest, DefaultAdminIdempotentAcrossRestart) {
    users_->ensure_default_admin(AuthMethod::SCRAM_SHA_256);
    restart();
    // load() already populated the admin; calling again must not duplicate.
    users_->ensure_default_admin(AuthMethod::SCRAM_SHA_256);

    auto rows = persistence_->load_users();
    ASSERT_TRUE(rows.has_value());
    int admin_count = 0;
    for (const auto& r : *rows) {
        if (r.username == "sixseven") {
            ++admin_count;
        }
    }
    EXPECT_EQ(admin_count, 1);
}

// -- A reloaded SCRAM record still authenticates ------------------------------

namespace {

// Recompute the SCRAM client proof from the plaintext password (mirrors the
// client side of the exchange), as in test_auth.cpp's FullSuccessfulExchange.
std::vector<uint8_t> client_proof_for(const std::string& password,
                                      const UserRecord& record,
                                      const std::string& client_first_bare,
                                      const std::string& server_first,
                                      const ScramServerState& state) {
    auto salt = base64_decode(record.salt);
    std::vector<uint8_t> pass_key(password.begin(), password.end());
    std::string salt_str(salt.begin(), salt.end());
    salt_str += '\0';
    salt_str += '\0';
    salt_str += '\0';
    salt_str += '\1';
    auto u_prev = hmac_sha256(pass_key, salt_str);
    auto salted = u_prev;
    for (int32_t i = 1; i < record.iterations; ++i) {
        std::string u_str(u_prev.begin(), u_prev.end());
        u_prev = hmac_sha256(pass_key, u_str);
        salted = xor_bytes(salted, u_prev);
    }

    std::string keys_part = record.password_hash.substr(record.password_hash.rfind('$') + 1);
    auto stored_key = base64_decode(keys_part.substr(0, keys_part.find(':')));

    std::string client_final_without_proof = "c=biws,r=" + state.server_nonce;
    std::string auth_message =
        client_first_bare + "," + server_first + "," + client_final_without_proof;
    auto client_signature = hmac_sha256(stored_key, auth_message);
    auto client_key = hmac_sha256(salted, "Client Key");
    return xor_bytes(client_key, client_signature);
}

} // namespace

TEST_F(UserPersistenceTest, ReloadedScramRecordAuthenticates) {
    ASSERT_TRUE(users_->create_user("frank", "topsecret", AuthMethod::SCRAM_SHA_256).has_value());

    restart();

    auto record = users_->get_user("frank");
    ASSERT_TRUE(record.has_value());

    std::string client_nonce = base64_encode(random_bytes(18));
    std::string client_first_bare = "n=frank,r=" + client_nonce;
    std::string client_first = "n,," + client_first_bare;

    ScramServerState state;
    auto server_first = scram_server_first(client_first, *record, state);
    ASSERT_TRUE(server_first.has_value()) << server_first.error().message;

    auto proof = client_proof_for("topsecret", *record, client_first_bare, *server_first, state);
    std::string client_final = "c=biws,r=" + state.server_nonce + ",p=" + base64_encode(proof);

    auto server_final = scram_server_final(client_final, state);
    ASSERT_TRUE(server_final.has_value()) << server_final.error().message;
    EXPECT_EQ(server_final->substr(0, 2), "v=");
}
