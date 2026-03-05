#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// QueryEngine test fixture (with persistence support)
// =============================================================================

class AutoIncrementTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_autoincrement";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    /// Simulate a server restart: destroy all in-memory state and recreate.
    void restart() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());

        // Re-run bootstrap to load catalog from disk.
        run_bootstrap();
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed for: " << sql
                          << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error but got success for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// =============================================================================
// AC1: Parser acceptance and rejection
// =============================================================================

TEST_F(AutoIncrementTest, ParsesAutoincrementOnIntegerPK) {
    run_bootstrap();
    auto qr = exec_ok("CREATE TABLE t1 (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");
    EXPECT_EQ(qr.message, "CREATE TABLE");
}

TEST_F(AutoIncrementTest, ParsesAutoincrementAllSQLIntTypes) {
    run_bootstrap();
    exec_ok("CREATE TABLE t_tiny (id TINYINT PRIMARY KEY AUTOINCREMENT, v INT)");
    exec_ok("CREATE TABLE t_small (id SMALLINT PRIMARY KEY AUTOINCREMENT, v INT)");
    exec_ok("CREATE TABLE t_int (id INT PRIMARY KEY AUTOINCREMENT, v INT)");
    exec_ok("CREATE TABLE t_big (id BIGINT PRIMARY KEY AUTOINCREMENT, v INT)");
}

TEST_F(AutoIncrementTest, RejectsAutoincrementOnNonIntegerType) {
    run_bootstrap();
    exec_error("CREATE TABLE t_bad (id VARCHAR PRIMARY KEY AUTOINCREMENT)", StatusCode::TYPE_ERROR);
}

TEST_F(AutoIncrementTest, RejectsAutoincrementOnFloat) {
    run_bootstrap();
    exec_error("CREATE TABLE t_bad (val FLOAT PRIMARY KEY AUTOINCREMENT)", StatusCode::TYPE_ERROR);
}

TEST_F(AutoIncrementTest, RejectsAutoincrementOnBool) {
    run_bootstrap();
    exec_error("CREATE TABLE t_bad (flag BOOLEAN PRIMARY KEY AUTOINCREMENT)",
               StatusCode::TYPE_ERROR);
}

TEST_F(AutoIncrementTest, RejectsAutoincrementOnNonPKColumn) {
    run_bootstrap();
    exec_error("CREATE TABLE t_bad (id INT AUTOINCREMENT, name VARCHAR)",
               StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(AutoIncrementTest, RejectsAutoincrementWithDefault) {
    run_bootstrap();
    exec_error("CREATE TABLE t_bad (id INT PRIMARY KEY AUTOINCREMENT DEFAULT 1)",
               StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(AutoIncrementTest, RejectsDefaultWithAutoincrement) {
    run_bootstrap();
    exec_error("CREATE TABLE t_bad (id INT PRIMARY KEY DEFAULT 1 AUTOINCREMENT)",
               StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// AC3: INSERT with omitted auto-increment column
// =============================================================================

TEST_F(AutoIncrementTest, InsertOmittedColumnGeneratesSequentialValues) {
    run_bootstrap();
    exec_ok("CREATE TABLE seq_test (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");

    exec_ok("INSERT INTO seq_test (name) VALUES ('alice')");
    exec_ok("INSERT INTO seq_test (name) VALUES ('bob')");
    exec_ok("INSERT INTO seq_test (name) VALUES ('carol')");

    auto qr = exec_ok("SELECT id, name FROM seq_test ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[1][1].as_string(), "bob");
    EXPECT_EQ(qr.rows[2][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[2][1].as_string(), "carol");
}

TEST_F(AutoIncrementTest, InsertMultiRowOmittedColumn) {
    run_bootstrap();
    exec_ok("CREATE TABLE multi (id INT PRIMARY KEY AUTOINCREMENT, val VARCHAR)");

    exec_ok("INSERT INTO multi (val) VALUES ('a'), ('b'), ('c')");

    auto qr = exec_ok("SELECT id, val FROM multi ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 3);
}

// =============================================================================
// AC4: INSERT with explicit value advances counter
// =============================================================================

TEST_F(AutoIncrementTest, ExplicitValueAdvancesCounter) {
    run_bootstrap();
    exec_ok("CREATE TABLE ex_test (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");

    // Insert with explicit value 10.
    exec_ok("INSERT INTO ex_test (id, name) VALUES (10, 'alice')");

    // Next auto-generated value should be 11.
    exec_ok("INSERT INTO ex_test (name) VALUES ('bob')");

    auto qr = exec_ok("SELECT id, name FROM ex_test ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 10);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 11);
}

TEST_F(AutoIncrementTest, ExplicitSmallerValueDoesNotRewindCounter) {
    run_bootstrap();
    exec_ok("CREATE TABLE no_rewind (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO no_rewind (id, v) VALUES (100, 'jump')"); // counter -> 101
    exec_ok("INSERT INTO no_rewind (id, v) VALUES (50, 'back')");  // 50 < 101, no rewind
    exec_ok("INSERT INTO no_rewind (v) VALUES ('auto')");          // should be 101

    auto qr = exec_ok("SELECT id FROM no_rewind ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 50);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 100);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 101);
}

TEST_F(AutoIncrementTest, MixedExplicitAndAutoValues) {
    run_bootstrap();
    exec_ok("CREATE TABLE mixed (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO mixed (v) VALUES ('auto1')");         // id=1
    exec_ok("INSERT INTO mixed (id, v) VALUES (100, 'exp1')"); // id=100, counter->101
    exec_ok("INSERT INTO mixed (v) VALUES ('auto2')");         // id=101
    exec_ok("INSERT INTO mixed (id, v) VALUES (50, 'exp2')");  // id=50, counter stays 102
    exec_ok("INSERT INTO mixed (v) VALUES ('auto3')");         // id=102

    auto qr = exec_ok("SELECT id, v FROM mixed ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 50);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 100);
    EXPECT_EQ(qr.rows[3][0].as_int32(), 101);
    EXPECT_EQ(qr.rows[4][0].as_int32(), 102);
}

// =============================================================================
// AC6: Overflow protection (end-to-end via SQL)
// =============================================================================

TEST_F(AutoIncrementTest, OverflowTinyint) {
    run_bootstrap();
    exec_ok("CREATE TABLE ov_tiny (id TINYINT PRIMARY KEY AUTOINCREMENT, v INT)");

    // TINYINT (INT8) max is 127. Insert 127 explicitly to push counter to 128.
    exec_ok("INSERT INTO ov_tiny (id, v) VALUES (127, 1)");

    // Next auto-generated should overflow.
    exec_error("INSERT INTO ov_tiny (v) VALUES (2)", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(AutoIncrementTest, OverflowSmallint) {
    run_bootstrap();
    exec_ok("CREATE TABLE ov_small (id SMALLINT PRIMARY KEY AUTOINCREMENT, v INT)");

    exec_ok("INSERT INTO ov_small (id, v) VALUES (32767, 1)");

    exec_error("INSERT INTO ov_small (v) VALUES (2)", StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// AC8: SHOW COLUMNS displays AUTOINCREMENT attribute
// =============================================================================

TEST_F(AutoIncrementTest, ShowColumnsDisplaysAutoincrement) {
    run_bootstrap();
    exec_ok("CREATE TABLE show_test (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");

    auto qr = exec_ok("SHOW COLUMNS FROM show_test");
    ASSERT_EQ(qr.rows.size(), 2u);

    // Column output: name, type, nullable, default, autoincrement
    EXPECT_EQ(qr.rows[0][0].as_string(), "id");
    EXPECT_EQ(qr.rows[0][4].as_bool(), true);

    EXPECT_EQ(qr.rows[1][0].as_string(), "name");
    EXPECT_EQ(qr.rows[1][4].as_bool(), false);
}

// =============================================================================
// All SQL integer types: sequential generation
// =============================================================================

TEST_F(AutoIncrementTest, TinyintSequentialGeneration) {
    run_bootstrap();
    exec_ok("CREATE TABLE ai_tiny (id TINYINT PRIMARY KEY AUTOINCREMENT, v INT)");
    exec_ok("INSERT INTO ai_tiny (v) VALUES (10)");
    exec_ok("INSERT INTO ai_tiny (v) VALUES (20)");
    exec_ok("INSERT INTO ai_tiny (v) VALUES (30)");

    auto qr = exec_ok("SELECT id FROM ai_tiny ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int8(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int8(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int8(), 3);
}

TEST_F(AutoIncrementTest, SmallintSequentialGeneration) {
    run_bootstrap();
    exec_ok("CREATE TABLE ai_small (id SMALLINT PRIMARY KEY AUTOINCREMENT, v INT)");
    exec_ok("INSERT INTO ai_small (v) VALUES (10)");
    exec_ok("INSERT INTO ai_small (v) VALUES (20)");

    auto qr = exec_ok("SELECT id FROM ai_small ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int16(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int16(), 2);
}

TEST_F(AutoIncrementTest, BigintSequentialGeneration) {
    run_bootstrap();
    exec_ok("CREATE TABLE ai_big (id BIGINT PRIMARY KEY AUTOINCREMENT, v INT)");
    exec_ok("INSERT INTO ai_big (v) VALUES (10)");
    exec_ok("INSERT INTO ai_big (v) VALUES (20)");

    auto qr = exec_ok("SELECT id FROM ai_big ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int64(), 2);
}

// =============================================================================
// AC2: Catalog persistence (auto-increment survives restart)
// =============================================================================

TEST_F(AutoIncrementTest, AutoincrementPersistsAcrossRestart) {
    run_bootstrap();
    exec_ok("CREATE TABLE persist (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");
    exec_ok("INSERT INTO persist (name) VALUES ('a')"); // id=1
    exec_ok("INSERT INTO persist (name) VALUES ('b')"); // id=2
    exec_ok("INSERT INTO persist (name) VALUES ('c')"); // id=3

    // Restart: destroy and recreate everything from disk.
    restart();

    // Next auto value should be 4 (max existing = 3, so counter = 4).
    auto result = engine_->execute("INSERT INTO persist (name) VALUES ('d')");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto qr = engine_->execute("SELECT id, name FROM persist ORDER BY id");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 4u);
    EXPECT_EQ(qr->rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr->rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr->rows[2][0].as_int32(), 3);
    EXPECT_EQ(qr->rows[3][0].as_int32(), 4);
}

TEST_F(AutoIncrementTest, AutoincrementPersistsExplicitGap) {
    run_bootstrap();
    exec_ok("CREATE TABLE persist_gap (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO persist_gap (id, v) VALUES (100, 'jump')");

    restart();

    auto result = engine_->execute("INSERT INTO persist_gap (v) VALUES ('after')");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto qr = engine_->execute("SELECT id FROM persist_gap ORDER BY id");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 2u);
    EXPECT_EQ(qr->rows[0][0].as_int32(), 100);
    EXPECT_EQ(qr->rows[1][0].as_int32(), 101);
}

// =============================================================================
// Edge cases
// =============================================================================

TEST_F(AutoIncrementTest, InsertWithDefaultOnOtherColumn) {
    run_bootstrap();
    exec_ok("CREATE TABLE defv (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR DEFAULT 'anon')");

    exec_ok("INSERT INTO defv (name) VALUES ('hello')");

    auto qr = exec_ok("SELECT id, name FROM defv");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "hello");
}

TEST_F(AutoIncrementTest, SelectCountWithAutoincrement) {
    run_bootstrap();
    exec_ok("CREATE TABLE cnt (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO cnt (v) VALUES ('a'), ('b'), ('c')");

    auto qr = exec_ok("SELECT COUNT(*) FROM cnt");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 3);
}

TEST_F(AutoIncrementTest, DeleteDoesNotResetCounter) {
    run_bootstrap();
    exec_ok("CREATE TABLE del_test (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("INSERT INTO del_test (v) VALUES ('a'), ('b'), ('c')"); // ids 1,2,3

    exec_ok("DELETE FROM del_test WHERE id = 2");

    exec_ok("INSERT INTO del_test (v) VALUES ('d')"); // should be id=4, not 2

    auto qr = exec_ok("SELECT id FROM del_test ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 4);
}

TEST_F(AutoIncrementTest, TableWithoutAutoincrement) {
    run_bootstrap();
    exec_ok("CREATE TABLE normal (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO normal VALUES (1, 'a')");

    auto qr = exec_ok("SELECT id FROM normal");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

TEST_F(AutoIncrementTest, MultipleTablesIndependentCounters) {
    run_bootstrap();
    exec_ok("CREATE TABLE a (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");
    exec_ok("CREATE TABLE b (id INT PRIMARY KEY AUTOINCREMENT, v VARCHAR)");

    exec_ok("INSERT INTO a (v) VALUES ('a1')"); // a.id=1
    exec_ok("INSERT INTO a (v) VALUES ('a2')"); // a.id=2
    exec_ok("INSERT INTO b (v) VALUES ('b1')"); // b.id=1
    exec_ok("INSERT INTO a (v) VALUES ('a3')"); // a.id=3
    exec_ok("INSERT INTO b (v) VALUES ('b2')"); // b.id=2

    auto qr_a = exec_ok("SELECT id FROM a ORDER BY id");
    ASSERT_EQ(qr_a.rows.size(), 3u);
    EXPECT_EQ(qr_a.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr_a.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr_a.rows[2][0].as_int32(), 3);

    auto qr_b = exec_ok("SELECT id FROM b ORDER BY id");
    ASSERT_EQ(qr_b.rows.size(), 2u);
    EXPECT_EQ(qr_b.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr_b.rows[1][0].as_int32(), 2);
}

// =============================================================================
// Catalog-level unit tests for autoincrement counter
// =============================================================================

TEST(CatalogAutoIncrement, InitAndNext) {
    Catalog catalog;
    catalog.init_autoincrement(42, 1);

    auto v1 = catalog.next_autoincrement(42, TypeId::INT32);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1);

    auto v2 = catalog.next_autoincrement(42, TypeId::INT32);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 2);

    auto v3 = catalog.next_autoincrement(42, TypeId::INT32);
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v3, 3);
}

TEST(CatalogAutoIncrement, AdvancePastExplicit) {
    Catalog catalog;
    catalog.init_autoincrement(42, 1);

    catalog.advance_autoincrement(42, 100);
    EXPECT_EQ(catalog.get_autoincrement_counter(42), 101);

    auto v = catalog.next_autoincrement(42, TypeId::INT32);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 101);
}

TEST(CatalogAutoIncrement, AdvanceSmallerDoesNotRewind) {
    Catalog catalog;
    catalog.init_autoincrement(42, 50);

    catalog.advance_autoincrement(42, 10); // smaller than 50
    EXPECT_EQ(catalog.get_autoincrement_counter(42), 50);
}

TEST(CatalogAutoIncrement, OverflowInt8AtMax) {
    Catalog catalog;
    catalog.init_autoincrement(42, 128); // INT8 max is 127

    auto result = catalog.next_autoincrement(42, TypeId::INT8);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowUint8AtMax) {
    Catalog catalog;
    catalog.init_autoincrement(42, 256); // UINT8 max is 255

    auto result = catalog.next_autoincrement(42, TypeId::UINT8);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowInt16AtMax) {
    Catalog catalog;
    catalog.init_autoincrement(42, 32768); // INT16 max is 32767

    auto result = catalog.next_autoincrement(42, TypeId::INT16);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowUint16AtMax) {
    Catalog catalog;
    catalog.init_autoincrement(42, 65536); // UINT16 max is 65535

    auto result = catalog.next_autoincrement(42, TypeId::UINT16);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowInt32AtMax) {
    Catalog catalog;
    int64_t val = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
    catalog.init_autoincrement(42, val);

    auto result = catalog.next_autoincrement(42, TypeId::INT32);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowUint32AtMax) {
    Catalog catalog;
    int64_t val = static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    catalog.init_autoincrement(42, val);

    auto result = catalog.next_autoincrement(42, TypeId::UINT32);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowInt64AtMax) {
    Catalog catalog;
    catalog.init_autoincrement(42, std::numeric_limits<int64_t>::max());

    auto result = catalog.next_autoincrement(42, TypeId::INT64);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, OverflowUint64AtMax) {
    Catalog catalog;
    catalog.init_autoincrement(42, std::numeric_limits<int64_t>::max());

    auto result = catalog.next_autoincrement(42, TypeId::UINT64);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(CatalogAutoIncrement, NonExistentCounterErrors) {
    Catalog catalog;

    auto result = catalog.next_autoincrement(999, TypeId::INT32);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

TEST(CatalogAutoIncrement, GetCounterNonExistent) {
    Catalog catalog;
    EXPECT_EQ(catalog.get_autoincrement_counter(999), 0);
}

TEST(CatalogAutoIncrement, NonIntegerTypeErrors) {
    Catalog catalog;
    catalog.init_autoincrement(42, 1);

    auto result = catalog.next_autoincrement(42, TypeId::STRING);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}
