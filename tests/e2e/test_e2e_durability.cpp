/// E2E test: kill -9 durability scenario (GDB-959).
///
/// Launches a real sixseven-server child process, connects via the pg-wire
/// protocol, inserts committed rows, then hard-kills the process (SIGKILL).
/// A second server launch on the same data directory must recover via WAL
/// and serve the committed rows.
///
/// Windows: the scenario requires POSIX SIGKILL semantics and socket
/// behaviour that is unreliable under the Windows CRT fd-assert.  The test
/// is registered so CTest -L e2e discovers and reports it, but it
/// GTEST_SKIPs cleanly on _WIN32.  The real pass is POSIX / CI only.

#include "sixseven/cli/pg_wire_codec.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifndef _WIN32

/// Write a minimal JSON config for sixseven-server.
void write_server_config(const fs::path& cfg_path, const fs::path& data_dir,
                         int port) {
    std::ofstream f(cfg_path);
    f << "{\n"
      << "  \"data_dir\": \"" << data_dir.string() << "\",\n"
      << "  \"port\": " << port << ",\n"
      << "  \"log_level\": \"warn\",\n"
      << "  \"auth_method\": \"trust\",\n"
      << "  \"buffer_pool_size_mb\": 32\n"
      << "}\n";
}

/// Attempt to connect to host:port, retrying until timeout_ms elapses.
/// Returns connected socket fd, or -1 on timeout/failure.
int connect_with_retry(const char* host, int port, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, host, &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

/// Blocking read until 'Z' (ReadyForQuery) appears or timeout expires.
/// Accumulates all received bytes into out.
bool wait_ready_for_query(int fd, std::vector<uint8_t>& out, int timeout_ms) {
    std::vector<uint8_t> buf(4096);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto n = ::recv(fd, reinterpret_cast<char*>(buf.data()),
                        static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            return false;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + n);
        for (uint8_t b : out) {
            if (b == 'Z') {
                return true;
            }
        }
    }
    return false;
}

/// Send startup + a SQL query on fd; collect the response into out.
/// Returns true if ReadyForQuery was received for the query response.
bool pg_exec(int fd, const std::string& sql, std::vector<uint8_t>& out) {
    // Send startup.
    auto startup = sixseven::cli::encode_startup_message("sixseven", "sixseven");
    if (::send(fd, reinterpret_cast<const char*>(startup.data()),
               static_cast<int>(startup.size()), 0) < 0) {
        return false;
    }
    // Wait for ReadyForQuery from startup phase.
    if (!wait_ready_for_query(fd, out, /*ms=*/5000)) {
        return false;
    }

    // Send query.
    auto qmsg = sixseven::cli::encode_query_message(sql);
    if (::send(fd, reinterpret_cast<const char*>(qmsg.data()),
               static_cast<int>(qmsg.size()), 0) < 0) {
        return false;
    }
    // Wait for ReadyForQuery from query response.
    out.clear();
    return wait_ready_for_query(fd, out, /*ms=*/8000);
}

/// Launch server using the JSON config at cfg_path.  Returns child PID or -1.
pid_t launch_server(const fs::path& cfg_path) {
    pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        std::string cfg_str = cfg_path.string();
        ::execlp(SIXSEVEN_SERVER_BINARY, SIXSEVEN_SERVER_BINARY,
                 cfg_str.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid;
}

#endif // !_WIN32

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class E2EDurabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = fs::temp_directory_path() / "sixseven_e2e_dur_959";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);
    }

    void TearDown() override {
#ifndef _WIN32
        // Kill any still-running server child (defensive; normally reaped in test).
        if (server_pid_ > 0) {
            ::kill(server_pid_, SIGKILL);
            int status = 0;
            ::waitpid(server_pid_, &status, 0);
            server_pid_ = -1;
        }
#endif
        std::error_code ec;
        fs::remove_all(data_dir_, ec);
    }

    fs::path data_dir_;
#ifndef _WIN32
    pid_t server_pid_ = -1;
#endif

    // Unique port for this test class.  Pick something unlikely to collide.
    static constexpr int kPort = 16759;
};

// ---------------------------------------------------------------------------
// TEST: kill -9 durability
// ---------------------------------------------------------------------------

TEST_F(E2EDurabilityTest, CommittedRowsSurviveKillNine) {
#ifdef _WIN32
    GTEST_SKIP() << "kill -9 durability scenario requires POSIX; "
                    "skipped on Windows (POSIX/CI only - GDB-959)";
#else
    // ------------------------------------------------------------------
    // Phase 1: Start server, insert committed rows, hard-kill.
    // ------------------------------------------------------------------
    fs::path cfg = data_dir_ / "server.json";
    write_server_config(cfg, data_dir_, kPort);

    server_pid_ = launch_server(cfg);
    ASSERT_GT(server_pid_, 0) << "fork/exec failed";

    int fd1 = connect_with_retry("127.0.0.1", kPort, /*timeout_ms=*/8000);
    ASSERT_GE(fd1, 0) << "server did not become ready within 8 s";

    std::vector<uint8_t> resp;
    ASSERT_TRUE(pg_exec(fd1,
                        "CREATE TABLE dur_test "
                        "(id INT32 NOT NULL, val STRING NOT NULL);",
                        resp))
        << "CREATE TABLE failed";

    ::close(fd1);

    // Reconnect for INSERT (reuse the startup handshake each time).
    fd1 = connect_with_retry("127.0.0.1", kPort, /*timeout_ms=*/2000);
    ASSERT_GE(fd1, 0) << "reconnect for INSERT failed";

    resp.clear();
    ASSERT_TRUE(pg_exec(fd1,
                        "INSERT INTO dur_test VALUES (1, 'alpha');"
                        "INSERT INTO dur_test VALUES (2, 'beta');",
                        resp))
        << "INSERT failed";

    ::close(fd1);

    // Hard-kill: no clean shutdown, no clean-shutdown marker.
    ::kill(server_pid_, SIGKILL);
    int status = 0;
    ::waitpid(server_pid_, &status, 0);
    server_pid_ = -1;

    // ------------------------------------------------------------------
    // Phase 2: Restart on same data dir; verify rows survived WAL recovery.
    // ------------------------------------------------------------------
    server_pid_ = launch_server(cfg);
    ASSERT_GT(server_pid_, 0) << "second fork/exec failed";

    int fd2 = connect_with_retry("127.0.0.1", kPort, /*timeout_ms=*/10000);
    ASSERT_GE(fd2, 0) << "restarted server did not become ready within 10 s";

    std::vector<uint8_t> sel_resp;
    ASSERT_TRUE(pg_exec(fd2, "SELECT id, val FROM dur_test ORDER BY id;", sel_resp))
        << "SELECT after restart failed";

    ::close(fd2);

    // Kill the second server cleanly before teardown.
    ::kill(server_pid_, SIGTERM);
    ::waitpid(server_pid_, &status, 0);
    server_pid_ = -1;

    // Verify both committed rows are present in the DataRow bytes.
    std::string resp_str(sel_resp.begin(), sel_resp.end());
    EXPECT_NE(resp_str.find("alpha"), std::string::npos)
        << "committed row 'alpha' not found after WAL recovery";
    EXPECT_NE(resp_str.find("beta"), std::string::npos)
        << "committed row 'beta' not found after WAL recovery";
#endif // _WIN32
}
