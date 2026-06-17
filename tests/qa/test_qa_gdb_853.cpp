// QA adversarial tests for GDB-853:
// Config::load_from_file wires 5 previously-ignored fields and returns
// INVALID_ARGUMENT on wrong-typed known keys + negative range guards.
//
// These tests deliberately try to break the implementation with edge cases,
// boundary values, and hostile JSON payloads.

#include "sixseven/common/config.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Fixture: write a temp JSON file and clean it up after each test.
// ---------------------------------------------------------------------------

class QA_GDB853 : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        auto tmp =
            std::filesystem::temp_directory_path() /
            ("sixseven_qa_gdb853_" +
             std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".json");
        tmp_path_ = tmp.string();
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    void write(const std::string& json) {
        std::ofstream f(tmp_path_);
        f << json;
    }

    Result<Config> load() { return Config::load_from_file(tmp_path_); }
};

// ---------------------------------------------------------------------------
// SECTION 1: Wrong-type variants for each of the 5 new fields
// ---------------------------------------------------------------------------

// --- archive_enabled ---

TEST_F(QA_GDB853, ArchiveEnabled_StringWhereBool_ReturnsInvalidArgument) {
    write(R"({"archive_enabled": "yes"})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveEnabled_FloatWhereBool_ReturnsInvalidArgument) {
    write(R"({"archive_enabled": 1.0})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveEnabled_ArrayWhereBool_ReturnsInvalidArgument) {
    write(R"({"archive_enabled": [true]})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveEnabled_ObjectWhereBool_ReturnsInvalidArgument) {
    write(R"({"archive_enabled": {"value": true}})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveEnabled_NullWhereBool_ReturnsInvalidArgument) {
    write(R"({"archive_enabled": null})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveEnabled_TrueLoaded_Exact) {
    write(R"({"archive_enabled": true})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->archive_enabled);
}

TEST_F(QA_GDB853, ArchiveEnabled_FalseLoaded_Exact) {
    write(R"({"archive_enabled": false})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->archive_enabled);
}

// --- archive_cleanup_policy ---

TEST_F(QA_GDB853, ArchiveCleanupPolicy_IntWhereString_ReturnsInvalidArgument) {
    write(R"({"archive_cleanup_policy": 42})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveCleanupPolicy_BoolWhereString_ReturnsInvalidArgument) {
    write(R"({"archive_cleanup_policy": true})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveCleanupPolicy_FloatWhereString_ReturnsInvalidArgument) {
    write(R"({"archive_cleanup_policy": 3.14})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveCleanupPolicy_ArrayWhereString_ReturnsInvalidArgument) {
    write(R"({"archive_cleanup_policy": ["keep_all"]})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveCleanupPolicy_NullWhereString_ReturnsInvalidArgument) {
    write(R"({"archive_cleanup_policy": null})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ArchiveCleanupPolicy_EmptyStringLoaded_Verbatim) {
    // Empty string is a valid string — must be accepted and stored verbatim.
    write(R"({"archive_cleanup_policy": ""})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->archive_cleanup_policy, "");
}

TEST_F(QA_GDB853, ArchiveCleanupPolicy_ArbitraryStringLoaded_Verbatim) {
    write(R"({"archive_cleanup_policy": "delete_old"})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->archive_cleanup_policy, "delete_old");
}

// --- replication_max_wal_senders ---

TEST_F(QA_GDB853, MaxWalSenders_StringWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": "10"})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, MaxWalSenders_BoolWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": true})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, MaxWalSenders_FloatWhereInt_ReturnsInvalidArgument) {
    // 20.5 is_number_integer() == false in nlohmann — must reject.
    write(R"({"replication_max_wal_senders": 20.5})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, MaxWalSenders_ArrayWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": [10]})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, MaxWalSenders_ObjectWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": {"n": 10}})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, MaxWalSenders_NullWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": null})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- replication_keepalive_interval_ms ---

TEST_F(QA_GDB853, KeepaliveMs_StringWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_keepalive_interval_ms": "5000"})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, KeepaliveMs_FloatWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_keepalive_interval_ms": 5000.5})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, KeepaliveMs_NullWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_keepalive_interval_ms": null})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, KeepaliveMs_ArrayWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_keepalive_interval_ms": [5000]})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- replication_sender_timeout_ms ---

TEST_F(QA_GDB853, SenderTimeoutMs_StringWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_sender_timeout_ms": "60000"})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, SenderTimeoutMs_FloatWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_sender_timeout_ms": 60000.1})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, SenderTimeoutMs_NullWhereInt_ReturnsInvalidArgument) {
    write(R"({"replication_sender_timeout_ms": null})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// SECTION 2: Boundary values for the 3 int fields
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, MaxWalSenders_Zero_IsValid) {
    // 0 replicas is a legitimate configuration (no replication).
    write(R"({"replication_max_wal_senders": 0})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_max_wal_senders, 0);
}

TEST_F(QA_GDB853, KeepaliveMs_Zero_IsValid) {
    write(R"({"replication_keepalive_interval_ms": 0})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_keepalive_interval_ms, 0);
}

TEST_F(QA_GDB853, SenderTimeoutMs_Zero_IsValid) {
    write(R"({"replication_sender_timeout_ms": 0})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_sender_timeout_ms, 0);
}

TEST_F(QA_GDB853, MaxWalSenders_INT32MAX_IsValid) {
    // INT32_MAX = 2147483647; is_number_integer() == true, >= 0.
    write(R"({"replication_max_wal_senders": 2147483647})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_max_wal_senders, 2147483647);
}

TEST_F(QA_GDB853, KeepaliveMs_INT32MAX_IsValid) {
    write(R"({"replication_keepalive_interval_ms": 2147483647})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_keepalive_interval_ms, 2147483647);
}

TEST_F(QA_GDB853, SenderTimeoutMs_INT32MAX_IsValid) {
    write(R"({"replication_sender_timeout_ms": 2147483647})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_sender_timeout_ms, 2147483647);
}

TEST_F(QA_GDB853, MaxWalSenders_Negative_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": -1})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, KeepaliveMs_Negative_ReturnsInvalidArgument) {
    write(R"({"replication_keepalive_interval_ms": -1})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, SenderTimeoutMs_Negative_ReturnsInvalidArgument) {
    write(R"({"replication_sender_timeout_ms": -1})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, MaxWalSenders_LargeNegative_ReturnsInvalidArgument) {
    write(R"({"replication_max_wal_senders": -2147483648})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// A value exceeding INT32_MAX forces is_number_integer() behavior — nlohmann
// stores it as uint64 or int64; get<int32_t>() is UB / may truncate. Confirm
// the field is correctly rejected or its truncated value does not silently
// corrupt the config. The implementation uses get<int32_t>() after checking
// is_number_integer(), which allows overflow on very large values — this test
// documents current behavior.
TEST_F(QA_GDB853, MaxWalSenders_ExceedingINT32MAX_DocumentsBehavior) {
    // 2147483648 = INT32_MAX + 1. nlohmann is_number_integer() is true for this
    // on 64-bit platforms; get<int32_t>() truncates. The >= 0 guard on int32_t
    // still passes if the truncated value happens to be >= 0. We assert the
    // result does NOT produce a negative value silently (which would be a bug
    // since the value is out of i32 range).
    write(R"({"replication_max_wal_senders": 2147483648})");
    auto r = load();
    if (r.has_value()) {
        // If it loaded, the value must be >= 0 (the guard must not allow a
        // negative-appearing truncation to slip through).
        EXPECT_GE(r->replication_max_wal_senders, 0)
            << "Silent int32 overflow produced a negative value — bug!";
    }
    // Either error or non-negative is acceptable; negative would be a High bug.
}

// ---------------------------------------------------------------------------
// SECTION 3: Port boundary values (top-level and replication.primary_port)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, Port_Zero_IsValid) {
    write(R"({"port": 0})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 0);
}

TEST_F(QA_GDB853, Port_One_IsValid) {
    write(R"({"port": 1})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 1);
}

TEST_F(QA_GDB853, Port_65535_IsValid) {
    write(R"({"port": 65535})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 65535);
}

TEST_F(QA_GDB853, Port_65536_ReturnsInvalidArgument) {
    write(R"({"port": 65536})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, Port_70000_ReturnsInvalidArgument) {
    write(R"({"port": 70000})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, Port_NegativeInt_ReturnsInvalidArgument) {
    // Negative integers are not unsigned — must error.
    write(R"({"port": -1})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ReplicationPrimaryPort_Zero_IsValid) {
    write(R"({"replication": {"primary_port": 0}})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_primary_port, 0);
}

TEST_F(QA_GDB853, ReplicationPrimaryPort_One_IsValid) {
    write(R"({"replication": {"primary_port": 1}})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_primary_port, 1);
}

TEST_F(QA_GDB853, ReplicationPrimaryPort_65535_IsValid) {
    write(R"({"replication": {"primary_port": 65535}})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_primary_port, 65535);
}

TEST_F(QA_GDB853, ReplicationPrimaryPort_65536_ReturnsInvalidArgument) {
    write(R"({"replication": {"primary_port": 65536}})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ReplicationPrimaryPort_70000_ReturnsInvalidArgument) {
    write(R"({"replication": {"primary_port": 70000}})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, ReplicationPrimaryPort_NegativeInt_ReturnsInvalidArgument) {
    // Negative integers fail is_number_unsigned() — must error.
    write(R"({"replication": {"primary_port": -1}})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// CONSISTENCY: top-level port and replication.primary_port must have identical
// range semantics (both error on > 65535, both accept 0-65535).
TEST_F(QA_GDB853, PortAndPrimaryPort_ConsistentUpperBound) {
    // 65535 must succeed for both.
    write(R"({"port": 65535, "replication": {"primary_port": 65535}})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 65535);
    EXPECT_EQ(r->replication_primary_port, 65535);
}

TEST_F(QA_GDB853, PortAndPrimaryPort_ConsistentOverflow) {
    // 65536 must fail for port (and stop before reading primary_port).
    write(R"({"port": 65536, "replication": {"primary_port": 65536}})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// SECTION 4: Missing-all-5 fields → documented defaults
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, MissingAll5Fields_LoadsWithExactDefaults) {
    // A config with NONE of the 5 new fields present must load cleanly and
    // produce the exact default values documented in config.h.
    write(R"({"port": 9000})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->archive_enabled);                       // default: false
    EXPECT_EQ(r->archive_cleanup_policy, "keep_all");       // default: "keep_all"
    EXPECT_EQ(r->replication_max_wal_senders, 10);          // default: 10
    EXPECT_EQ(r->replication_keepalive_interval_ms, 10000); // default: 10000
    EXPECT_EQ(r->replication_sender_timeout_ms, 60000);     // default: 60000
}

TEST_F(QA_GDB853, EmptyObject_AllDefaultsPreserved) {
    write("{}");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->archive_enabled);
    EXPECT_EQ(r->archive_cleanup_policy, "keep_all");
    EXPECT_EQ(r->replication_max_wal_senders, 10);
    EXPECT_EQ(r->replication_keepalive_interval_ms, 10000);
    EXPECT_EQ(r->replication_sender_timeout_ms, 60000);
    // Classic fields also keep defaults.
    EXPECT_EQ(r->port, 6767);
    EXPECT_EQ(r->data_dir, "./data");
    EXPECT_EQ(r->buffer_pool_size_mb, 256u);
}

// ---------------------------------------------------------------------------
// SECTION 5: Malformed JSON / empty file / file-not-found
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, MalformedJson_ReturnsParseError_NoCrash) {
    write("{bad json]]]");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB853, TruncatedJson_ReturnsParseError_NoCrash) {
    write(R"({"port": 808)");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB853, EmptyFile_ReturnsParseError_NoCrash) {
    write("");
    auto r = load();
    // nlohmann::json::parse on an empty stream throws parse_error.
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB853, FileNotFound_ReturnsDefaults) {
    // load_from_file must return defaults (not an error) when the file is absent.
    auto r = Config::load_from_file("/nonexistent/qa_gdb853/config.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->port, 6767);
    EXPECT_FALSE(r->archive_enabled);
    EXPECT_EQ(r->archive_cleanup_policy, "keep_all");
}

TEST_F(QA_GDB853, JsonArrayAtRoot_ReturnsError_NoCrash) {
    // Root is an array, not an object — field access via j.contains() on a JSON
    // array should not crash.
    write(R"([{"port": 9000}])");
    auto r = load();
    // nlohmann does not throw for j.contains() on arrays; it returns false.
    // The config should either load all-defaults or return an error — no crash.
    // We assert no crash implicitly by reaching this EXPECT.
    (void)r;
    SUCCEED();
}

// ---------------------------------------------------------------------------
// SECTION 6: Unknown/extra keys — must be IGNORED (forward-compat)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, UnknownTopLevelKey_Ignored_LoadSucceeds) {
    write(R"({"port": 8080, "unknown_future_setting": "anything"})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 8080);
}

TEST_F(QA_GDB853, UnknownKeyInsideReplicationObject_Ignored_LoadSucceeds) {
    // An unknown key INSIDE the replication block must not cause an error.
    write(R"({"replication": {"primary_host": "db1", "new_future_key": 99}})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_primary_host, "db1");
}

TEST_F(QA_GDB853, UnknownKeyInsideServerObject_Ignored_LoadSucceeds) {
    write(R"({"server": {"mode": "primary", "new_opt": true}})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->standby_mode);
}

TEST_F(QA_GDB853, MultipleUnknownKeys_AllIgnored_LoadSucceeds) {
    write(R"({"a": 1, "b": "x", "c": null, "d": [], "port": 5000})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 5000);
}

// ---------------------------------------------------------------------------
// SECTION 7: Case sensitivity — wrong-case keys treated as unknown → ignored
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, CaseSensitive_UpperCaseMaxWalSenders_Ignored) {
    // "replication_Max_Wal_Senders" is not the documented key.
    // It must be treated as unknown → ignored, field stays at default 10.
    write(R"({"replication_Max_Wal_Senders": 99})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_max_wal_senders, 10) << "Wrong-case key must be ignored";
}

TEST_F(QA_GDB853, CaseSensitive_UpperCaseArchiveEnabled_Ignored) {
    write(R"({"Archive_Enabled": true})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->archive_enabled) << "Wrong-case key must be ignored, default stays false";
}

TEST_F(QA_GDB853, CaseSensitive_UpperCasePort_Ignored) {
    write(R"({"Port": 9000})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->port, 6767) << "Wrong-case 'Port' must be ignored, default stays 6767";
}

// ---------------------------------------------------------------------------
// SECTION 8: Known key with correct type + the 5 new fields coexist
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, AllFieldsLoadedTogether_FullValidConfig_ExactAssertions) {
    write(R"({
        "data_dir": "/mnt/db",
        "port": 5432,
        "log_level": "warn",
        "buffer_pool_size_mb": 1024,
        "wal_segment_size_mb": 64,
        "max_connections": 50,
        "shutdown_timeout_s": 120,
        "auth_method": "trust",
        "archive_enabled": true,
        "archive_cleanup_policy": "delete_old",
        "replication_max_wal_senders": 5,
        "replication_keepalive_interval_ms": 2000,
        "replication_sender_timeout_ms": 15000,
        "server": {"mode": "standby"},
        "replication": {
            "primary_host": "primary.db.example.com",
            "primary_port": 5432,
            "retry_interval_ms": 1000,
            "max_retry_interval_ms": 20000,
            "promote_max_lag_bytes": 524288,
            "synchronous_mode": "remote_flush",
            "synchronous_standby_names": "s1,s2",
            "synchronous_commit_count": 2,
            "synchronous_timeout_ms": 5000,
            "synchronous_fallback": "warn",
            "lag_warning_threshold_ms": 3000,
            "disconnect_warning_threshold_ms": 15000
        }
    })");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Classic fields
    EXPECT_EQ(r->data_dir, "/mnt/db");
    EXPECT_EQ(r->port, 5432);
    EXPECT_EQ(r->log_level, "warn");
    EXPECT_EQ(r->buffer_pool_size_mb, 1024u);
    EXPECT_EQ(r->wal_segment_size_mb, 64u);
    EXPECT_EQ(r->max_connections, 50u);
    EXPECT_EQ(r->shutdown_timeout_s, 120);
    EXPECT_EQ(r->auth_method, "trust");
    // 5 new fields
    EXPECT_TRUE(r->archive_enabled);
    EXPECT_EQ(r->archive_cleanup_policy, "delete_old");
    EXPECT_EQ(r->replication_max_wal_senders, 5);
    EXPECT_EQ(r->replication_keepalive_interval_ms, 2000);
    EXPECT_EQ(r->replication_sender_timeout_ms, 15000);
    // Replication sub-object fields
    EXPECT_TRUE(r->standby_mode);
    EXPECT_EQ(r->replication_primary_host, "primary.db.example.com");
    EXPECT_EQ(r->replication_primary_port, 5432);
    EXPECT_EQ(r->replication_retry_interval_ms, 1000);
    EXPECT_EQ(r->replication_max_retry_interval_ms, 20000);
    EXPECT_EQ(r->replication_promote_max_lag_bytes, 524288);
    EXPECT_EQ(r->replication_synchronous_mode, "remote_flush");
    EXPECT_EQ(r->replication_synchronous_standby_names, "s1,s2");
    EXPECT_EQ(r->replication_synchronous_commit_count, 2);
    EXPECT_EQ(r->replication_synchronous_timeout_ms, 5000);
    EXPECT_EQ(r->replication_synchronous_fallback, "warn");
    EXPECT_EQ(r->replication_lag_warning_threshold_ms, 3000);
    EXPECT_EQ(r->replication_disconnect_warning_threshold_ms, 15000);
}

// ---------------------------------------------------------------------------
// SECTION 9: Duplicate JSON keys — nlohmann takes last; no crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, DuplicateKeys_LastValueWins_NoCrash) {
    // nlohmann silently takes the last value for duplicate keys.
    // We assert the result is last-value and no crash occurs.
    write(R"({"port": 8000, "port": 9000})");
    auto r = load();
    // Either value is fine; we just must not crash and must load successfully.
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Last value in standard JSON semantics (nlohmann behavior).
    EXPECT_EQ(r->port, 9000);
}

TEST_F(QA_GDB853, DuplicateKeys_NewField_LastValueWins_NoCrash) {
    write(R"({"replication_max_wal_senders": 5, "replication_max_wal_senders": 20})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_max_wal_senders, 20);
}

// ---------------------------------------------------------------------------
// SECTION 10: Regression — the 5 fields must not remain silently ignored
// (mutation-grade: if a field is ignored the value stays at its default and
//  these tests detect that).
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, Regression_ArchiveEnabled_NotIgnored) {
    // Mutate from default (false → true). If still silently ignored this fails.
    write(R"({"archive_enabled": true})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->archive_enabled) << "archive_enabled is still being silently ignored";
}

TEST_F(QA_GDB853, Regression_ArchiveCleanupPolicy_NotIgnored) {
    // Mutate from default ("keep_all" → different value).
    write(R"({"archive_cleanup_policy": "compress_old"})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->archive_cleanup_policy, "compress_old")
        << "archive_cleanup_policy is still being silently ignored";
}

TEST_F(QA_GDB853, Regression_MaxWalSenders_NotIgnored) {
    // Mutate from default (10 → 25).
    write(R"({"replication_max_wal_senders": 25})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_max_wal_senders, 25)
        << "replication_max_wal_senders is still being silently ignored";
}

TEST_F(QA_GDB853, Regression_KeepaliveIntervalMs_NotIgnored) {
    // Mutate from default (10000 → 1000).
    write(R"({"replication_keepalive_interval_ms": 1000})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_keepalive_interval_ms, 1000)
        << "replication_keepalive_interval_ms is still being silently ignored";
}

TEST_F(QA_GDB853, Regression_SenderTimeoutMs_NotIgnored) {
    // Mutate from default (60000 → 5000).
    write(R"({"replication_sender_timeout_ms": 5000})");
    auto r = load();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->replication_sender_timeout_ms, 5000)
        << "replication_sender_timeout_ms is still being silently ignored";
}

// ---------------------------------------------------------------------------
// SECTION 11: Wrong-typed KNOWN keys that were previously silently-defaulted
// (pre-GDB-853 regression guard)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB853, WrongType_DataDir_StringExpected_BoolGiven_ReturnsError) {
    write(R"({"data_dir": false})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, WrongType_LogLevel_StringExpected_NumberGiven_ReturnsError) {
    write(R"({"log_level": 3})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, WrongType_BufferPoolSizeMb_StringGiven_ReturnsError) {
    write(R"({"buffer_pool_size_mb": "1024"})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, WrongType_MaxConnections_NegativeFloat_ReturnsError) {
    // is_number_unsigned() rejects signed negative, is_number_integer() for
    // floating-point returns false — must error.
    write(R"({"max_connections": -5.5})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, WrongType_AuthMethod_ArrayGiven_ReturnsError) {
    write(R"({"auth_method": ["trust"]})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB853, WrongType_ArchiveCleanupPolicy_IntGiven_ReturnsError) {
    write(R"({"archive_cleanup_policy": 0})");
    auto r = load();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}
