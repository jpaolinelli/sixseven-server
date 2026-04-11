#include "sixseven/common/crash_handler.h"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Read everything available from a pipe fd until EOF, with a hard cap.
std::string drain_pipe(int fd, std::size_t max_bytes = 64 * 1024) {
    std::string out;
    char buf[1024];
    while (out.size() < max_bytes) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

// Fork a child, install the crash handler in it, raise `signo`, and return
// whatever the child wrote to stderr before dying.
std::string capture_child_crash(int signo) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        ADD_FAILURE() << "pipe() failed";
        return {};
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ADD_FAILURE() << "fork() failed";
        return {};
    }

    if (pid == 0) {
        // Child: redirect stderr → pipe write end, then crash.
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDERR_FILENO) < 0) {
            ::_exit(99);
        }
        ::close(pipefd[1]);

        sixseven::install_crash_handlers();

        // For SIGSEGV we want a real fault, not just raise(), so the
        // handler exercises the SA_SIGINFO path with a real si_addr.
        if (signo == SIGSEGV) {
            volatile int* p = nullptr;
            *p = 42; // NOLINT — intentional null deref
            ::_exit(0); // unreachable
        }

        ::raise(signo);
        ::_exit(0); // unreachable
    }

    // Parent.
    ::close(pipefd[1]);
    std::string out = drain_pipe(pipefd[0]);
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    // Our handler does _exit(128 + signo) — see crash_handler.cpp for the
    // rationale. So the child should have a normal exit with code 128+signo.
    EXPECT_TRUE(WIFEXITED(status))
        << "child should have exited normally via _exit, status=" << status;
    if (WIFEXITED(status)) {
        EXPECT_EQ(WEXITSTATUS(status), 128 + signo)
            << "expected exit code 128+" << signo << ", got " << WEXITSTATUS(status);
    }

    return out;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST(CrashHandler, SIGSEGVDumpsBacktraceToStderr) {
    std::string out = capture_child_crash(SIGSEGV);

    EXPECT_TRUE(contains(out, "sixseven-server caught fatal signal"))
        << "missing crash banner; stderr was:\n" << out;
    EXPECT_TRUE(contains(out, "SIGSEGV"))
        << "missing SIGSEGV name; stderr was:\n" << out;
    EXPECT_TRUE(contains(out, "si_addr"))
        << "missing si_addr line; stderr was:\n" << out;
    EXPECT_TRUE(contains(out, "--- backtrace"))
        << "missing backtrace marker; stderr was:\n" << out;
    EXPECT_TRUE(contains(out, "--- end backtrace ---"))
        << "missing end-of-backtrace marker; stderr was:\n" << out;
}

TEST(CrashHandler, SIGABRTDumpsBacktraceToStderr) {
    std::string out = capture_child_crash(SIGABRT);

    EXPECT_TRUE(contains(out, "SIGABRT"))
        << "missing SIGABRT name; stderr was:\n" << out;
    EXPECT_TRUE(contains(out, "--- backtrace"))
        << "missing backtrace marker; stderr was:\n" << out;
}

TEST(CrashHandler, IsIdempotent) {
    // Calling install_crash_handlers() twice in the same process must not
    // crash, deadlock, or corrupt state. We then crash a child process to
    // confirm the handlers still work after a re-install.
    sixseven::install_crash_handlers();
    sixseven::install_crash_handlers();

    std::string out = capture_child_crash(SIGABRT);
    EXPECT_TRUE(contains(out, "--- backtrace")) << out;
}
