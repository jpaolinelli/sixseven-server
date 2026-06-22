#include "sixseven/storage/clean_shutdown_marker.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/status.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sixseven {

CleanShutdownMarker::CleanShutdownMarker(std::filesystem::path data_dir)
    : data_dir_(std::move(data_dir)), marker_path_(data_dir_ / "clean_shutdown") {}

bool CleanShutdownMarker::exists() const {
    std::error_code ec;
    return std::filesystem::exists(marker_path_, ec);
}

Result<void> CleanShutdownMarker::write() {
    // Open/create and immediately close the marker file to create it.
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(marker_path_.wstring().c_str(),
                             GENERIC_WRITE,
                             0,
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("CleanShutdownMarker::write CreateFile: error=") +
                              std::to_string(::GetLastError()));
    }
    // FlushFileBuffers ensures the file content (even if empty) is durable.
    (void)::FlushFileBuffers(h);
    ::CloseHandle(h);
#else
    int fd = ::open(marker_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("CleanShutdownMarker::write open: ") + std::strerror(errno));
    }
    if (::fsync(fd) < 0) {
        (void)::close(fd);
        return make_error(StatusCode::IO_ERROR,
                          std::string("CleanShutdownMarker::write fsync: ") + std::strerror(errno));
    }
    (void)::close(fd);
#endif

    // fsync the directory so the new entry is durable.
    auto dir_result = fsync_dir();
    if (!dir_result) {
        SIXSEVEN_LOG_WARN("clean-shutdown marker: directory fsync failed (non-fatal): {}",
                          dir_result.error().message);
    }

    return ok();
}

Result<void> CleanShutdownMarker::remove() {
    std::error_code ec;
    std::filesystem::remove(marker_path_, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("CleanShutdownMarker::remove: ") + ec.message());
    }

    // fsync the directory so the removal is durable.
    auto dir_result = fsync_dir();
    if (!dir_result) {
        SIXSEVEN_LOG_WARN("clean-shutdown marker: directory fsync after remove failed (non-fatal): "
                          "{}",
                          dir_result.error().message);
    }

    return ok();
}

Result<void> CleanShutdownMarker::fsync_dir() const {
#if defined(_WIN32)
    // On Windows, NTFS guarantees directory-entry durability through its own
    // journal; FlushFileBuffers on a directory handle requires admin rights,
    // so we skip it and rely on NTFS semantics.
    return ok();
#else
    int dfd = ::open(data_dir_.c_str(), O_RDONLY);
    if (dfd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          std::string("CleanShutdownMarker: dir open: ") + std::strerror(errno));
    }
    if (::fsync(dfd) < 0) {
        int saved = errno;
        (void)::close(dfd);
        return make_error(StatusCode::IO_ERROR,
                          std::string("CleanShutdownMarker: dir fsync: ") + std::strerror(saved));
    }
    (void)::close(dfd);
    return ok();
#endif
}

} // namespace sixseven
