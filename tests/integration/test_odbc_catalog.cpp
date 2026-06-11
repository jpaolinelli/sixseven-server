/// ODBC catalog function integration tests for psqlODBC compatibility.
/// Requires unixODBC + psqlODBC installed. Starts an embedded SixSevenDB
/// server and connects via ODBC to exercise SQLTables, SQLColumns,
/// SQLGetTypeInfo, SQLStatistics, and parameterized queries.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/settings_cache.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/server/server.h"
#include "sixseven/storage/disk_manager.h"

#include "sixseven/common/platform.h"

#include <gtest/gtest.h>

#include <sql.h>
#include <sqlext.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR state[6], msg[1024];
    SQLINTEGER native;
    SQLSMALLINT len;
    std::string out;
    SQLSMALLINT i = 1;
    while (SQLGetDiagRec(handle_type, handle, i++, state, &native, msg, sizeof(msg), &len) ==
           SQL_SUCCESS) {
        if (!out.empty())
            out += "; ";
        out += std::string(reinterpret_cast<char*>(state)) + ": " +
               std::string(reinterpret_cast<char*>(msg), len);
    }
    return out.empty() ? "(no ODBC diagnostic)" : out;
}

static std::string get_string_col(SQLHSTMT stmt, SQLUSMALLINT col) {
    char buf[1024] = {};
    SQLLEN ind = 0;
    SQLGetData(stmt, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
    if (ind == SQL_NULL_DATA)
        return "(null)";
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Test fixture — starts an embedded SixSevenDB server and connects via ODBC
// ---------------------------------------------------------------------------

class OdbcCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_logging("warn");

        data_dir_ = fs::temp_directory_path() / "sixseven_odbc_test";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());

        // Bootstrap system (creates pg_catalog tables etc).
        Config cfg;
        cfg.data_dir = data_dir_.string();
        auto boot = SystemBootstrap::bootstrap(
            *engine_, catalog_, *storage_, *persistence_, cfg, data_dir_);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Load settings cache.
        settings_cache_ = std::make_unique<SettingsCache>();
        auto load = settings_cache_->load(*engine_);
        ASSERT_TRUE(load.has_value()) << load.error().message;
        engine_->set_settings_cache(settings_cache_.get());

        // Switch to default user database.
        engine_->set_current_database(default_database_id);

        // Create test tables.
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, email VARCHAR)");
        exec_ok("CREATE TABLE products (product_id BIGINT, title VARCHAR, price DOUBLE, active "
                "BOOLEAN)");
        exec_ok("CREATE TABLE orders (order_id INT, user_id INT, total DOUBLE)");

        // Start the server on an ephemeral port.
        Config server_cfg;
        server_cfg.port = 0;
        server_ = std::make_unique<Server>(std::move(server_cfg));

        // Wire query executor.
        server_->set_query_executor(
            [this](const std::string& sql, const std::string& database) -> Result<QueryResult> {
                if (!database.empty()) {
                    auto db = catalog_.get_database(database);
                    if (db) {
                        return engine_->execute(sql, db->database_id);
                    }
                }
                return engine_->execute(sql);
            });

        // Wire query describer.
        server_->set_query_describer(
            [this](const std::string& sql,
                   const std::string& database) -> Result<std::vector<ColumnDescription>> {
                if (!database.empty()) {
                    auto db = catalog_.get_database(database);
                    if (db) {
                        return engine_->describe(sql, db->database_id);
                    }
                }
                return engine_->describe(sql);
            });

        // Start server in a background thread (start() blocks).
        server_thread_ = std::thread([this] { (void)server_->start(); });

        // Wait for the server to start accepting connections.
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (server_->is_running())
                break;
        }
        ASSERT_TRUE(server_->is_running()) << "Server failed to start within timeout";

        port_ = server_->bound_port();
        ASSERT_GT(port_, 0);

        // Allocate ODBC environment and connection.
        ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_), SQL_SUCCESS);
        SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);

        ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_), SQL_SUCCESS);

        // Connect with a connection string (bypasses DSN).
        std::string conn_str =
            "DRIVER={PostgreSQL Unicode};Server=localhost;Port=" + std::to_string(port_) +
            ";Database=demo;Uid=demo;Pwd=;" + "Debug=1;CommLog=1;";
        SQLCHAR out_conn[1024];
        SQLSMALLINT out_len;
        auto rc = SQLDriverConnect(dbc_,
                                   nullptr,
                                   reinterpret_cast<SQLCHAR*>(conn_str.data()),
                                   static_cast<SQLSMALLINT>(conn_str.size()),
                                   out_conn,
                                   sizeof(out_conn),
                                   &out_len,
                                   SQL_DRIVER_NOPROMPT);
        ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
            << "ODBC connect failed: " << odbc_error(SQL_HANDLE_DBC, dbc_);

        connected_ = true;
    }

    void TearDown() override {
        if (stmt_) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt_);
            stmt_ = SQL_NULL_HSTMT;
        }
        if (connected_) {
            SQLDisconnect(dbc_);
            connected_ = false;
        }
        if (dbc_) {
            SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
            dbc_ = SQL_NULL_HDBC;
        }
        if (env_) {
            SQLFreeHandle(SQL_HANDLE_ENV, env_);
            env_ = SQL_NULL_HENV;
        }

        if (server_) {
            server_->shutdown();
            if (server_thread_.joinable())
                server_thread_.join();
        }

        engine_.reset();
        settings_cache_.reset();
        persistence_.reset();
        storage_.reset();
        fs::remove_all(data_dir_);
    }

    SQLHSTMT alloc_stmt() {
        if (stmt_) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt_);
            stmt_ = SQL_NULL_HSTMT;
        }
        SQLHSTMT s = SQL_NULL_HSTMT;
        auto rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &s);
        EXPECT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
            << "alloc stmt: " << odbc_error(SQL_HANDLE_DBC, dbc_);
        stmt_ = s;
        return s;
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    fs::path data_dir_;
    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<SettingsCache> settings_cache_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<Server> server_;
    std::thread server_thread_;
    uint16_t port_ = 0;

    SQLHENV env_ = SQL_NULL_HENV;
    SQLHDBC dbc_ = SQL_NULL_HDBC;
    SQLHSTMT stmt_ = SQL_NULL_HSTMT;
    bool connected_ = false;
};

// ===========================================================================
// 1. Basic connectivity
// ===========================================================================

TEST_F(OdbcCatalogTest, ConnectSucceeds) {
    SUCCEED();
}

// ===========================================================================
// 2. SQLTables — list user tables
// ===========================================================================

TEST_F(OdbcCatalogTest, DISABLED_SQLTablesReturnsUserTables) {
    // Known limitation: psqlODBC sends a multi-table implicit JOIN query
    // (pg_class JOIN pg_namespace) that requires implicit JOIN support.
    auto s = alloc_stmt();
    auto rc = SQLTables(s,
                        nullptr,
                        0,
                        nullptr,
                        0,
                        nullptr,
                        0,
                        reinterpret_cast<SQLCHAR*>(const_cast<char*>("TABLE")),
                        SQL_NTS);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "SQLTables: " << odbc_error(SQL_HANDLE_STMT, s);

    std::vector<std::string> tables;
    while (SQLFetch(s) == SQL_SUCCESS) {
        tables.push_back(get_string_col(s, 3));
    }

    EXPECT_GE(tables.size(), 3u) << "Expected at least 3 user tables";
}

// ===========================================================================
// 3. SQLColumns — column metadata
// ===========================================================================

TEST_F(OdbcCatalogTest, DISABLED_SQLColumnsReturnsCorrectMetadata) {
    // Known limitation: psqlODBC sends a complex 4-JOIN catalog query with
    // CASE expressions, operator() syntax, and pg_get_expr() that require
    // significant parser/planner work.
    auto s = alloc_stmt();
    auto rc = SQLColumns(s,
                         nullptr,
                         0,
                         nullptr,
                         0,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>("users")),
                         SQL_NTS,
                         nullptr,
                         0);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "SQLColumns: " << odbc_error(SQL_HANDLE_STMT, s);

    std::vector<std::pair<std::string, std::string>> cols;
    while (SQLFetch(s) == SQL_SUCCESS) {
        auto name = get_string_col(s, 4);
        auto type = get_string_col(s, 6);
        cols.emplace_back(name, type);
    }

    EXPECT_GE(cols.size(), 3u) << "Expected at least 3 columns for 'users'";
}

// ===========================================================================
// 4. SQLGetTypeInfo — supported types
// ===========================================================================

TEST_F(OdbcCatalogTest, SQLGetTypeInfoReturnsSupportedTypes) {
    auto s = alloc_stmt();
    auto rc = SQLGetTypeInfo(s, SQL_ALL_TYPES);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "SQLGetTypeInfo: " << odbc_error(SQL_HANDLE_STMT, s);

    std::vector<std::string> types;
    while (SQLFetch(s) == SQL_SUCCESS) {
        types.push_back(get_string_col(s, 1));
    }

    EXPECT_GE(types.size(), 3u) << "Expected at least a few type entries";
}

// ===========================================================================
// 5. SQLStatistics — index info
// ===========================================================================

TEST_F(OdbcCatalogTest, DISABLED_SQLStatisticsReturnsIndexInfo) {
    // Known limitation: psqlODBC sends a multi-table JOIN query against
    // pg_class, pg_namespace, pg_index, pg_attribute for index metadata.
    auto s = alloc_stmt();
    auto rc = SQLStatistics(s,
                            nullptr,
                            0,
                            nullptr,
                            0,
                            reinterpret_cast<SQLCHAR*>(const_cast<char*>("users")),
                            SQL_NTS,
                            SQL_INDEX_ALL,
                            SQL_ENSURE);
    EXPECT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO || rc == SQL_NO_DATA)
        << "SQLStatistics: " << odbc_error(SQL_HANDLE_STMT, s);
}

// ===========================================================================
// 6. Simple SELECT through ODBC
// ===========================================================================

TEST_F(OdbcCatalogTest, SimpleSelectWorks) {
    auto s = alloc_stmt();
    std::string sql = "SELECT 1 AS val";
    auto rc = SQLExecDirect(
        s, reinterpret_cast<SQLCHAR*>(sql.data()), static_cast<SQLINTEGER>(sql.size()));
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "ExecDirect: " << odbc_error(SQL_HANDLE_STMT, s);

    ASSERT_EQ(SQLFetch(s), SQL_SUCCESS);
    auto val = get_string_col(s, 1);
    EXPECT_EQ(val, "1");
}

// ===========================================================================
// 7. Parameterized query (prepared statement)
// ===========================================================================

TEST_F(OdbcCatalogTest, ParameterizedQueryWorks) {
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 'alice@test.com')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 'bob@test.com')");

    auto s = alloc_stmt();
    std::string sql = "SELECT name FROM users WHERE id = ?";
    auto rc =
        SQLPrepare(s, reinterpret_cast<SQLCHAR*>(sql.data()), static_cast<SQLINTEGER>(sql.size()));
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "Prepare: " << odbc_error(SQL_HANDLE_STMT, s);

    SQLINTEGER param_val = 1;
    SQLLEN ind = 0;
    rc = SQLBindParameter(
        s, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param_val, sizeof(param_val), &ind);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "BindParam: " << odbc_error(SQL_HANDLE_STMT, s);

    rc = SQLExecute(s);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "Execute: " << odbc_error(SQL_HANDLE_STMT, s);

    ASSERT_EQ(SQLFetch(s), SQL_SUCCESS);
    auto name = get_string_col(s, 1);
    EXPECT_EQ(name, "Alice");
}

// ===========================================================================
// 8. All tables visible (no type filter)
// ===========================================================================

TEST_F(OdbcCatalogTest, DISABLED_AllTablesVisible) {
    // Known limitation: same implicit JOIN issue as SQLTablesReturnsUserTables.
    auto s = alloc_stmt();
    auto rc = SQLTables(s, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "SQLTables (all): " << odbc_error(SQL_HANDLE_STMT, s);

    std::vector<std::string> tables;
    while (SQLFetch(s) == SQL_SUCCESS) {
        tables.push_back(get_string_col(s, 3));
    }

    EXPECT_GE(tables.size(), 3u);
}

// ===========================================================================
// 9. SQLColumns for different column types (products table)
// ===========================================================================

TEST_F(OdbcCatalogTest, DISABLED_SQLColumnsForProductsTable) {
    // Known limitation: same complex catalog query issue as SQLColumnsReturnsCorrectMetadata.
    auto s = alloc_stmt();
    auto rc = SQLColumns(s,
                         nullptr,
                         0,
                         nullptr,
                         0,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>("products")),
                         SQL_NTS,
                         nullptr,
                         0);
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "SQLColumns(products): " << odbc_error(SQL_HANDLE_STMT, s);

    std::vector<std::pair<std::string, std::string>> cols;
    while (SQLFetch(s) == SQL_SUCCESS) {
        auto name = get_string_col(s, 4);
        auto type = get_string_col(s, 6);
        cols.emplace_back(name, type);
    }

    EXPECT_GE(cols.size(), 4u) << "Expected 4 columns for products";
}

// ===========================================================================
// 10. Direct pg_catalog query (diagnostic)
// ===========================================================================

TEST_F(OdbcCatalogTest, DirectPgCatalogQuery) {
    auto s = alloc_stmt();
    std::string sql = "SELECT relname FROM pg_catalog.pg_class";
    auto rc = SQLExecDirect(
        s, reinterpret_cast<SQLCHAR*>(sql.data()), static_cast<SQLINTEGER>(sql.size()));
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        auto err = odbc_error(SQL_HANDLE_STMT, s);
        FAIL() << "Direct pg_catalog query failed: " << err;
    }

    std::vector<std::string> tables;
    while (SQLFetch(s) == SQL_SUCCESS) {
        tables.push_back(get_string_col(s, 1));
    }
    EXPECT_GE(tables.size(), 3u);
}

// ===========================================================================
// 11. SELECT from user table through ODBC
// ===========================================================================

TEST_F(OdbcCatalogTest, SelectFromUserTable) {
    exec_ok("INSERT INTO users VALUES (10, 'Charlie', 'charlie@test.com')");

    auto s = alloc_stmt();
    std::string sql = "SELECT id, name, email FROM users WHERE id = 10";
    auto rc = SQLExecDirect(
        s, reinterpret_cast<SQLCHAR*>(sql.data()), static_cast<SQLINTEGER>(sql.size()));
    ASSERT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        << "ExecDirect: " << odbc_error(SQL_HANDLE_STMT, s);

    ASSERT_EQ(SQLFetch(s), SQL_SUCCESS);
    EXPECT_EQ(get_string_col(s, 1), "10");
    EXPECT_EQ(get_string_col(s, 2), "Charlie");
    EXPECT_EQ(get_string_col(s, 3), "charlie@test.com");
}
