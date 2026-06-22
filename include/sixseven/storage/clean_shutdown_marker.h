#pragma once

#include "sixseven/common/result.h"

#include <filesystem>

namespace sixseven {

// -- Clean-Shutdown Marker ----------------------------------------------------
//
// A durable flag-file that records whether the previous server run ended via
// a graceful shutdown (clean) or was interrupted by a crash / kill -9 (dirty).
//
// Semantics:
//   - On NORMAL startup: marker ABSENT  => crash path, WAL recovery must run.
//                        marker PRESENT => clean path, delete it, skip recovery.
//   - On GRACEFUL shutdown: write + fsync the marker AFTER all data is flushed.
//   - On CRASH / abnormal exit: marker is never written, so the next startup
//     correctly detects the need for recovery.
//
// The file is placed under <data_dir>/clean_shutdown.
// An fsync of the containing directory is performed after create/remove to make
// the directory-entry mutation durable (POSIX systems only — on Windows a
// FlushFileBuffers on the directory handle is attempted but failure is
// non-fatal, as Windows NTFS maintains its own durable directory journal).

class CleanShutdownMarker {
public:
    explicit CleanShutdownMarker(std::filesystem::path data_dir);

    /// Check whether the clean-shutdown marker file exists.
    [[nodiscard]] bool exists() const;

    /// Write the marker file and fsync it (and the directory).
    /// Call after all data has been flushed on graceful shutdown.
    [[nodiscard]] Result<void> write();

    /// Remove the marker file and fsync the directory.
    /// Call at the START of a clean-path startup (after confirming presence).
    [[nodiscard]] Result<void> remove();

    /// Return the marker file path (useful for logging).
    [[nodiscard]] const std::filesystem::path& path() const { return marker_path_; }

private:
    /// fsync the parent directory of marker_path_ to make directory-entry
    /// mutations durable. Non-fatal on Windows (logs a warning).
    [[nodiscard]] Result<void> fsync_dir() const;

    std::filesystem::path data_dir_;
    std::filesystem::path marker_path_;
};

} // namespace sixseven
