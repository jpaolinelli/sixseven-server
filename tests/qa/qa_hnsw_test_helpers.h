#pragma once

/// @file qa_hnsw_test_helpers.h
/// @brief Shared TempDir helper for HNSW QA test fixtures.
///
/// Extracted from test_qa_gdb_123.cpp and test_qa_gdb_124.cpp as part of
/// GDB-857 (de-duplicate copy-pasted TempDir helper).
///
/// Each test file provides a unique prefix string so that concurrent runs do
/// not collide.  The Fixture struct is NOT shared because it varies per file
/// (the database file name differs).
///
/// ODR safety: QaTempDir is a class defined in namespace sixseven.  Class
/// definitions may appear in multiple TUs in the same binary as long as they
/// are identical (C++ ODR, basic.def p1).  All member functions are defined
/// inline inside the class body (implicitly inline).

#include "sixseven/common/platform.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace sixseven {

/// RAII temporary directory for disk-backed QA tests.
///
/// Creates a unique directory under std::filesystem::temp_directory_path()
/// using the supplied @p prefix.  The directory is removed recursively on
/// destruction.
///
/// @param prefix  Distinguishing prefix embedded in the directory name, e.g.
///                "sixseven_qa123".  Must be a valid directory-name fragment
///                (no path separators).
class QaTempDir {
public:
    explicit QaTempDir(const std::string& prefix) {
        path_ = std::filesystem::temp_directory_path() / (prefix + "_XXXXXX");
        std::string tmpl = path_.string();
        char* result = sixseven_platform::mkdtemp(tmpl.data());
        EXPECT_NE(result, nullptr);
        path_ = result;
    }

    ~QaTempDir() { std::filesystem::remove_all(path_); }

    // Non-copyable, non-movable (owns a directory on disk).
    QaTempDir(const QaTempDir&) = delete;
    QaTempDir& operator=(const QaTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace sixseven
