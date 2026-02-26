/// @file test_qa_gdb_10.cpp
/// @brief Adversarial QA tests for GDB-10: Repository & Build System Setup
///
/// Tests cover: logging edge cases, Result/Error edge cases, Config boundary
/// values, apply_setting adversarial inputs, and StatusCode completeness.

#include "giodb/common/config.h"
#include "giodb/common/logging.h"
#include "giodb/common/result.h"
#include "giodb/common/status.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace giodb;

// ============================================================================
// QA_Logging — adversarial logging tests
// ============================================================================

TEST(QA_Logging, InitWithInvalidLevel) {
    // Invalid level string should not crash — spdlog uses "off" as fallback.
    EXPECT_NO_THROW(giodb::init_logging("nonexistent_level"));
}

TEST(QA_Logging, InitWithEmptyString) {
    EXPECT_NO_THROW(giodb::init_logging(""));
}

TEST(QA_Logging, InitCalledMultipleTimes) {
    // Re-initializing logging should not crash or leak.
    for (int i = 0; i < 100; ++i) {
        EXPECT_NO_THROW(giodb::init_logging("info"));
    }
}

TEST(QA_Logging, AllValidLevels) {
    std::vector<std::string> levels = {"trace", "debug", "info", "warn", "error", "critical", "off"};
    for (const auto& level : levels) {
        EXPECT_NO_THROW(giodb::init_logging(level));
    }
}

TEST(QA_Logging, LogWithFormatArgs) {
    giodb::init_logging("trace");
    EXPECT_NO_THROW({
        GIODB_LOG_INFO("int: {}", 42);
        GIODB_LOG_INFO("string: {}", std::string("hello"));
        GIODB_LOG_INFO("double: {:.2f}", 3.14159);
    });
}

// ============================================================================
// QA_Result — adversarial Result<T> and Error tests
// ============================================================================

TEST(QA_Result, VoidResultSuccess) {
    Result<void> r = ok();
    EXPECT_TRUE(r.has_value());
}

TEST(QA_Result, VoidResultError) {
    Result<void> r = make_error(StatusCode::INTERNAL_ERROR, "void error");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INTERNAL_ERROR);
    EXPECT_EQ(r.error().message, "void error");
}

TEST(QA_Result, ErrorWithEmptyMessage) {
    Result<int> r = make_error(StatusCode::NOT_FOUND, "");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "");
}

TEST(QA_Result, ErrorWithVeryLongMessage) {
    std::string long_msg(10000, 'x');
    Result<int> r = make_error(StatusCode::INTERNAL_ERROR, long_msg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message.size(), 10000u);
}

TEST(QA_Result, MoveSemantics) {
    Result<std::string> r1 = ok(std::string("original"));
    Result<std::string> r2 = std::move(r1);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, "original");
}

TEST(QA_Result, OkWithZero) {
    Result<int> r = ok(0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);
}

TEST(QA_Result, OkWithNegative) {
    Result<int> r = ok(-1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, -1);
}

TEST(QA_Result, OkWithMaxInt) {
    Result<int> r = ok(INT_MAX);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT_MAX);
}

TEST(QA_Result, OkWithMinInt) {
    Result<int> r = ok(INT_MIN);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT_MIN);
}

TEST(QA_Result, OkWithEmptyString) {
    Result<std::string> r = ok(std::string(""));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
}

TEST(QA_Result, SourceLocationCapture) {
    auto err = make_error(StatusCode::IO_ERROR, "test");
    // The source_location should be captured at the call site.
    EXPECT_GT(err.value().location.line(), 0u);
    std::string file = err.value().location.file_name();
    EXPECT_FALSE(file.empty());
}

// ============================================================================
// QA_StatusCode — adversarial StatusCode tests
// ============================================================================

TEST(QA_StatusCode, AllCodesHaveNames) {
    // Every StatusCode should return a non-null, non-"UNKNOWN" name.
    std::vector<StatusCode> all_codes = {
        StatusCode::OK,
        StatusCode::NOT_FOUND,
        StatusCode::ALREADY_EXISTS,
        StatusCode::INVALID_ARGUMENT,
        StatusCode::INTERNAL_ERROR,
        StatusCode::NOT_IMPLEMENTED,
        StatusCode::IO_ERROR,
        StatusCode::PARSE_ERROR,
        StatusCode::TYPE_ERROR,
        StatusCode::CONSTRAINT_VIOLATION,
        StatusCode::TXN_CONFLICT,
        StatusCode::TXN_ABORTED,
        StatusCode::NETWORK_ERROR,
        StatusCode::AUTH_ERROR,
        StatusCode::REPLICATION_ERROR,
        StatusCode::READ_ONLY,
        StatusCode::LOCK_TIMEOUT,
        StatusCode::DEADLOCK,
    };
    for (auto code : all_codes) {
        const char* name = status_code_name(code);
        EXPECT_NE(name, nullptr);
        EXPECT_STRNE(name, "UNKNOWN") << "StatusCode " << static_cast<int>(code) << " returns UNKNOWN";
    }
}

TEST(QA_StatusCode, EveryCodeUsableInError) {
    // Every StatusCode should be constructible as an Error.
    std::vector<StatusCode> codes = {
        StatusCode::OK,           StatusCode::NOT_FOUND,   StatusCode::ALREADY_EXISTS,
        StatusCode::INTERNAL_ERROR, StatusCode::IO_ERROR,    StatusCode::PARSE_ERROR,
        StatusCode::TYPE_ERROR,   StatusCode::TXN_CONFLICT, StatusCode::TXN_ABORTED,
        StatusCode::NETWORK_ERROR, StatusCode::AUTH_ERROR,   StatusCode::REPLICATION_ERROR,
        StatusCode::READ_ONLY,    StatusCode::LOCK_TIMEOUT, StatusCode::DEADLOCK,
    };
    for (auto code : codes) {
        Result<int> r = make_error(code, "test");
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, code);
    }
}

// ============================================================================
// QA_Config — adversarial Config tests
// ============================================================================

class QA_ConfigFile : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        auto tmp_dir = std::filesystem::temp_directory_path();
        auto tmpl = (tmp_dir / "giodb_qa_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        ASSERT_NE(fd, -1) << "mkstemp failed";
        close(fd);
        tmp_path_ = std::string(buf.data()) + ".json";
        std::rename(buf.data(), tmp_path_.c_str());
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    void write_file(const std::string& content) {
        std::ofstream f(tmp_path_);
        f << content;
    }
};

TEST_F(QA_ConfigFile, PortZero) {
    write_file(R"({"port": 0})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 0);
}

TEST_F(QA_ConfigFile, PortMaxValid) {
    write_file(R"({"port": 65535})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 65535);
}

TEST_F(QA_ConfigFile, PortExactly65536) {
    write_file(R"({"port": 65536})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_ConfigFile, PortNegative) {
    // Negative port should be silently ignored (not is_number_unsigned).
    write_file(R"({"port": -1})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    // Port should remain at default since -1 is not unsigned.
    EXPECT_EQ(result->port, 6767);
}

TEST_F(QA_ConfigFile, PortAsString) {
    // String port should be silently ignored (type mismatch).
    write_file(R"({"port": "8080"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 6767);
}

TEST_F(QA_ConfigFile, PortAsFloat) {
    // Float port should be silently ignored (not is_number_unsigned for float).
    write_file(R"({"port": 8080.5})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    // Float is not unsigned, so should use default.
    EXPECT_EQ(result->port, 6767);
}

TEST_F(QA_ConfigFile, BufferPoolSizeZero) {
    write_file(R"({"buffer_pool_size_mb": 0})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->buffer_pool_size_mb, 0u);
}

TEST_F(QA_ConfigFile, MaxConnectionsZero) {
    write_file(R"({"max_connections": 0})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->max_connections, 0u);
}

TEST_F(QA_ConfigFile, EmptyDataDir) {
    write_file(R"({"data_dir": ""})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->data_dir, "");
}

TEST_F(QA_ConfigFile, DataDirWithSpecialChars) {
    write_file(R"({"data_dir": "/tmp/giodb test dir/data"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->data_dir, "/tmp/giodb test dir/data");
}

TEST_F(QA_ConfigFile, UnknownFieldsIgnored) {
    write_file(R"({"unknown_field": 42, "another_unknown": "test", "port": 9999})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 9999);
}

TEST_F(QA_ConfigFile, JsonArray) {
    // A JSON array (instead of object) should not crash.
    write_file(R"([1, 2, 3])");
    auto result = Config::load_from_file(tmp_path_);
    // This is valid JSON but semantically wrong — should either succeed with
    // defaults (fields don't match) or fail gracefully.
    if (result.has_value()) {
        // If it defaults, port should be default.
        EXPECT_EQ(result->port, 6767);
    }
    // Either way, no crash is the main assertion.
}

TEST_F(QA_ConfigFile, EmptyFile) {
    write_file("");
    auto result = Config::load_from_file(tmp_path_);
    // Empty file is not valid JSON — should return PARSE_ERROR.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_ConfigFile, WhitespaceOnlyFile) {
    write_file("   \n\t  ");
    auto result = Config::load_from_file(tmp_path_);
    // Whitespace-only is not valid JSON.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_ConfigFile, VeryLargeJson) {
    // Generate a large but valid JSON with many unknown fields.
    std::string json = "{";
    for (int i = 0; i < 1000; ++i) {
        if (i > 0) json += ",";
        json += "\"field_" + std::to_string(i) + "\": " + std::to_string(i);
    }
    json += ", \"port\": 1234}";
    write_file(json);
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 1234);
}

TEST_F(QA_ConfigFile, MasterKeyPathDefaultDerived) {
    write_file(R"({"data_dir": "/custom/path"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->master_key_path, "/custom/path/master.key");
}

TEST_F(QA_ConfigFile, MasterKeyPathExplicit) {
    write_file(R"({"master_key_path": "/explicit/key.pem"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->master_key_path, "/explicit/key.pem");
}

TEST_F(QA_ConfigFile, StandbyModeFromServerObject) {
    write_file(R"({"server": {"mode": "standby"}})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->standby_mode);
}

TEST_F(QA_ConfigFile, PrimaryModeFromServerObject) {
    write_file(R"({"server": {"mode": "primary"}})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->standby_mode);
}

TEST_F(QA_ConfigFile, ReplicationSettings) {
    write_file(R"({
        "replication": {
            "primary_host": "10.0.0.1",
            "primary_port": 6767,
            "retry_interval_ms": 2000,
            "max_retry_interval_ms": 30000
        }
    })");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->replication_primary_host, "10.0.0.1");
    EXPECT_EQ(result->replication_primary_port, 6767);
    EXPECT_EQ(result->replication_retry_interval_ms, 2000);
    EXPECT_EQ(result->replication_max_retry_interval_ms, 30000);
}

// ============================================================================
// QA_ApplySetting — adversarial apply_setting tests
// ============================================================================

TEST(QA_ApplySetting, ValidPortSetting) {
    Config config = Config::load_defaults();
    config.apply_setting("server.port", "9999");
    EXPECT_EQ(config.port, 9999);
}

TEST(QA_ApplySetting, PortOverflow) {
    Config config = Config::load_defaults();
    // Port > 65535 should be silently ignored.
    config.apply_setting("server.port", "70000");
    EXPECT_EQ(config.port, 6767); // unchanged
}

TEST(QA_ApplySetting, UnknownKeyIgnored) {
    Config config = Config::load_defaults();
    EXPECT_NO_THROW(config.apply_setting("totally.unknown.key", "value"));
}

TEST(QA_ApplySetting, EmptyValue) {
    Config config = Config::load_defaults();
    // Applying an empty value for a string setting.
    config.apply_setting("storage.data_dir", "");
    EXPECT_EQ(config.data_dir, "");
}

TEST(QA_ApplySetting, BoolSettingVariants) {
    Config config = Config::load_defaults();
    config.apply_setting("replication.archive_enabled", "TRUE");
    EXPECT_TRUE(config.archive_enabled);

    config.apply_setting("replication.archive_enabled", "true");
    EXPECT_TRUE(config.archive_enabled);

    config.apply_setting("replication.archive_enabled", "1");
    EXPECT_TRUE(config.archive_enabled);

    config.apply_setting("replication.archive_enabled", "false");
    EXPECT_FALSE(config.archive_enabled);

    config.apply_setting("replication.archive_enabled", "0");
    EXPECT_FALSE(config.archive_enabled);
}

TEST(QA_ApplySetting, StandbyModeSetting) {
    Config config = Config::load_defaults();
    EXPECT_FALSE(config.standby_mode);
    config.apply_setting("server.mode", "standby");
    EXPECT_TRUE(config.standby_mode);
    config.apply_setting("server.mode", "primary");
    EXPECT_FALSE(config.standby_mode);
}

TEST(QA_ApplySetting, AllStringSettings) {
    Config config = Config::load_defaults();
    config.apply_setting("logging.level", "debug");
    EXPECT_EQ(config.log_level, "debug");

    config.apply_setting("replication.synchronous_mode", "remote_write");
    EXPECT_EQ(config.replication_synchronous_mode, "remote_write");

    config.apply_setting("replication.synchronous_fallback", "warn");
    EXPECT_EQ(config.replication_synchronous_fallback, "warn");

    config.apply_setting("server.auth_method", "scram-sha-256");
    EXPECT_EQ(config.auth_method, "scram-sha-256");
}

TEST(QA_ApplySetting, NumericSettings) {
    Config config = Config::load_defaults();
    config.apply_setting("server.max_connections", "500");
    EXPECT_EQ(config.max_connections, 500u);

    config.apply_setting("storage.buffer_pool_size_mb", "1024");
    EXPECT_EQ(config.buffer_pool_size_mb, 1024u);

    config.apply_setting("storage.wal_segment_size_mb", "64");
    EXPECT_EQ(config.wal_segment_size_mb, 64u);

    config.apply_setting("server.shutdown_timeout_s", "60");
    EXPECT_EQ(config.shutdown_timeout_s, 60);
}

// ============================================================================
// QA_Defaults — verify Config defaults match acceptance criteria
// ============================================================================

TEST(QA_Defaults, AllDefaultsCorrect) {
    Config config = Config::load_defaults();
    EXPECT_EQ(config.data_dir, "./data");
    EXPECT_EQ(config.port, 6767);
    EXPECT_EQ(config.log_level, "info");
    EXPECT_EQ(config.buffer_pool_size_mb, 256u);
    EXPECT_EQ(config.wal_segment_size_mb, 16u);
    EXPECT_EQ(config.max_connections, 100u);
    EXPECT_EQ(config.master_key_path, "./data/master.key");
    EXPECT_FALSE(config.archive_enabled);
    EXPECT_EQ(config.archive_cleanup_policy, "keep_all");
    EXPECT_FALSE(config.standby_mode);
    EXPECT_EQ(config.auth_method, "trust");
    EXPECT_EQ(config.shutdown_timeout_s, 30);
}

// ============================================================================
// QA_BuildSystem — verify build system acceptance criteria
// ============================================================================

TEST(QA_BuildSystem, UnitTestTargetRuns) {
    // If this test runs, the unit test target was built successfully.
    EXPECT_TRUE(true);
}
