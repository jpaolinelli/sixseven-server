#pragma once

// platform.h — Cross-platform POSIX compatibility layer for SixSevenDB.
//
// On POSIX systems (Linux, macOS) this header includes the native system
// headers and is otherwise a no-op.
//
// On Windows this header:
//   1. Suppresses MSVC deprecation warnings so the CRT POSIX-compat names
//      (open, close, read, write, unlink, …) are available at global scope.
//   2. Provides inline implementations for APIs that truly don't exist on
//      Windows: pread, pwrite, flock, ftruncate, fsync, fdatasync.
//   3. Provides socket helpers (socket_close, platform_init for WSAStartup).
//
// IMPORTANT: This header must NEVER #define common identifiers (open, close,
// read, write, stat, etc.) as macros — doing so breaks the MSVC STL which
// uses those same names as member functions in <fstream> and elsewhere.

#if defined(_WIN32)

// ── Suppress MSVC CRT deprecation warnings ──────────────────────────────────
// Must come before ANY #include.
#if !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_CRT_NONSTDC_NO_DEPRECATION_WARNINGS)
#define _CRT_NONSTDC_NO_DEPRECATION_WARNINGS
#endif

// _WIN32_WINNT must be set before <winsock2.h> to enable WSAPoll (Vista+).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>        // open, close, read, write, lseek, _pipe, _commit, _get_osfhandle
#include <fcntl.h>     // O_RDONLY, O_RDWR, O_CREAT, O_TRUNC, O_EXCL
#include <sys/stat.h>  // stat, fstat (32-bit st_size; see sixseven_fstat below)
#include <process.h>   // getpid
#include <direct.h>    // mkdir
#include <cerrno>
#include <cstdint>
#include <cstring>

// ── Undefine Windows macros that collide with our identifiers ────────────────
// <windows.h> defines DELETE as a virtual key code (0x2E). This breaks
// our WalRecordType::DELETE enum value.
#ifdef DELETE
#undef DELETE
#endif

// The Windows SDK SAL headers define IN and OUT as empty annotation macros,
// which mangle our TraverseDirection::IN/OUT enum values when platform.h is
// included before parser/ast.h.
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

// ── Type aliases ─────────────────────────────────────────────────────────────
#if !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
using ssize_t = std::int64_t;
#endif

// ── File-open flags ──────────────────────────────────────────────────────────
// <fcntl.h> on MSVC already defines O_RDONLY, O_RDWR, O_CREAT, etc.
// O_CLOEXEC has no meaning on Windows (no fork+exec); define as 0.
#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

// ── flock constants ──────────────────────────────────────────────────────────
#if !defined(LOCK_SH)
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif

// ── STDERR_FILENO ────────────────────────────────────────────────────────────
#if !defined(STDERR_FILENO)
#define STDERR_FILENO 2
#endif

// ── Signals not available on Windows ─────────────────────────────────────────
#include <csignal>
#if !defined(SIGBUS)
#define SIGBUS  10
#endif
#if !defined(SIGTRAP)
#define SIGTRAP 5
#endif

// ── pread / pwrite ───────────────────────────────────────────────────────────
// Windows has no pread/pwrite. Implement using Win32 ReadFile/WriteFile with
// OVERLAPPED for atomic positioned I/O (no seeking, no file pointer movement).

namespace sixseven_platform {

inline ssize_t pread(int fd, void* buf, std::size_t count, std::int64_t offset) {
    HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
    DWORD bytes_read = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(count), &bytes_read, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
            return static_cast<ssize_t>(bytes_read);
        }
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(bytes_read);
}

inline ssize_t pwrite(int fd, const void* buf, std::size_t count, std::int64_t offset) {
    HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
    DWORD bytes_written = 0;
    if (!WriteFile(h, buf, static_cast<DWORD>(count), &bytes_written, &ov)) {
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(bytes_written);
}

// ── flock ────────────────────────────────────────────────────────────────────
inline int flock(int fd, int operation) {
    HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (operation & LOCK_UN) {
        OVERLAPPED ov{};
        if (!UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov)) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    DWORD flags = 0;
    if (operation & LOCK_EX) flags |= LOCKFILE_EXCLUSIVE_LOCK;
    if (operation & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    OVERLAPPED ov{};
    if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
        errno = (GetLastError() == ERROR_LOCK_VIOLATION) ? EWOULDBLOCK : EACCES;
        return -1;
    }
    return 0;
}

// ── ftruncate ────────────────────────────────────────────────────────────────
inline int ftruncate(int fd, std::int64_t length) {
    return ::_chsize_s(fd, length);
}

// ── fsync / fdatasync ────────────────────────────────────────────────────────
inline int fsync(int fd) { return ::_commit(fd); }
inline int fdatasync(int fd) { return ::_commit(fd); }

// ── 64-bit fstat wrapper ─────────────────────────────────────────────────────
// MSVC's fstat() returns a struct _stat with a 32-bit st_size. For a database
// we need 64-bit file sizes. This wrapper uses _fstat64 / struct __stat64.
struct stat64_t : __stat64 {};

inline int fstat64(int fd, stat64_t* st) {
    return ::_fstat64(fd, st);
}

// ── Socket helpers ───────────────────────────────────────────────────────────
inline int socket_close(int fd) {
    return ::closesocket(static_cast<SOCKET>(fd));
}

inline int set_socket_nonblocking(int fd) {
    u_long mode = 1;
    return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
}

inline bool is_socket_would_block() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

// ── pipe ─────────────────────────────────────────────────────────────────────
inline int pipe(int fds[2]) {
    return ::_pipe(fds, 4096, _O_BINARY);
}

// ── socketpair (emulated via loopback TCP) ───────────────────────────────────
// Windows has no socketpair(). Emulate it with a loopback TCP connection.
// Used in tests to create connected socket pairs.
inline int socketpair(int /*domain*/, int /*type*/, int /*protocol*/, int fds[2]) {
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // ephemeral
    if (::bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(listener);
        return -1;
    }
    int addrlen = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
        ::closesocket(listener);
        return -1;
    }
    if (::listen(listener, 1) != 0) {
        ::closesocket(listener);
        return -1;
    }
    SOCKET connector = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connector == INVALID_SOCKET) {
        ::closesocket(listener);
        return -1;
    }
    if (::connect(connector, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(connector);
        ::closesocket(listener);
        return -1;
    }
    SOCKET acceptor = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);
    if (acceptor == INVALID_SOCKET) {
        ::closesocket(connector);
        return -1;
    }
    fds[0] = static_cast<int>(connector);
    fds[1] = static_cast<int>(acceptor);
    return 0;
}

// ── mkstemp / mkdtemp ────────────────────────────────────────────────────────
// POSIX mkstemp/mkdtemp don't exist on Windows. Provide simple replacements.
inline int mkstemp(char* tmpl) {
    if (::_mktemp_s(tmpl, std::strlen(tmpl) + 1) != 0) return -1;
    return ::_open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR, _S_IREAD | _S_IWRITE);
}

inline char* mkdtemp(char* tmpl) {
    if (::_mktemp_s(tmpl, std::strlen(tmpl) + 1) != 0) return nullptr;
    if (::_mkdir(tmpl) != 0) return nullptr;
    return tmpl;
}

} // namespace sixseven_platform

// ── Platform init ────────────────────────────────────────────────────────────
namespace sixseven {
inline bool platform_init() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
inline void platform_cleanup() { WSACleanup(); }
} // namespace sixseven

#else // !_WIN32

// ── POSIX (Linux / macOS) — include native headers ───────────────────────────

#include <cstdlib>    // mkstemp, mkdtemp
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace sixseven_platform {

// On POSIX, all APIs exist natively. These wrappers just forward.
inline ssize_t pread(int fd, void* buf, size_t n, off_t off) { return ::pread(fd, buf, n, off); }
inline ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) { return ::pwrite(fd, buf, n, off); }
inline int flock(int fd, int op) { return ::flock(fd, op); }
inline int ftruncate(int fd, off_t len) { return ::ftruncate(fd, len); }
inline int fsync(int fd) { return ::fsync(fd); }
#ifdef __APPLE__
inline int fdatasync(int fd) { return ::fsync(fd); } // macOS has no fdatasync
#else
inline int fdatasync(int fd) { return ::fdatasync(fd); }
#endif
inline int pipe(int fds[2]) { return ::pipe(fds); }

// 64-bit fstat — on POSIX, struct stat already has a 64-bit st_size.
using stat64_t = struct stat;
inline int fstat64(int fd, stat64_t* st) { return ::fstat(fd, st); }

// Socket helpers — on POSIX, sockets are regular file descriptors.
inline int socket_close(int fd) { return ::close(fd); }
inline int socketpair(int domain, int type, int protocol, int fds[2]) {
    return ::socketpair(domain, type, protocol, fds);
}

// mkstemp / mkdtemp — just forward to the native POSIX functions.
inline int mkstemp(char* tmpl) { return ::mkstemp(tmpl); }
inline char* mkdtemp(char* tmpl) { return ::mkdtemp(tmpl); }

} // namespace sixseven_platform

namespace sixseven {
inline bool platform_init() { return true; }
inline void platform_cleanup() {}
} // namespace sixseven

#endif // _WIN32
