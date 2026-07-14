// QA regression / adversarial tests for GDB-1298: PRIMARY KEY / UNIQUE
// constraints now get a backing auto-created unique index at CREATE TABLE
// time so duplicate keys are rejected on INSERT.
//
// Focus areas (per QA handoff):
//   - composite PK duplicate detection (partial-match should NOT collide)
//   - empty-string vs NULL vs normal-value PK/UNIQUE interactions
//   - concurrent/transactional insert races on the same unique key
//   - DROP TABLE + recreate does not leave orphaned index entries / does not
//     resurrect stale duplicate-rejection state

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QA_GDB1298 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1298";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();

        auto bootstrap_result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(bootstrap_result.has_value()) << bootstrap_result.error().message;

        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        auto rebuild = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(rebuild.has_value()) << rebuild.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " -> " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << "expected failure for: " << sql;
        EXPECT_EQ(result.error().code, expected) << sql << " -> " << result.error().message;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
    Config config_;
};

} // namespace

// =============================================================================
// Composite PK: partial-key matches must NOT be treated as duplicates.
// =============================================================================

TEST_F(QA_GDB1298, CompositePkPartialMatchOnFirstColumnIsNotDuplicate) {
    exec_ok("CREATE TABLE ci1 (a INT, b INT, PRIMARY KEY (a, b))");
    exec_ok("INSERT INTO ci1 VALUES (1, 1)");
    // Same 'a', different 'b' -- must be accepted.
    exec_ok("INSERT INTO ci1 VALUES (1, 2)");
    auto qr = exec_ok("SELECT a, b FROM ci1");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QA_GDB1298, CompositePkPartialMatchOnSecondColumnIsNotDuplicate) {
    exec_ok("CREATE TABLE ci2 (a INT, b INT, PRIMARY KEY (a, b))");
    exec_ok("INSERT INTO ci2 VALUES (1, 1)");
    // Same 'b', different 'a' -- must be accepted.
    exec_ok("INSERT INTO ci2 VALUES (2, 1)");
    auto qr = exec_ok("SELECT a, b FROM ci2");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QA_GDB1298, CompositePkOrderSensitiveNotSwappedFalsePositive) {
    // (1, 2) and (2, 1) must both be allowed -- they are not the same
    // composite key even though they share the same value set.
    exec_ok("CREATE TABLE ci3 (a INT, b INT, PRIMARY KEY (a, b))");
    exec_ok("INSERT INTO ci3 VALUES (1, 2)");
    exec_ok("INSERT INTO ci3 VALUES (2, 1)");
    auto qr = exec_ok("SELECT a, b FROM ci3");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Empty-string vs NULL vs normal-value interactions (adjacent to GDB-1245).
// =============================================================================

TEST_F(QA_GDB1298, UniqueStringAllowsEmptyStringOnceAndRejectsSecond) {
    exec_ok("CREATE TABLE es1 (id INT, tag VARCHAR UNIQUE)");
    exec_ok("INSERT INTO es1 VALUES (1, '')");
    exec_error("INSERT INTO es1 VALUES (2, '')", StatusCode::CONSTRAINT_VIOLATION);
    // NULL must remain distinct from empty string and be accepted freely.
    exec_ok("INSERT INTO es1 VALUES (3, NULL)");
    exec_ok("INSERT INTO es1 VALUES (4, NULL)");
    auto qr = exec_ok("SELECT id FROM es1");
    EXPECT_EQ(qr.rows.size(), 3u);
}

TEST_F(QA_GDB1298, CompositeUniqueWithOneNullColumnSkipsIndexEntirely) {
    // If ANY column in a composite unique key is NULL, the row is skipped
    // from the unique index (per implementation comment). Verify two rows
    // that share the same non-null column value but differ only by having
    // NULL in the other slot are both accepted (not spuriously rejected),
    // AND that this doesn't allow two *fully* identical non-null rows through.
    exec_ok("CREATE TABLE cu1 (a INT, b INT, UNIQUE (a, b))");
    exec_ok("INSERT INTO cu1 VALUES (1, NULL)");
    exec_ok("INSERT INTO cu1 VALUES (1, NULL)");
    auto qr = exec_ok("SELECT a, b FROM cu1");
    EXPECT_EQ(qr.rows.size(), 2u);

    exec_ok("INSERT INTO cu1 VALUES (5, 5)");
    exec_error("INSERT INTO cu1 VALUES (5, 5)", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(QA_GDB1298, PrimaryKeyRejectsExplicitNullInsertAttempt) {
    // PK columns are implicitly NOT NULL; a NULL literal on a PK column
    // should ideally fail. This is a KNOWN, already-filed, out-of-scope gap
    // (per GDB-1298 implementation handoff: "NOT NULL on explicit NULL
    // literals is a separate known gap (already filed) -- don't re-report
    // it"). We reproduce it here for visibility but do not fail QA on it,
    // since it is orthogonal to the PK/UNIQUE-index-backing fix under test.
    exec_ok("CREATE TABLE pknull (id INT PRIMARY KEY, v INT)");
    auto result = engine_->execute("INSERT INTO pknull VALUES (NULL, 1)");
    if (result.has_value()) {
        GTEST_SKIP() << "Known pre-existing gap (already filed, not GDB-1298 scope): "
                         "PRIMARY KEY column silently accepted an explicit NULL literal.";
    }
}

// =============================================================================
// Concurrency: autocommit inserts racing on the same unique key (TOCTOU).
// =============================================================================

TEST_F(QA_GDB1298, ConcurrentInsertsOnSameUniqueKeyOnlyOneSucceeds) {
    exec_ok("CREATE TABLE race1 (id INT PRIMARY KEY, worker INT)");

    constexpr int kThreads = 8;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    // NOTE: QueryEngine here is a single shared instance -- if the engine is
    // not internally thread-safe for concurrent execute() calls, this test
    // primarily demonstrates whether uniqueness holds under interleaving
    // (via TOCTOU) rather than testing true multi-connection concurrency.
    // A shared std::mutex is intentionally NOT used here so any race in the
    // uniqueness check has a chance to manifest.
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            auto result = engine_->execute(
                "INSERT INTO race1 VALUES (1, " + std::to_string(i) + ")");
            if (result.has_value()) {
                successes.fetch_add(1);
            } else {
                failures.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Regardless of internal locking strategy, the *end state* of the table
    // must never contain more than one row with id = 1.
    auto qr = exec_ok("SELECT id FROM race1 WHERE id = 1");
    EXPECT_LE(qr.rows.size(), 1u)
        << "duplicate PK rows survived concurrent autocommit inserts (found "
        << qr.rows.size() << ")";
}

// =============================================================================
// DROP TABLE + recreate: no orphaned index entries, no stale rejection state.
// =============================================================================

TEST_F(QA_GDB1298, DropAndRecreateTableClearsUniqueIndexState) {
    exec_ok("CREATE TABLE dr1 (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO dr1 VALUES (1, 'alice')");
    exec_error("INSERT INTO dr1 VALUES (1, 'bob')", StatusCode::CONSTRAINT_VIOLATION);

    exec_ok("DROP TABLE dr1");

    // Recreate with the same name/schema. The value 1 must be insertable
    // again -- if the auto-created index or its data were not cleaned up,
    // this would spuriously reject as a duplicate against ghost data.
    exec_ok("CREATE TABLE dr1 (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO dr1 VALUES (1, 'carol')");
    auto qr = exec_ok("SELECT id, name FROM dr1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "carol");

    // Uniqueness must still be enforced post-recreate.
    exec_error("INSERT INTO dr1 VALUES (1, 'dave')", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(QA_GDB1298, DropAndRecreateTableDoesNotLeakAutoIndexNames) {
    exec_ok("CREATE TABLE dr2 (id INT PRIMARY KEY)");
    auto table_id_1 = catalog_->get_table(default_database_id, "dr2")->table_id;
    auto indexes_1 = catalog_->list_indexes(table_id_1);
    size_t count_1 = indexes_1.size();

    exec_ok("DROP TABLE dr2");
    exec_ok("CREATE TABLE dr2 (id INT PRIMARY KEY)");
    auto table_id_2 = catalog_->get_table(default_database_id, "dr2")->table_id;
    auto indexes_2 = catalog_->list_indexes(table_id_2);

    // The recreated table should have exactly the same number of
    // auto-created indexes as the first creation -- not accumulating extras
    // across drop/recreate cycles.
    EXPECT_EQ(indexes_2.size(), count_1);
}

TEST_F(QA_GDB1298, RepeatedDropRecreateCyclesKeepUniquenessConsistent) {
    for (int cycle = 0; cycle < 5; ++cycle) {
        exec_ok("CREATE TABLE dr3 (id INT PRIMARY KEY)");
        exec_ok("INSERT INTO dr3 VALUES (42)");
        exec_error("INSERT INTO dr3 VALUES (42)", StatusCode::CONSTRAINT_VIOLATION);
        exec_ok("DROP TABLE dr3");
    }
}

// =============================================================================
// Scale: many rows inserted, duplicates interspersed.
// =============================================================================

TEST_F(QA_GDB1298, BulkInsertWithInterspersedDuplicatesRejectsOnlyDuplicates) {
    exec_ok("CREATE TABLE bulk1 (id INT PRIMARY KEY)");
    int accepted = 0;
    int rejected = 0;
    for (int i = 0; i < 500; ++i) {
        int id = i % 300; // guarantees duplicates after id 300 wraps
        auto result = engine_->execute("INSERT INTO bulk1 VALUES (" + std::to_string(id) + ")");
        if (result.has_value()) {
            accepted++;
        } else {
            EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
            rejected++;
        }
    }
    EXPECT_EQ(accepted, 300);
    EXPECT_EQ(rejected, 200);

    auto qr = exec_ok("SELECT id FROM bulk1");
    EXPECT_EQ(qr.rows.size(), 300u);
}
