#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace giodb {
namespace test {

/// Temporary WAL directory with automatic cleanup.
/// Shared between test_wal.cpp and test_wal_recovery.cpp.
class TempWalDir {
public:
    TempWalDir() {
        path_ = std::filesystem::temp_directory_path() / ("wal_test_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempWalDir() { std::filesystem::remove_all(path_); }

    // Non-copyable, non-movable.
    TempWalDir(const TempWalDir&) = delete;
    TempWalDir& operator=(const TempWalDir&) = delete;
    TempWalDir(TempWalDir&&) = delete;
    TempWalDir& operator=(TempWalDir&&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

} // namespace test
} // namespace giodb
