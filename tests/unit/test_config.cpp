#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "giodb/common/config.h"

using namespace giodb;

TEST(Config, DefaultValues) {
    auto config = Config::load_defaults();
    EXPECT_EQ(config.data_dir, "./data");
    EXPECT_EQ(config.port, 5432);
    EXPECT_EQ(config.log_level, "info");
    EXPECT_EQ(config.buffer_pool_size_mb, 256u);
    EXPECT_EQ(config.wal_segment_size_mb, 16u);
    EXPECT_EQ(config.max_connections, 100u);
}

TEST(Config, MissingFileReturnsDefaults) {
    auto result = Config::load_from_file("/nonexistent/path/config.json");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 5432);
}

class ConfigFileTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        auto tmp_dir = std::filesystem::temp_directory_path();
        tmp_path_ = (tmp_dir / "giodb_test_XXXXXX.json").string();
    }

    void TearDown() override {
        std::remove(tmp_path_.c_str());
    }

    void write_file(const std::string& content) {
        std::ofstream f(tmp_path_);
        f << content;
    }
};

TEST_F(ConfigFileTest, LoadFromValidJson) {
    write_file(R"({
        "data_dir": "/var/giodb",
        "port": 9999,
        "log_level": "debug",
        "buffer_pool_size_mb": 512,
        "wal_segment_size_mb": 32,
        "max_connections": 200
    })");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->data_dir, "/var/giodb");
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

TEST_F(ConfigFileTest, EmptyJsonObjectUsesDefaults) {
    write_file("{}");

    auto result = Config::load_from_file(tmp_path_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->port, 5432);
    EXPECT_EQ(result->data_dir, "./data");
}
