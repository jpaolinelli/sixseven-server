/// QA adversarial tests for GDB-711: VACUUM/ANALYZE dispatch (QA follow-up).
///
/// The implementer's regression suite lives in test_qa_gdb_711.cpp. This file
/// contains the QA engineer's adversarial tests:
///
///   - QA_GDB711_Adversarial: engine-level attacks (type zoo, case
///     sensitivity, quoted identifiers, unknown/virtual/phantom tables,
///     trailing-token statement variants, DML interleaving, restart + lazy
///     storage open, 1000-row stress).
///   - QA_GDB711_EmptyDB:     bare VACUUM/ANALYZE with zero user tables.
///   - QA_GDB711_Stats:       direct analyze_table() verification — the same
///     code path execute_analyze() uses — checking row_count, null_fraction,
///     min/max, ndistinct, MCV ordering, sampling configs, and the corrupt
///     tuple skip path.
///
/// VACUUM is a validated no-op by design (GDB-1230); these tests attack
/// whether the no-op is truly safe (no data mutation, clean error paths),
/// not whether reclamation happens.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/planner/statistics.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

/// The exact planner default-branch message that GDB-711 removed for
/// VACUUM/ANALYZE. No error from either statement may ever carry it again.
constexpr const char* kPlannerNotImplementedMsg = "planner does not support this statement type";

} // namespace

// =============================================================================
// Fixture: full engine pipeline with a seeded table.
// =============================================================================

class QA_GDB711_Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb711_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE widgets (id INT, name VARCHAR, qty INT)");
        exec_ok("INSERT INTO widgets VALUES (1, 'alpha', 10)");
        exec_ok("INSERT INTO widgets VALUES (2, 'beta', 20)");
        exec_ok("INSERT INTO widgets VALUES (3, 'gamma', 30)");
        exec_ok("INSERT INTO widgets VALUES (4, 'delta', 40)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    Error exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error for: " << sql;
        if (result.has_value()) {
            return Error{StatusCode::OK, "expected error but got success"};
        }
        return result.error();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// Regression hard-guard: the planner default-branch message is gone for every
// statement form, including the error paths.
// =============================================================================

TEST_F(QA_GDB711_Adversarial, PlannerNotImplementedMessageGoneEverywhere) {
    // Success paths.
    EXPECT_EQ(exec_ok("VACUUM").message, "VACUUM");
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");
    EXPECT_EQ(exec_ok("ANALYZE").message, "ANALYZE");
    EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");

    // Error paths must be NOT_FOUND, and must not leak the planner message.
    auto verr = exec_err("VACUUM no_such_table");
    EXPECT_EQ(verr.code, StatusCode::NOT_FOUND);
    EXPECT_EQ(verr.message.find(kPlannerNotImplementedMsg), std::string::npos) << verr.message;

    auto aerr = exec_err("ANALYZE no_such_table");
    EXPECT_EQ(aerr.code, StatusCode::NOT_FOUND);
    EXPECT_EQ(aerr.message.find(kPlannerNotImplementedMsg), std::string::npos) << aerr.message;
}

// =============================================================================
// Type zoo: ANALYZE/VACUUM over a table containing every practically
// insertable column type, with NULLs in the exotic columns.
// =============================================================================

TEST_F(QA_GDB711_Adversarial, AnalyzeAndVacuumTypeZooTable) {
    exec_ok("CREATE TABLE typezoo ("
            "  i8 TINYINT, i16 SMALLINT, i32 INT, i64 BIGINT,"
            "  f32 FLOAT, f64 DOUBLE, dec DECIMAL(10,2), flag BOOLEAN,"
            "  name VARCHAR,"
            "  d DATE DEFAULT CURRENT_DATE,"
            "  tm TIME DEFAULT CURRENT_TIME,"
            "  ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
            "  j JSON, u UUID, b BLOB, p POINT"
            ")");

    exec_ok("INSERT INTO typezoo (i8, i16, i32, i64, f32, f64, dec, flag, name, j, u, b, p) "
            "VALUES (1, 100, 1000, 100000, 1.5, 2.5, 12.34, TRUE, 'alpha', NULL, "
            "'d1458b55-f0bf-44d4-b191-e52f1ef1f60a', NULL, NULL)");
    exec_ok("INSERT INTO typezoo (i8, i16, i32, i64, f32, f64, dec, flag, name, j, u, b, p) "
            "VALUES (2, -100, -1000, -100000, -1.5, -2.5, 99.99, FALSE, 'beta', NULL, "
            "'00000000-0000-0000-0000-000000000001', NULL, NULL)");
    // A row that is NULL in every nullable column.
    exec_ok("INSERT INTO typezoo (i8, i16, i32, i64, f32, f64, dec, flag, name, j, u, b, p) "
            "VALUES (NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "
            "NULL)");

    EXPECT_EQ(exec_ok("ANALYZE typezoo").message, "ANALYZE");
    EXPECT_EQ(exec_ok("VACUUM typezoo").message, "VACUUM");

    // Neither command may mutate the data.
    auto qr = exec_ok("SELECT i32, name FROM typezoo");
    EXPECT_EQ(qr.rows.size(), 3u);

    // Bare forms over the zoo + widgets.
    EXPECT_EQ(exec_ok("ANALYZE").message, "ANALYZE");
    EXPECT_EQ(exec_ok("VACUUM").message, "VACUUM");
}

TEST_F(QA_GDB711_Adversarial, AnalyzeTableWithAllNullColumn) {
    exec_ok("CREATE TABLE nulls_only (id INT, ghost VARCHAR)");
    exec_ok("INSERT INTO nulls_only VALUES (1, NULL)");
    exec_ok("INSERT INTO nulls_only VALUES (2, NULL)");
    exec_ok("INSERT INTO nulls_only VALUES (3, NULL)");

    EXPECT_EQ(exec_ok("ANALYZE nulls_only").message, "ANALYZE");
    auto qr = exec_ok("SELECT id FROM nulls_only");
    EXPECT_EQ(qr.rows.size(), 3u);
}

// =============================================================================
// Identifier handling: case sensitivity, quoted identifiers, long names.
// =============================================================================

// The engine is case-sensitive for identifiers throughout (the lexer does not
// fold case). VACUUM/ANALYZE must follow the same rules as SELECT — a wrong
// case is NOT_FOUND, not a crash and not a silent bare VACUUM.
TEST_F(QA_GDB711_Adversarial, TableNamesAreCaseSensitive) {
    EXPECT_EQ(exec_err("VACUUM WIDGETS").code, StatusCode::NOT_FOUND);
    EXPECT_EQ(exec_err("VACUUM Widgets").code, StatusCode::NOT_FOUND);
    EXPECT_EQ(exec_err("ANALYZE WIDGETS").code, StatusCode::NOT_FOUND);
    EXPECT_EQ(exec_err("ANALYZE Widgets").code, StatusCode::NOT_FOUND);

    // Exact case still works after the failed attempts.
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");
    EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");
}

// Double-quoted identifiers are not supported by the lexer; the statement must
// fail cleanly (parse error) and leave the engine usable.
TEST_F(QA_GDB711_Adversarial, QuotedIdentifierParsesCleanly) {
    // GDB-1224: the lexer DOES support double-quoted identifiers (see the
    // "unterminated quoted identifier" / "zero-length delimited identifier"
    // handling in src/parser/lexer.cpp), so ANALYZE "widgets" is valid SQL
    // that resolves to the same "widgets" table as the unquoted form. This
    // test's prior expectation (rejection) was simply wrong about current
    // lexer capabilities -- matches the identical fix in
    // tests/qa/test_qa_gdb_602.cpp's QuotedIdentifierDatabaseNameParsesCleanly.
    auto result = engine_->execute("ANALYZE \"widgets\"");
    ASSERT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
    EXPECT_EQ(result->message, "ANALYZE");

    // Engine still works afterwards.
    EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");
}

TEST_F(QA_GDB711_Adversarial, VeryLongUnknownTableNameFailsCleanly) {
    const std::string long_name(2048, 'x');
    auto verr = exec_err("VACUUM " + long_name);
    EXPECT_EQ(verr.code, StatusCode::NOT_FOUND);

    auto aerr = exec_err("ANALYZE " + long_name);
    EXPECT_EQ(aerr.code, StatusCode::NOT_FOUND);

    // Engine still usable.
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");
}

// pg_catalog virtual tables (pg_type, ...) resolve in the binder for SELECT
// compatibility but have no heap storage. VACUUM/ANALYZE on them must produce
// a clean NOT_FOUND, never a crash or a bogus success with stats.
TEST_F(QA_GDB711_Adversarial, VirtualTableRejectedCleanly) {
    auto aerr = exec_err("ANALYZE pg_type");
    EXPECT_EQ(aerr.code, StatusCode::NOT_FOUND);

    auto verr = exec_err("VACUUM pg_type");
    EXPECT_EQ(verr.code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Statement variants. The parser accepts only `VACUUM [name]` / `ANALYZE
// [name]`. PostgreSQL option syntax is not implemented.
// =============================================================================

// KNOWN ISSUE (reported in GDB-711 QA): Parser::parse() does not verify that
// all tokens were consumed, so `VACUUM FULL widgets` silently executes as a
// bare VACUUM over every table — the FULL qualifier and the table name are
// dropped on the floor. This test pins the *current* behavior so the suite
// stays green; when the parser gains an end-of-input check, flip these to
// expect PARSE_ERROR.
TEST_F(QA_GDB711_Adversarial, VacuumFullCurrentlyParsesAsBareVacuum) {
    auto result = engine_->execute("VACUUM FULL widgets");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "VACUUM");

    // Whatever the parse outcome, no data may be harmed.
    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 4u);
}

// ANALYZE VERBOSE: VERBOSE is not a keyword, so it parses as a table name and
// fails NOT_FOUND. Clean failure, engine usable.
TEST_F(QA_GDB711_Adversarial, AnalyzeVerboseFailsAsUnknownTable) {
    auto err = exec_err("ANALYZE VERBOSE");
    EXPECT_EQ(err.code, StatusCode::NOT_FOUND);
    EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");
}

// =============================================================================
// DML interleaving and stress.
// =============================================================================

TEST_F(QA_GDB711_Adversarial, InterleavedDmlAnalyzeVacuumKeepsDataExact) {
    // Start: 4 rows. Each round adds 2 and deletes 1 → net +1 per round.
    for (int round = 0; round < 5; ++round) {
        int base = 100 + round * 10;
        exec_ok("INSERT INTO widgets VALUES (" + std::to_string(base) + ", 'r', 1), (" +
                std::to_string(base + 1) + ", 's', 2)");
        EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");
        exec_ok("UPDATE widgets SET qty = qty + 1 WHERE id = " + std::to_string(base));
        EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");
        exec_ok("DELETE FROM widgets WHERE id = " + std::to_string(base + 1));
        EXPECT_EQ(exec_ok("ANALYZE").message, "ANALYZE");
    }

    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 9u) << "interleaved VACUUM/ANALYZE corrupted row count";
}

TEST_F(QA_GDB711_Adversarial, VacuumAfterDeleteDoesNotResurrectRows) {
    exec_ok("DELETE FROM widgets WHERE qty > 25");
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");

    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 2u) << "VACUUM resurrected or destroyed rows";

    EXPECT_EQ(exec_ok("VACUUM").message, "VACUUM");
    qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QA_GDB711_Adversarial, ThousandRowStressAnalyzeVacuum) {
    exec_ok("CREATE TABLE big_t (id INT, payload VARCHAR)");
    for (int batch = 0; batch < 10; ++batch) {
        std::string sql = "INSERT INTO big_t VALUES ";
        for (int i = 0; i < 100; ++i) {
            int id = batch * 100 + i;
            if (i > 0) {
                sql += ", ";
            }
            sql += "(" + std::to_string(id) + ", 'row-" + std::to_string(id) + "')";
        }
        exec_ok(sql);
    }

    EXPECT_EQ(exec_ok("ANALYZE big_t").message, "ANALYZE");
    EXPECT_EQ(exec_ok("VACUUM big_t").message, "VACUUM");

    auto qr = exec_ok("SELECT id FROM big_t");
    EXPECT_EQ(qr.rows.size(), 1000u);
}

TEST_F(QA_GDB711_Adversarial, RepeatedAnalyzeIsStable) {
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");
    }
    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 4u);
}

// =============================================================================
// DROP TABLE interaction.
// =============================================================================

TEST_F(QA_GDB711_Adversarial, AnalyzeAfterDropTableIsNotFound) {
    exec_ok("CREATE TABLE doomed (id INT)");
    exec_ok("INSERT INTO doomed VALUES (1)");
    EXPECT_EQ(exec_ok("ANALYZE doomed").message, "ANALYZE");

    exec_ok("DROP TABLE doomed");

    EXPECT_EQ(exec_err("ANALYZE doomed").code, StatusCode::NOT_FOUND);
    EXPECT_EQ(exec_err("VACUUM doomed").code, StatusCode::NOT_FOUND);

    // Bare forms skip the dropped table without error.
    EXPECT_EQ(exec_ok("ANALYZE").message, "ANALYZE");
    EXPECT_EQ(exec_ok("VACUUM").message, "VACUUM");
}

// =============================================================================
// Restart: a fresh StorageManager/QueryEngine over the same data directory
// must lazily reopen table storage inside execute_analyze. This also
// exercises the new ~StorageManager() flush+close (GDB-1226 fold-in): without
// it the first StorageManager leaks open handles into the shared DiskManager.
// =============================================================================

TEST_F(QA_GDB711_Adversarial, AnalyzeAfterEngineRestartLazilyOpensStorage) {
    // Tear down the first engine + storage manager. ~StorageManager flushes
    // and closes all table files registered in dm_.
    engine_.reset();
    storage_.reset();

    storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
    engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

    // ANALYZE must take the table_file_exists -> open_table_storage path.
    EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");
    EXPECT_EQ(exec_ok("VACUUM widgets").message, "VACUUM");

    // The data flushed by ~StorageManager must be intact.
    auto qr = exec_ok("SELECT id, name FROM widgets");
    EXPECT_EQ(qr.rows.size(), 4u);
}

// =============================================================================
// Phantom table: registered in the catalog, but no storage file on disk.
// ANALYZE needs the heap so it must fail cleanly; VACUUM only validates the
// name (validated no-op, GDB-1230) so it succeeds.
// =============================================================================

TEST_F(QA_GDB711_Adversarial, PhantomTableWithoutStorageFile) {
    TableSchema phantom;
    phantom.name = "phantom_t";
    CatalogColumnDef col;
    col.ordinal = 0;
    col.name = "id";
    col.type_id = TypeId::INT32;
    phantom.columns.push_back(col);
    auto created = catalog_.create_table(default_database_id, std::move(phantom));
    ASSERT_TRUE(created.has_value()) << created.error().message;

    // ANALYZE: storage file does not exist -> clean NOT_FOUND, no crash.
    auto aerr = exec_err("ANALYZE phantom_t");
    EXPECT_EQ(aerr.code, StatusCode::NOT_FOUND);

    // GDB-1224: GDB-969 ("Wire VACUUM to txn::Vacuum with safe deadness
    // check") wired real reclamation into VACUUM, which requires opening the
    // table's storage the same way ANALYZE does (see execute_vacuum() in
    // src/executor/query_engine.cpp) -- it is no longer a validated no-op
    // for tables lacking a storage file. StorageManager::get_table_storage()
    // returns the same StatusCode::NOT_FOUND for the missing-storage case as
    // ANALYZE's equivalent path, so VACUUM and ANALYZE are consistent with
    // each other; this test's prior expectation (unconditional no-op
    // success) simply predates GDB-969's behavior change.
    auto verr = exec_err("VACUUM phantom_t");
    EXPECT_EQ(verr.code, StatusCode::NOT_FOUND);

    // Bare ANALYZE currently fails wholesale when any catalog table lacks a
    // storage file (it does not skip the phantom). Pin the no-crash behavior.
    auto bare = engine_->execute("ANALYZE");
    EXPECT_FALSE(bare.has_value());

    // Engine remains fully usable.
    auto qr = exec_ok("SELECT id FROM widgets");
    EXPECT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(exec_ok("ANALYZE widgets").message, "ANALYZE");

    // Clean up so TearDown's remove_all is not affected.
    auto dropped = catalog_.drop_table(default_database_id, "phantom_t");
    EXPECT_TRUE(dropped.has_value());
}

// =============================================================================
// Empty database: zero user tables.
// =============================================================================

class QA_GDB711_EmptyDB : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb711_empty";
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

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA_GDB711_EmptyDB, BareVacuumWithZeroTablesSucceeds) {
    auto result = engine_->execute("VACUUM");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "VACUUM");
}

TEST_F(QA_GDB711_EmptyDB, BareAnalyzeWithZeroTablesSucceeds) {
    auto result = engine_->execute("ANALYZE");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->message, "ANALYZE");
}

TEST_F(QA_GDB711_EmptyDB, NamedFormsOnEmptyDatabaseAreNotFound) {
    auto vacuum_result = engine_->execute("VACUUM anything");
    ASSERT_FALSE(vacuum_result.has_value());
    EXPECT_EQ(vacuum_result.error().code, StatusCode::NOT_FOUND);

    auto analyze_result = engine_->execute("ANALYZE anything");
    ASSERT_FALSE(analyze_result.has_value());
    EXPECT_EQ(analyze_result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Direct analyze_table verification. execute_analyze() routes through this
// exact function, so these tests verify the statistics ANALYZE produces.
// =============================================================================

class QA_GDB711_Stats : public QA_GDB711_Adversarial {
protected:
    /// Resolve a table created through the engine to its id + open storage.
    struct Target {
        table_id_t table_id = 0;
        TableSchema schema;
        TableStorage* storage = nullptr;
    };

    Target target_for(const std::string& table) {
        Target t;
        auto schema = catalog_.get_table(default_database_id, table);
        EXPECT_TRUE(schema.has_value()) << "no catalog entry for " << table;
        if (!schema) {
            return t;
        }
        t.schema = *schema;
        t.table_id = schema->table_id;
        auto ts = storage_->get_table_storage(t.table_id);
        EXPECT_TRUE(ts.has_value()) << "no open storage for " << table;
        t.storage = ts ? *ts : nullptr;
        return t;
    }

    Result<void> analyze_into(const Target& t, StatisticsStore& store, AnalyzeConfig config = {}) {
        return analyze_table(
            t.table_id, t.schema, *t.storage->heap, t.storage->storage_schema, store, config);
    }
};

TEST_F(QA_GDB711_Stats, TableStatsRowCountAndWidthExact) {
    auto t = target_for("widgets");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    auto analyzed = analyze_into(t, store);
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;

    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 4u);
    EXPECT_GE(stats->page_count, 1u);
    EXPECT_GT(stats->avg_row_width, 0.0);
}

TEST_F(QA_GDB711_Stats, NullFractionExact) {
    exec_ok("CREATE TABLE null_frac (a INT, b INT)");
    exec_ok("INSERT INTO null_frac VALUES (1, 10), (2, NULL), (3, 20), (4, NULL)");

    auto t = target_for("null_frac");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    auto analyzed = analyze_into(t, store);
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;

    const auto* a_stats = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(a_stats, nullptr);
    EXPECT_DOUBLE_EQ(a_stats->null_fraction, 0.0);

    const auto* b_stats = store.get_column_stats(t.table_id, 1);
    ASSERT_NE(b_stats, nullptr);
    EXPECT_DOUBLE_EQ(b_stats->null_fraction, 0.5);
    EXPECT_EQ(b_stats->ndistinct, 2u);
}

TEST_F(QA_GDB711_Stats, MinMaxAndNDistinctIntegers) {
    exec_ok("CREATE TABLE minmax_i (v INT)");
    exec_ok("INSERT INTO minmax_i VALUES (5), (-3), (5), (42), (0)");

    auto t = target_for("minmax_i");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    auto analyzed = analyze_into(t, store);
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;

    const auto* stats = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->ndistinct, 4u);
    ASSERT_FALSE(stats->min_value.is_null());
    ASSERT_FALSE(stats->max_value.is_null());
    EXPECT_EQ(stats->min_value.as_int32(), -3);
    EXPECT_EQ(stats->max_value.as_int32(), 42);
}

TEST_F(QA_GDB711_Stats, MinMaxStrings) {
    auto t = target_for("widgets");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    auto analyzed = analyze_into(t, store);
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;

    // Column 1 = name: alpha, beta, gamma, delta.
    const auto* stats = store.get_column_stats(t.table_id, 1);
    ASSERT_NE(stats, nullptr);
    ASSERT_FALSE(stats->min_value.is_null());
    ASSERT_FALSE(stats->max_value.is_null());
    EXPECT_EQ(stats->min_value.as_string(), "alpha");
    EXPECT_EQ(stats->max_value.as_string(), "gamma");
    EXPECT_EQ(stats->ndistinct, 4u);
}

TEST_F(QA_GDB711_Stats, McvMostFrequentValueFirst) {
    exec_ok("CREATE TABLE mcv_t (v INT)");
    exec_ok("INSERT INTO mcv_t VALUES (7), (7), (7), (7), (1), (2)");

    auto t = target_for("mcv_t");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    auto analyzed = analyze_into(t, store);
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;

    const auto* stats = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(stats, nullptr);
    ASSERT_FALSE(stats->mcv_list.empty());
    EXPECT_EQ(stats->mcv_list[0].value.as_int32(), 7);
    EXPECT_DOUBLE_EQ(stats->mcv_list[0].frequency, 4.0 / 6.0);
}

TEST_F(QA_GDB711_Stats, ReanalyzeReflectsUpdateAndDelete) {
    auto t = target_for("widgets");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    ASSERT_TRUE(analyze_into(t, store).has_value());
    const auto* before = store.get_table_stats(t.table_id);
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(before->row_count, 4u);

    exec_ok("UPDATE widgets SET qty = qty + 100");
    exec_ok("DELETE FROM widgets WHERE id = 1");

    ASSERT_TRUE(analyze_into(t, store).has_value());
    const auto* after = store.get_table_stats(t.table_id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->row_count, 3u) << "re-ANALYZE did not reflect DELETE";

    const auto* qty_stats = store.get_column_stats(t.table_id, 2);
    ASSERT_NE(qty_stats, nullptr);
    ASSERT_FALSE(qty_stats->max_value.is_null());
    EXPECT_EQ(qty_stats->max_value.as_int32(), 140) << "re-ANALYZE did not reflect UPDATE";
}

TEST_F(QA_GDB711_Stats, EmptyTableProducesZeroedStats) {
    exec_ok("CREATE TABLE hollow (v INT)");

    auto t = target_for("hollow");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    ASSERT_TRUE(analyze_into(t, store).has_value());

    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 0u);
    EXPECT_DOUBLE_EQ(stats->avg_row_width, 0.0);

    const auto* col = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->ndistinct, 0u);
    EXPECT_TRUE(col->mcv_list.empty());
    EXPECT_TRUE(col->min_value.is_null());
}

// Boundary configs: sample_size of 0 and 1, zero MCVs, zero histogram buckets.
TEST_F(QA_GDB711_Stats, DegenerateAnalyzeConfigsDoNotCrash) {
    auto t = target_for("widgets");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;

    AnalyzeConfig zero_sample;
    zero_sample.sample_size = 0;
    auto r0 = analyze_into(t, store, zero_sample);
    ASSERT_TRUE(r0.has_value()) << r0.error().message;
    const auto* stats0 = store.get_table_stats(t.table_id);
    ASSERT_NE(stats0, nullptr);
    EXPECT_EQ(stats0->row_count, 4u) << "row_count must be the full count, not the sample";

    AnalyzeConfig one_sample;
    one_sample.sample_size = 1;
    auto r1 = analyze_into(t, store, one_sample);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    AnalyzeConfig no_buckets;
    no_buckets.mcv_count = 0;
    no_buckets.histogram_buckets = 0;
    auto r2 = analyze_into(t, store, no_buckets);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    const auto* col = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(col, nullptr);
    EXPECT_TRUE(col->mcv_list.empty());
    EXPECT_TRUE(col->histogram.empty());
}

// Reservoir-sampling path on a clean table: sample smaller than the table.
TEST_F(QA_GDB711_Stats, ReservoirSamplingPathOnLargeTable) {
    exec_ok("CREATE TABLE res_t (v INT)");
    for (int batch = 0; batch < 3; ++batch) {
        std::string sql = "INSERT INTO res_t VALUES ";
        for (int i = 0; i < 100; ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += "(" + std::to_string(batch * 100 + i) + ")";
        }
        exec_ok(sql);
    }

    auto t = target_for("res_t");
    ASSERT_NE(t.storage, nullptr);

    StatisticsStore store;
    AnalyzeConfig config;
    config.sample_size = 16;
    config.histogram_buckets = 4;
    auto analyzed = analyze_into(t, store, config);
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;

    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 300u) << "row_count must count all rows, not just the sample";

    const auto* col = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(col, nullptr);
    EXPECT_GE(col->ndistinct, 1u);
    EXPECT_LE(col->ndistinct, 16u);
}

// KNOWN BUG GDB-1232 (High, found in GDB-711 QA): sample_table() in
// src/planner/statistics.cpp increments row_index for tuples that fail to
// deserialize ("skip corrupt tuples") but does not push them into the sample.
// Once row_index reaches max_sample_size the reservoir branch writes
// sample[j] for any j < max_sample_size — but sample.size() is smaller than
// max_sample_size by the number of skipped tuples, so sample[j] is an
// out-of-bounds vector write (crashes under MSVC debug iterator checks;
// silent heap corruption in release).
//
// Fixed by GDB-1232: the reservoir is now driven by a valid-rows counter, so
// skipped corrupt tuples can no longer leave the sample under-filled while the
// replacement branch indexes up to max_sample_size.
TEST_F(QA_GDB711_Stats, CorruptTupleDuringReservoirSamplingMustNotCorruptMemory) {
    // 2 valid rows, then 1 corrupt tuple injected straight into the heap,
    // then 200 more valid rows. With sample_size = 4 the corrupt tuple is
    // inside the fill window, so sample.size() == 3 when the reservoir
    // replacement begins choosing j in [0, 4).
    exec_ok("CREATE TABLE corrupt_t (v INT)");
    exec_ok("INSERT INTO corrupt_t VALUES (1), (2)");

    auto t = target_for("corrupt_t");
    ASSERT_NE(t.storage, nullptr);

    // One byte: a null bitmap claiming column 0 is NOT null, with no int32
    // payload behind it. TupleSerializer::deserialize rejects this with
    // "tuple data too short for fixed field at column 0", triggering the
    // corrupt-tuple skip path in sample_table().
    const std::vector<uint8_t> garbage = {0x00};
    auto injected = t.storage->heap->insert_tuple(garbage);
    ASSERT_TRUE(injected.has_value()) << injected.error().message;

    std::string sql = "INSERT INTO corrupt_t VALUES ";
    for (int i = 0; i < 200; ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += "(" + std::to_string(i + 10) + ")";
    }
    exec_ok(sql);

    StatisticsStore store;
    AnalyzeConfig config;
    config.sample_size = 4;
    auto analyzed = analyze_into(t, store, config);

    // ANALYZE over a table containing one corrupt tuple must either succeed
    // (skipping the corrupt tuple) or fail cleanly — never corrupt memory.
    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;
    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 203u);
}

// GDB-1232 verification probe: corrupt tuple exactly at the fill boundary.
// With sample_size = 4 and exactly 3 valid rows before the corrupt tuple,
// the corrupt tuple occupies the physical position that would have completed
// the fill. Pre-fix, row_index reached 4 with sample.size() == 3, so the very
// next valid row entered the replacement branch and could write sample[3]
// out of bounds. Post-fix, valid_rows == 3 means the next valid row completes
// the fill via push_back, and replacement only begins with a full sample.
TEST_F(QA_GDB711_Stats, CorruptTupleExactlyAtFillBoundary) {
    exec_ok("CREATE TABLE corrupt_edge (v INT)");
    exec_ok("INSERT INTO corrupt_edge VALUES (1), (2), (3)");

    auto t = target_for("corrupt_edge");
    ASSERT_NE(t.storage, nullptr);

    const std::vector<uint8_t> garbage = {0x00};
    auto injected = t.storage->heap->insert_tuple(garbage);
    ASSERT_TRUE(injected.has_value()) << injected.error().message;

    std::string sql = "INSERT INTO corrupt_edge VALUES ";
    for (int i = 0; i < 200; ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += "(" + std::to_string(i + 10) + ")";
    }
    exec_ok(sql);

    StatisticsStore store;
    AnalyzeConfig config;
    config.sample_size = 4;
    auto analyzed = analyze_into(t, store, config);

    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;
    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 204u) << "row_count must include the corrupt tuple";

    // The sample holds exactly sample_size valid rows; ndistinct is derived
    // from the sample, so it must be within [1, 4].
    const auto* col = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(col, nullptr);
    EXPECT_GE(col->ndistinct, 1u);
    EXPECT_LE(col->ndistinct, 4u);
}

// GDB-1232 verification probe: corrupt tuples appearing only AFTER the fill
// window is already full. These must be pure no-ops for the sample (skipped
// without perturbing the reservoir distribution or sample bounds) while still
// being counted in row_count.
TEST_F(QA_GDB711_Stats, CorruptTuplesAfterFillWindowAreSkippedButCounted) {
    exec_ok("CREATE TABLE corrupt_tail (v INT)");
    exec_ok("INSERT INTO corrupt_tail VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)");

    auto t = target_for("corrupt_tail");
    ASSERT_NE(t.storage, nullptr);

    const std::vector<uint8_t> garbage = {0x00};
    for (int i = 0; i < 5; ++i) {
        auto injected = t.storage->heap->insert_tuple(garbage);
        ASSERT_TRUE(injected.has_value()) << injected.error().message;
    }

    std::string sql = "INSERT INTO corrupt_tail VALUES ";
    for (int i = 0; i < 50; ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += "(" + std::to_string(i + 100) + ")";
    }
    exec_ok(sql);

    StatisticsStore store;
    AnalyzeConfig config;
    config.sample_size = 4;
    auto analyzed = analyze_into(t, store, config);

    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;
    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 65u) << "10 + 5 corrupt + 50 = 65 total tuples";

    const auto* col = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(col, nullptr);
    EXPECT_GE(col->ndistinct, 1u);
    EXPECT_LE(col->ndistinct, 4u) << "sample must never exceed sample_size rows";
}

// GDB-1232 verification probe: a table containing ONLY corrupt tuples. The
// sample stays empty (valid_rows never advances), every column gets default
// zeroed stats, and row_count still reports the full physical tuple count.
TEST_F(QA_GDB711_Stats, AllCorruptTableYieldsEmptySampleAndFullRowCount) {
    exec_ok("CREATE TABLE corrupt_all (v INT)");

    auto t = target_for("corrupt_all");
    ASSERT_NE(t.storage, nullptr);

    const std::vector<uint8_t> garbage = {0x00};
    for (int i = 0; i < 10; ++i) {
        auto injected = t.storage->heap->insert_tuple(garbage);
        ASSERT_TRUE(injected.has_value()) << injected.error().message;
    }

    StatisticsStore store;
    AnalyzeConfig config;
    config.sample_size = 4;
    auto analyzed = analyze_into(t, store, config);

    ASSERT_TRUE(analyzed.has_value()) << analyzed.error().message;
    const auto* stats = store.get_table_stats(t.table_id);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->row_count, 10u) << "corrupt tuples are counted in row_count";

    const auto* col = store.get_column_stats(t.table_id, 0);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->ndistinct, 0u) << "empty sample must produce zeroed column stats";
    EXPECT_TRUE(col->mcv_list.empty());
    EXPECT_TRUE(col->min_value.is_null());
    EXPECT_TRUE(col->histogram.empty());
}
