#include "sixseven/common/crash_handler.h"

#include "sixseven/common/platform.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>

#if !defined(_WIN32)
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sixseven {

namespace {

// CRITICAL: Everything in this anonymous namespace runs from inside a fatal
// signal handler. Nothing here may call malloc, std::string, std::cout,
// spdlog, fmt, or any other non-async-signal-safe function. Only:
//   - write(2)
//   - backtrace()  (POSIX, async-signal-safe on Darwin and Linux)
//   - signal()/raise() with constant arguments
//   - integer arithmetic on stack-local fixed buffers
//
// We deliberately do NOT call backtrace_symbols_fd(): on macOS it calls
// dladdr() under the dyld lock, which deadlocks if any other thread is
// inside the dynamic linker when the signal lands. We print raw frame
// addresses instead — symbolize them manually with:
//
//     atos -o ./build/debug/src/sixseven-server <addr1> <addr2> ...
//     # or, on Linux:
//     addr2line -e ./build/debug/src/sixseven-server -f -C <addr1> ...
//
// Adding ANY non-async-signal-safe call here can deadlock the process inside
// the handler.

constexpr int kBacktraceMaxFrames = 64;
#if !defined(_WIN32)
constexpr std::size_t kAltStackSize = 64 * 1024; // 64 KiB

// Captured at install time (normal context, safe to call dyld). Used inside
// the signal handler — where dyld calls would deadlock — so users can
// pass it to `atos -l` to symbolize backtrace addresses.
std::uintmax_t g_main_image_load_address = 0;
#endif

// Async-signal-safe write of a NUL-terminated literal C-string.
void safe_write(const char* msg) {
    std::size_t n = 0;
    while (msg[n] != '\0') {
        ++n;
    }
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    WriteFile(h, msg, static_cast<DWORD>(n), &written, nullptr);
#else
    (void)::write(STDERR_FILENO, msg, n);
#endif
}

// Async-signal-safe write of an unsigned integer in decimal.
void safe_write_uint(std::uintmax_t v) {
    char buf[32];
    int i = sizeof(buf);
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v > 0 && i > 0) {
            buf[--i] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    WriteFile(h, buf + i, static_cast<DWORD>(sizeof(buf) - static_cast<std::size_t>(i)),
              &written, nullptr);
#else
    (void)::write(STDERR_FILENO, buf + i, static_cast<std::size_t>(sizeof(buf) - i));
#endif
}

// Async-signal-safe write of an unsigned integer in hex (with 0x prefix).
void safe_write_hex(std::uintmax_t v) {
    char buf[32];
    int i = sizeof(buf);
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v > 0 && i > 0) {
            int digit = static_cast<int>(v & 0xF);
            buf[--i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            v >>= 4;
        }
    }
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    WriteFile(h, "0x", 2, &written, nullptr);
    WriteFile(h, buf + i, static_cast<DWORD>(sizeof(buf) - static_cast<std::size_t>(i)),
              &written, nullptr);
#else
    (void)::write(STDERR_FILENO, "0x", 2);
    (void)::write(STDERR_FILENO, buf + i, static_cast<std::size_t>(sizeof(buf) - i));
#endif
}

const char* signal_name(int signo) {
    switch (signo) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    case SIGTRAP:
        return "SIGTRAP";
    default:
        return "UNKNOWN";
    }
}

void write_backtrace() {
#if defined(_WIN32)
    safe_write("--- backtrace (use WinDbg or dumpbin to symbolize) ---\n");
    void* frames[kBacktraceMaxFrames];
    USHORT frame_count = CaptureStackBackTrace(
        0, static_cast<ULONG>(kBacktraceMaxFrames), frames, nullptr);
    for (USHORT i = 0; i < frame_count; ++i) {
        safe_write("  #");
        safe_write_uint(static_cast<std::uintmax_t>(i));
        safe_write(" ");
        safe_write_hex(reinterpret_cast<std::uintmax_t>(frames[i]));
        safe_write("\n");
    }
#else
    safe_write("--- backtrace (symbolize with `atos -o <binary> -l ");
    safe_write_hex(g_main_image_load_address);
    safe_write(" <addr...>`) ---\n");
    void* frames[kBacktraceMaxFrames];
    int frame_count = ::backtrace(frames, kBacktraceMaxFrames);
    for (int i = 0; i < frame_count; ++i) {
        safe_write("  #");
        safe_write_uint(static_cast<std::uintmax_t>(i));
        safe_write(" ");
        safe_write_hex(reinterpret_cast<std::uintmax_t>(frames[i]));
        safe_write("\n");
    }
#endif
    safe_write("--- end backtrace ---\n");
}

#if !defined(_WIN32)
extern "C" void crash_signal_handler(int signo, siginfo_t* info, void* /*context*/) {
    safe_write("\n=== sixseven-server caught fatal signal: ");
    safe_write(signal_name(signo));
    safe_write(" (");
    safe_write_uint(static_cast<std::uintmax_t>(signo));
    safe_write(") ===\n");

    if (info != nullptr) {
        safe_write("  si_code: ");
        safe_write_uint(static_cast<std::uintmax_t>(info->si_code));
        safe_write("\n  si_addr: ");
        safe_write_hex(reinterpret_cast<std::uintmax_t>(info->si_addr));
        safe_write("\n");
    }

    safe_write("  thread:  ");
    safe_write_hex(reinterpret_cast<std::uintmax_t>(pthread_self()));
    safe_write("\n  pid:     ");
    safe_write_uint(static_cast<std::uintmax_t>(::getpid()));
    safe_write("\n");

    write_backtrace();

    // Terminate immediately with _exit(128 + signo). We deliberately do NOT
    // restore SIG_DFL and re-raise: on macOS, fatal signals get routed
    // through Mach exception ports to ReportCrash, which can hang the
    // process for minutes (even SIGKILL won't unstick it during that
    // window) and obscures the backtrace we just wrote. _exit bypasses all
    // of that and gives a clean, observable exit code. The cost is no core
    // dump — but our diagnostic value comes from the in-process backtrace
    // we already wrote, not from a core file.
    ::_exit(128 + signo);
}
#endif // !defined(_WIN32)

void terminate_handler() {
    safe_write("\n=== sixseven-server std::terminate called ===\n");

    // If there's an in-flight exception, log its type and what().
    // std::current_exception/rethrow_exception are not strictly listed as
    // async-signal-safe, but std::terminate is not running from a signal
    // context — it's a normal C++ runtime callback — so this is safe.
    std::exception_ptr eptr = std::current_exception();
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            safe_write("  exception: ");
            safe_write(e.what());
            safe_write("\n");
        } catch (...) {
            safe_write("  exception: unknown non-std::exception type\n");
        }
    } else {
        safe_write("  no active exception\n");
    }

    write_backtrace();

    // Same reasoning as crash_signal_handler: skip abort()/Mach-exception
    // routing and exit immediately so the backtrace we just wrote isn't
    // hidden behind a multi-minute ReportCrash hang.
    ::_exit(134); // 128 + SIGABRT
}

} // namespace

void install_crash_handlers() {
#if defined(_WIN32)

    // Windows: use Structured Exception Handling (SEH) for crash capture.
    // SetUnhandledExceptionFilter installs a top-level filter that runs
    // when an unhandled SEH exception occurs (access violation, etc.).
    static auto seh_filter = [](EXCEPTION_POINTERS* ep) -> LONG {
        safe_write("\n=== sixseven-server caught fatal exception ===\n");
        safe_write("  ExceptionCode: ");
        safe_write_hex(static_cast<std::uintmax_t>(ep->ExceptionRecord->ExceptionCode));
        safe_write("\n  ExceptionAddress: ");
        safe_write_hex(
            reinterpret_cast<std::uintmax_t>(ep->ExceptionRecord->ExceptionAddress));
        safe_write("\n  thread:  ");
        safe_write_uint(static_cast<std::uintmax_t>(GetCurrentThreadId()));
        safe_write("\n  pid:     ");
        safe_write_uint(static_cast<std::uintmax_t>(getpid()));
        safe_write("\n");
        write_backtrace();
        ::_exit(1);
        return EXCEPTION_EXECUTE_HANDLER; // Unreachable; satisfies the type.
    };
    SetUnhandledExceptionFilter(seh_filter);

    // Also install POSIX-style signal handlers for the signals Windows does support.
    // (SIGABRT, SIGFPE, SIGILL, SIGSEGV, SIGTERM are available on Windows.)
    auto win_sig_handler = [](int signo) {
        safe_write("\n=== sixseven-server caught fatal signal: ");
        safe_write(signal_name(signo));
        safe_write(" ===\n");
        safe_write("  thread:  ");
        safe_write_uint(static_cast<std::uintmax_t>(GetCurrentThreadId()));
        safe_write("\n  pid:     ");
        safe_write_uint(static_cast<std::uintmax_t>(getpid()));
        safe_write("\n");
        write_backtrace();
        ::_exit(128 + signo);
    };

    signal(SIGSEGV, win_sig_handler);
    signal(SIGABRT, win_sig_handler);
    signal(SIGFPE,  win_sig_handler);
    signal(SIGILL,  win_sig_handler);

#else // POSIX

    // Capture the main binary's load address once, in normal context, so
    // the signal handler doesn't need to call dyld functions (which would
    // deadlock against the dyld lock if held by another thread). Image 0
    // is the main executable; its mach_header pointer is the load address
    // that `atos -l` wants.
#if defined(__APPLE__)
    g_main_image_load_address =
        reinterpret_cast<std::uintmax_t>(::_dyld_get_image_header(0));
#endif

    // Set up an alternate signal stack on the calling thread so a stack-
    // overflow crash on this thread can still run the handler.
    // NOTE: only the calling thread (typically main) gets the alt stack;
    // worker threads will run the handler on their own stack.
    static char alt_stack_storage[kAltStackSize];
    static bool alt_stack_installed = false;
    if (!alt_stack_installed) {
        stack_t alt_stack{};
        alt_stack.ss_sp = alt_stack_storage;
        alt_stack.ss_size = kAltStackSize;
        alt_stack.ss_flags = 0;
        (void)::sigaltstack(&alt_stack, nullptr);
        alt_stack_installed = true;
    }

    struct sigaction sa{};
    sa.sa_sigaction = &crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    constexpr int fatal_signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
    for (int sig : fatal_signals) {
        (void)::sigaction(sig, &sa, nullptr);
    }

#endif // _WIN32

    // Catch terminate-via-uncaught-exception (tl::bad_expected_access,
    // Ort::Exception escaping noexcept, etc.) before it becomes a bare abort.
    std::set_terminate(&terminate_handler);
}

} // namespace sixseven
