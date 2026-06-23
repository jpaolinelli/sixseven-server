/// Unit tests for CleanShutdownMarker (GDB-900).

#include "sixseven/storage/clean_shutdown_marker.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace sixseven;
namespace fs = std::filesystem;

class CleanShutdownMarkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "sixseven_test_clean_shutdown_marker";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }

    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
};

TEST_F(CleanShutdownMarkerTest, AbsentOnFreshDirectory) {
    CleanShutdownMarker marker(dir_);
    EXPECT_FALSE(marker.exists());
}

TEST_F(CleanShutdownMarkerTest, WriteCreatesFile) {
    CleanShutdownMarker marker(dir_);
    auto result = marker.write();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(marker.exists());
    EXPECT_TRUE(fs::exists(dir_ / "clean_shutdown"));
}

TEST_F(CleanShutdownMarkerTest, RemoveDeletesFile) {
    CleanShutdownMarker marker(dir_);
    ASSERT_TRUE(marker.write().has_value());
    ASSERT_TRUE(marker.exists());

    auto result = marker.remove();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(marker.exists());
    EXPECT_FALSE(fs::exists(dir_ / "clean_shutdown"));
}

TEST_F(CleanShutdownMarkerTest, RemoveOnAbsentFileSucceeds) {
    CleanShutdownMarker marker(dir_);
    ASSERT_FALSE(marker.exists());
    // Removing a file that does not exist must succeed (idempotent).
    auto result = marker.remove();
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(CleanShutdownMarkerTest, WriteIsIdempotent) {
    CleanShutdownMarker marker(dir_);
    ASSERT_TRUE(marker.write().has_value());
    // Writing twice must succeed.
    auto result = marker.write();
    EXPECT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(marker.exists());
}

TEST_F(CleanShutdownMarkerTest, PathReturnsMarkerPath) {
    CleanShutdownMarker marker(dir_);
    EXPECT_EQ(marker.path(), dir_ / "clean_shutdown");
}

TEST_F(CleanShutdownMarkerTest, SecondInstanceSeesSameFile) {
    // Two separate instances pointing at the same directory.
    CleanShutdownMarker m1(dir_);
    CleanShutdownMarker m2(dir_);

    ASSERT_TRUE(m1.write().has_value());
    EXPECT_TRUE(m2.exists()); // m2 sees the file written by m1.

    ASSERT_TRUE(m2.remove().has_value());
    EXPECT_FALSE(m1.exists()); // m1 sees the removal by m2.
}
