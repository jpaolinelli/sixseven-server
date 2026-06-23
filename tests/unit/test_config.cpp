#include "sixseven/common/config.h"
#include "sixseven/common/platform.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace sixseven;

TEST(Config, DefaultValues) {
    auto config = Config::load_defaults();
    EXPECT_EQ(config.data_dir, "./data");
    EXPECT_EQ(config.port, 6767);
    EXPECT_EQ(config.log_level, "info");
    EXPECT_EQ(config.buffer_pool_size_mb, 256u);
    EXPECT_EQ(config.wal_segment_size_mb, 16u);
    EXPECT_EQ(config.max_connections, 100u);
}

TEST(Config, MissingFileReturnsDefaults) {
    auto result = Config::load_from_file("/nonexistent/path/config.json");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 6767);
}

class ConfigFileTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        auto tmp_dir = std::filesystem::temp_directory_path();
        auto tmpl = (tmp_dir / "sixseven_test_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = sixseven_platform::mkstemp(buf.data());
        ASSERT_NE(fd, -1) << "mkstemp failed";
        ::close(fd);
        tmp_path_ = std::string(buf.data()) + ".json";
        std::rename(buf.data(), tmp_path_.c_str());
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    void write_file(const std::string& content) {
        std::ofstream f(tmp_path_);
        f << content;
    }
};

TEST_F(ConfigFileTest, LoadFromValidJson) {
    write_file(R"({
        "data_dir": "/var/sixseven",
        "port": 9999,
        "log_level": "debug",
        "buffer_pool_size_mb": 512,
        "wal_segment_size_mb": 32,
        "max_connections": 200
    })");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->data_dir, "/var/sixseven");
    EXPECT_EQ(result->port, 9999);
    EXPECT_EQ(result->log_level, "debug");
    EXPECT_EQ(result->buffer_pool_size_mb, 512u);
    EXPECT_EQ(result->wal_segment_size_mb, 32u);
    EXPECT_EQ(result->max_connections, 200u);
}

TEST_F(ConfigFileTest, LoadFromPartialJson) {
    write_file(R"({"port": 8080, "log_level": "warn"})");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    // Overridden fields
    EXPECT_EQ(result->port, 8080);
    EXPECT_EQ(result->log_level, "warn");
    // Default fields preserved
    EXPECT_EQ(result->data_dir, "./data");
    EXPECT_EQ(result->buffer_pool_size_mb, 256u);
    EXPECT_EQ(result->wal_segment_size_mb, 16u);
    EXPECT_EQ(result->max_connections, 100u);
}

TEST_F(ConfigFileTest, MalformedJsonReturnsError) {
    write_file("{invalid json content");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(ConfigFileTest, PortOutOfRangeReturnsError) {
    write_file(R"({"port": 70000})");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, EmptyJsonObjectUsesDefaults) {
    write_file("{}");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 6767);
    EXPECT_EQ(result->data_dir, "./data");
}

// ---------------------------------------------------------------------------
// GDB-729: apply_setting must return Result<void> and reject bad numeric input
// ---------------------------------------------------------------------------

// Helper: a fresh config so we can check mutations.
static Config make_config() {
    return Config::load_defaults();
}

TEST(Config, ApplySettingValidPort) {
    auto config = make_config();
    auto r = config.apply_setting("server.port", "8080");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.port, 8080);
}

TEST(Config, ApplySettingAlphaPortReturnsError) {
    auto config = make_config();
    auto r = config.apply_setting("server.port", "abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    // Port must be unchanged.
    EXPECT_EQ(config.port, 6767);
}

TEST(Config, ApplySettingEmptyStringReturnsError) {
    auto config = make_config();
    auto r = config.apply_setting("server.port", "");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Config, ApplySettingTrailingJunkRejected) {
    // "123abc" — from_chars consumes "123" but stops at 'a', ptr != end.
    auto config = make_config();
    auto r = config.apply_setting("server.port", "123abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(config.port, 6767);
}

TEST(Config, ApplySettingPortOutOfRangeReturnsError) {
    auto config = make_config();
    auto r = config.apply_setting("server.port", "70000");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(config.port, 6767);
}

TEST(Config, ApplySettingNegativeIntoUnsignedReturnsError) {
    // from_chars on an unsigned type does not accept a leading '-'.
    auto config = make_config();
    auto r = config.apply_setting("server.max_connections", "-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Config, ApplySettingValidI32) {
    auto config = make_config();
    auto r = config.apply_setting("replication.keepalive_interval_ms", "5000");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.replication_keepalive_interval_ms, 5000);
}

TEST(Config, ApplySettingAlphaI32ReturnsError) {
    auto config = make_config();
    auto r = config.apply_setting("replication.keepalive_interval_ms", "abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    // Value must be unchanged.
    EXPECT_EQ(config.replication_keepalive_interval_ms, 10000);
}

TEST(Config, ApplySettingTrailingJunkI32Rejected) {
    auto config = make_config();
    auto r = config.apply_setting("replication.keepalive_interval_ms", "5000ms");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Config, ApplySettingValidI64) {
    auto config = make_config();
    auto r = config.apply_setting("replication.promote_max_lag_bytes", "1048576");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.replication_promote_max_lag_bytes, 1048576);
}

TEST(Config, ApplySettingAlphaI64ReturnsError) {
    auto config = make_config();
    auto r = config.apply_setting("replication.promote_max_lag_bytes", "not_a_number");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Config, ApplySettingStringKeyAcceptsAnyValue) {
    // String keys (e.g. logging.level) must never error regardless of content.
    auto config = make_config();
    auto r = config.apply_setting("logging.level", "trace");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(config.log_level, "trace");
}

TEST(Config, ApplySettingUnknownKeyIgnored) {
    auto config = make_config();
    auto r = config.apply_setting("completely.unknown.key", "anything");
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

TEST(Config, ApplySettingValidValues_AllNumericKeys) {
    // Smoke-test that each numeric key accepts its own default value.
    auto config = make_config();
    struct KV {
        const char* key;
        const char* value;
    };
    KV cases[] = {
        {"server.port", "6767"},
        {"server.max_connections", "100"},
        {"storage.buffer_pool_size_mb", "256"},
        {"storage.wal_segment_size_mb", "16"},
        {"replication.max_wal_senders", "10"},
        {"replication.keepalive_interval_ms", "10000"},
        {"replication.sender_timeout_ms", "60000"},
        {"replication.primary_port", "6767"},
        {"replication.retry_interval_ms", "5000"},
        {"replication.max_retry_interval_ms", "60000"},
        {"replication.synchronous_commit_count", "1"},
        {"replication.synchronous_timeout_ms", "30000"},
        {"replication.promote_max_lag_bytes", "0"},
        {"replication.lag_warning_threshold_bytes", "10000"},
        {"replication.disconnect_warning_threshold_ms", "60000"},
        {"server.shutdown_timeout_s", "30"},
    };
    for (const auto& kv : cases) {
        auto r = config.apply_setting(kv.key, kv.value);
        EXPECT_TRUE(r.has_value())
            << "key=" << kv.key << " value=" << kv.value << " err=" << (r ? "" : r.error().message);
    }
}

// ---------------------------------------------------------------------------
// GDB-729: validate_setting must catch errors without mutating config
// ---------------------------------------------------------------------------

TEST(Config, ValidateSettingAlphaPortFails) {
    auto r = Config::validate_setting("server.port", "abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Config, ValidateSettingValidPortSucceeds) {
    auto r = Config::validate_setting("server.port", "9090");
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

TEST(Config, ValidateSettingUnknownKeySucceeds) {
    auto r = Config::validate_setting("no.such.key", "whatever");
    ASSERT_TRUE(r.has_value());
}

TEST(Config, ValidateSettingStringKeySucceeds) {
    auto r = Config::validate_setting("logging.level", "!!!not_a_log_level");
    // String keys accept any value — no parse check.
    ASSERT_TRUE(r.has_value());
}

// ---------------------------------------------------------------------------
// GDB-853: Five previously-ignored Config fields are now wired in load_from_file,
// and wrong-typed known keys now return INVALID_ARGUMENT instead of silently
// defaulting.
// ---------------------------------------------------------------------------

TEST_F(ConfigFileTest, GDB853_ArchiveEnabledLoaded) {
    write_file(R"({"archive_enabled": true})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->archive_enabled);
}

TEST_F(ConfigFileTest, GDB853_ArchiveEnabledFalseLoaded) {
    write_file(R"({"archive_enabled": false})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->archive_enabled);
}

TEST_F(ConfigFileTest, GDB853_ArchiveCleanupPolicyLoaded) {
    write_file(R"({"archive_cleanup_policy": "delete_old"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->archive_cleanup_policy, "delete_old");
}

TEST_F(ConfigFileTest, GDB853_MaxWalSendersLoaded) {
    write_file(R"({"replication_max_wal_senders": 20})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->replication_max_wal_senders, 20);
}

TEST_F(ConfigFileTest, GDB853_KeepaliveIntervalMsLoaded) {
    write_file(R"({"replication_keepalive_interval_ms": 5000})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->replication_keepalive_interval_ms, 5000);
}

TEST_F(ConfigFileTest, GDB853_SenderTimeoutMsLoaded) {
    write_file(R"({"replication_sender_timeout_ms": 30000})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->replication_sender_timeout_ms, 30000);
}

TEST_F(ConfigFileTest, GDB853_AllFiveMissingFieldsLoadedTogether) {
    write_file(R"({
        "archive_enabled": true,
        "archive_cleanup_policy": "delete_old",
        "replication_max_wal_senders": 20,
        "replication_keepalive_interval_ms": 5000,
        "replication_sender_timeout_ms": 30000
    })");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->archive_enabled);
    EXPECT_EQ(result->archive_cleanup_policy, "delete_old");
    EXPECT_EQ(result->replication_max_wal_senders, 20);
    EXPECT_EQ(result->replication_keepalive_interval_ms, 5000);
    EXPECT_EQ(result->replication_sender_timeout_ms, 30000);
}

TEST_F(ConfigFileTest, GDB853_WrongTypedPortReturnsError) {
    // "port" as a string should now be an error, not a silent default.
    write_file(R"({"port": "6767"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, GDB853_WrongTypedMaxWalSendersReturnsError) {
    // max_wal_senders as a string should error.
    write_file(R"({"replication_max_wal_senders": "x"})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, GDB853_WrongTypedArchiveEnabledReturnsError) {
    // archive_enabled as an integer should error (must be boolean).
    write_file(R"({"archive_enabled": 123})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, GDB853_ReplicationPrimaryPortOutOfRangeReturnsError) {
    // primary_port > 65535 used to be silently dropped; now it must error.
    write_file(R"({"replication": {"primary_port": 70000}})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, GDB853_FullValidConfigLoadsAllFields) {
    // Regression: a complete valid config must still load without error.
    write_file(R"({
        "data_dir": "/var/sixseven",
        "port": 5432,
        "log_level": "warn",
        "buffer_pool_size_mb": 512,
        "wal_segment_size_mb": 32,
        "max_connections": 50,
        "shutdown_timeout_s": 60,
        "auth_method": "trust",
        "archive_enabled": true,
        "archive_cleanup_policy": "delete_old",
        "replication_max_wal_senders": 20,
        "replication_keepalive_interval_ms": 5000,
        "replication_sender_timeout_ms": 30000,
        "server": {"mode": "primary"},
        "replication": {
            "primary_host": "db-primary",
            "primary_port": 5432,
            "retry_interval_ms": 3000,
            "max_retry_interval_ms": 30000,
            "promote_max_lag_bytes": 1048576,
            "synchronous_mode": "remote_write",
            "synchronous_standby_names": "standby1",
            "synchronous_commit_count": 2,
            "synchronous_timeout_ms": 10000,
            "synchronous_fallback": "warn",
            "lag_warning_threshold_bytes": 5000,
            "disconnect_warning_threshold_ms": 30000
        }
    })");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->data_dir, "/var/sixseven");
    EXPECT_EQ(result->port, 5432);
    EXPECT_TRUE(result->archive_enabled);
    EXPECT_EQ(result->archive_cleanup_policy, "delete_old");
    EXPECT_EQ(result->replication_max_wal_senders, 20);
    EXPECT_EQ(result->replication_keepalive_interval_ms, 5000);
    EXPECT_EQ(result->replication_sender_timeout_ms, 30000);
    EXPECT_EQ(result->replication_primary_host, "db-primary");
    EXPECT_EQ(result->replication_primary_port, 5432);
    EXPECT_FALSE(result->standby_mode);
}

TEST_F(ConfigFileTest, GDB853_UnknownExtraKeysAreIgnored) {
    // Extra unknown keys must NOT cause an error (forward-compatibility).
    write_file(R"({"port": 8080, "future_feature_x": "anything", "replication": {"new_key": 42}})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->port, 8080);
}

TEST_F(ConfigFileTest, GDB853_NegativeMaxWalSendersReturnsError) {
    // Negative integer must be rejected (range guard: >= 0 required).
    write_file(R"({"replication_max_wal_senders": -1})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, GDB853_NegativeKeepaliveIntervalMsReturnsError) {
    write_file(R"({"replication_keepalive_interval_ms": -500})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(ConfigFileTest, GDB853_NegativeSenderTimeoutMsReturnsError) {
    write_file(R"({"replication_sender_timeout_ms": -1})");
    auto result = Config::load_from_file(tmp_path_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}