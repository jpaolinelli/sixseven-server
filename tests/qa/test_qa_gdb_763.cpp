/// @file tests/qa/test_qa_gdb_763.cpp
/// QA regression tests for GDB-763: Validate buffer_pool_size_mb frame-count
/// overflow and test it.
///
/// The original Adversarial_Uint32OverflowBoundary test in test_qa_gdb_613.cpp
/// was vacuous — it only exercised local variables and documented C++ integer
/// overflow, not any production code path.  This file adds regression coverage
/// for the production validation paths:
///
///   1. Config::load_from_file  — JSON config file parsing
///   2. Config::validate_setting — runtime SET validation (GDB-729 path)
///   3. Config::apply_setting    — runtime SET apply (GDB-729 path)
///
/// Boundary: max safe value is UINT32_MAX / 128 = 33554431 MB.  Exactly that
/// value must be accepted; one above must be rejected with INVALID_ARGUMENT.

#include "sixseven/common/config.h"
#include "sixseven/common/status.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace sixseven {

class QA_GDB763 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb763";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(data_dir_); }

    std::filesystem::path data_dir_;
};

static constexpr uint64_t kMaxSafeMb = static_cast<uint64_t>(UINT32_MAX) / 128; // 33554431

// ---------------------------------------------------------------------------
// load_from_file — JSON config path
// ---------------------------------------------------------------------------

/// Normal values well below the boundary must be accepted.
TEST_F(QA_GDB763, LoadFromFile_NormalValueAccepted) {
    auto cfg_path = data_dir_ / "normal.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"buffer_pool_size_mb": 512})";
    }
    auto cfg = Config::load_from_file(cfg_path.string());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    EXPECT_EQ(cfg->buffer_pool_size_mb, 512u);
}

/// Exactly the max safe value must be accepted without error.
TEST_F(QA_GDB763, LoadFromFile_ExactBoundaryAccepted) {
    auto cfg_path = data_dir_ / "boundary.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"buffer_pool_size_mb": )" << kMaxSafeMb << "}";
    }
    auto cfg = Config::load_from_file(cfg_path.string());
    ASSERT_TRUE(cfg.has_value()) << "Max safe MB " << kMaxSafeMb
                                 << " must be accepted: " << cfg.error().message;
    EXPECT_EQ(cfg->buffer_pool_size_mb, static_cast<size_t>(kMaxSafeMb));
}

/// One above the max must be rejected with INVALID_ARGUMENT.
TEST_F(QA_GDB763, LoadFromFile_OverflowValueRejected) {
    auto cfg_path = data_dir_ / "overflow.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"buffer_pool_size_mb": )" << (kMaxSafeMb + 1) << "}";
    }
    auto cfg = Config::load_from_file(cfg_path.string());
    ASSERT_FALSE(cfg.has_value()) << "Overflow MB " << (kMaxSafeMb + 1) << " must be rejected";
    EXPECT_EQ(cfg.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// validate_setting — GDB-729 runtime SET validation path
// ---------------------------------------------------------------------------

/// Normal string value must validate without error.
TEST_F(QA_GDB763, ValidateSetting_NormalValueAccepted) {
    auto r = Config::validate_setting("storage.buffer_pool_size_mb", "256");
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

/// Exactly the max safe value must validate without error.
TEST_F(QA_GDB763, ValidateSetting_ExactBoundaryAccepted) {
    auto r = Config::validate_setting("storage.buffer_pool_size_mb", std::to_string(kMaxSafeMb));
    ASSERT_TRUE(r.has_value()) << "Max safe MB " << kMaxSafeMb
                               << " must validate: " << r.error().message;
}

/// One above the max must be rejected with INVALID_ARGUMENT.
TEST_F(QA_GDB763, ValidateSetting_OverflowValueRejected) {
    auto r =
        Config::validate_setting("storage.buffer_pool_size_mb", std::to_string(kMaxSafeMb + 1));
    ASSERT_FALSE(r.has_value()) << "Overflow MB " << (kMaxSafeMb + 1) << " must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// apply_setting — GDB-729 runtime SET apply path
// ---------------------------------------------------------------------------

/// Exactly the max safe value must be applied without error.
TEST_F(QA_GDB763, ApplySetting_ExactBoundaryAccepted) {
    Config cfg = Config::load_defaults();
    auto r = cfg.apply_setting("storage.buffer_pool_size_mb", std::to_string(kMaxSafeMb));
    ASSERT_TRUE(r.has_value()) << "Max safe MB " << kMaxSafeMb
                               << " must apply: " << r.error().message;
    EXPECT_EQ(cfg.buffer_pool_size_mb, static_cast<size_t>(kMaxSafeMb));
}

/// One above the max must be rejected and must NOT mutate the config.
TEST_F(QA_GDB763, ApplySetting_OverflowValueRejected) {
    Config cfg = Config::load_defaults();
    size_t original = cfg.buffer_pool_size_mb;
    auto r = cfg.apply_setting("storage.buffer_pool_size_mb", std::to_string(kMaxSafeMb + 1));
    ASSERT_FALSE(r.has_value()) << "Overflow MB " << (kMaxSafeMb + 1) << " must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(cfg.buffer_pool_size_mb, original) << "Config must not be mutated on rejection";
}

} // namespace sixseven
