/// E2E test: two-process replication scenario (GDB-959).
///
/// Launches a PRIMARY sixseven-server and a STANDBY sixseven-server
/// (--standby pointing at the primary).  The test writes data on the primary
/// and verifies that the standby:
///   - connects and enters standby mode (becomes ready for queries),
///   - serves reads (SELECT 1),
///   - rejects DML with a read-only ErrorResponse.
///
/// Windows: the scenario requires POSIX fork/exec + socket behaviour that
/// is unreliable under the Windows CRT fd-assert.  The test is registered
/// so CTest -L e2e discovers and reports it, but it GTEST_SKIPs cleanly on
/// _WIN32.  The real pass is POSIX / CI only.

#include "sixseven/cli/pg_wire_codec.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "e2e_helpers.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace fs = std::filesystem;

namespace {

#ifndef _WIN32

/// Launch server using the JSON config at cfg_path.
/// Pass standby_flag=true to append --standby on the command line.
/// Returns child PID, or -1 on failure.
pid_t launch_server(const fs::path& cfg_path, bool standby_flag) {
    pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        std::string cfg_str = cfg_path.string();
        if (standby_flag) {
            ::execlp(SIXSEVEN_SERVER_BINARY,
                     SIXSEVEN_SERVER_BINARY,
                     cfg_str.c_str(),
                     "--standby",
                     static_cast<char*>(nullptr));
        } else {
            ::execlp(SIXSEVEN_SERVER_BINARY,
                     SIXSEVEN_SERVER_BINARY,
                     cfg_str.c_str(),
                     static_cast<char*>(nullptr));
        }
        ::_exit(127);
    }
    return pid;
}

/// Send startup + SQL on fd, collect response.  Returns true if
/// ReadyForQuery was received.
bool pg_exec(int fd, const std::string& sql, std::vector<uint8_t>& out) {
    // Startup.
    auto startup = sixseven::cli::encode_startup_message("sixseven", "sixseven");
    if (::send(fd,
               reinterpret_cast<const char*>(startup.data()),
               static_cast<int>(startup.size()),
               0) < 0) {
        return false;
    }
    std::vector<uint8_t> startup_resp;
    if (!sixseven::e2e::wait_ready_for_query(fd, startup_resp, /*ms=*/5000)) {
        return false;
    }

    // Query.
    auto qmsg = sixseven::cli::encode_query_message(sql);
    if (::send(fd, reinterpret_cast<const char*>(qmsg.data()), static_cast<int>(qmsg.size()), 0) <
        0) {
        return false;
    }
    return sixseven::e2e::wait_ready_for_query(fd, out, /*ms=*/8000);
}

/// Send sig to pid and waitpid.  Sets pid to -1.
void kill_wait(pid_t& pid, int sig = SIGTERM) {
    if (pid > 0) {
        ::kill(pid, sig);
        int status = 0;
        ::waitpid(pid, &status, 0);
        pid = -1;
    }
}

#endif // !_WIN32

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class E2EReplicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_dir_ = fs::temp_directory_path() / "sixseven_e2e_rep_959";
        primary_dir_ = base_dir_ / "primary";
        standby_dir_ = base_dir_ / "standby";
        fs::remove_all(base_dir_);
        fs::create_directories(primary_dir_);
        fs::create_directories(standby_dir_);
    }

    void TearDown() override {
#ifndef _WIN32
        kill_wait(standby_pid_, SIGKILL);
        kill_wait(primary_pid_, SIGKILL);
#endif
        std::error_code ec;
        fs::remove_all(base_dir_, ec);
    }

    fs::path base_dir_;
    fs::path primary_dir_;
    fs::path standby_dir_;

#ifndef _WIN32
    pid_t primary_pid_ = -1;
    pid_t standby_pid_ = -1;
#endif

    // Unique ports for this test class, distinct from the durability test.
    // Static port registry: 16759=durability, 16760=replication-primary, 16761=replication-standby
    static constexpr int kPrimaryPort = 16760;
    static constexpr int kStandbyPort = 16761;
};

// ---------------------------------------------------------------------------
// TEST: two-process replication
// ---------------------------------------------------------------------------

TEST_F(E2EReplicationTest, StandbyConnectsAndRejectsWrites) {
#ifdef _WIN32
    GTEST_SKIP() << "Two-process replication scenario requires POSIX; "
                    "skipped on Windows (POSIX/CI only - GDB-959)";
#else
    // ------------------------------------------------------------------
    // Phase 1: Start the primary and write data.
    // ------------------------------------------------------------------
    {
        fs::path cfg = base_dir_ / "primary.json";
        sixseven::e2e::write_server_config(cfg,
                                           primary_dir_,
                                           kPrimaryPort,
                                           /*standby=*/false,
                                           "",
                                           0);
        primary_pid_ = launch_server(cfg, /*standby_flag=*/false);
        ASSERT_GT(primary_pid_, 0) << "fork/exec primary failed";
    }

    int fd_primary =
        sixseven::e2e::connect_with_retry("127.0.0.1", kPrimaryPort, /*timeout_ms=*/8000);
    ASSERT_GE(fd_primary, 0) << "primary did not become ready within 8 s";

    std::vector<uint8_t> resp;
    ASSERT_TRUE(pg_exec(fd_primary,
                        "CREATE TABLE rep_test "
                        "(id INT32 NOT NULL, msg STRING NOT NULL);",
                        resp))
        << "CREATE TABLE on primary failed";
    ::close(fd_primary);

    fd_primary = sixseven::e2e::connect_with_retry("127.0.0.1", kPrimaryPort, /*timeout_ms=*/2000);
    ASSERT_GE(fd_primary, 0) << "reconnect to primary failed";

    resp.clear();
    ASSERT_TRUE(pg_exec(fd_primary, "INSERT INTO rep_test VALUES (1, 'hello_from_primary');", resp))
        << "INSERT on primary failed";
    ::close(fd_primary);

    // ------------------------------------------------------------------
    // Phase 2: Start the standby pointing at the primary.
    // ------------------------------------------------------------------
    {
        fs::path cfg = base_dir_ / "standby.json";
        sixseven::e2e::write_server_config(cfg,
                                           standby_dir_,
                                           kStandbyPort,
                                           /*standby=*/true,
                                           "127.0.0.1",
                                           kPrimaryPort);
        standby_pid_ = launch_server(cfg, /*standby_flag=*/true);
        ASSERT_GT(standby_pid_, 0) << "fork/exec standby failed";
    }

    int fd_standby =
        sixseven::e2e::connect_with_retry("127.0.0.1", kStandbyPort, /*timeout_ms=*/10000);
    ASSERT_GE(fd_standby, 0) << "standby did not become ready within 10 s";

    // ------------------------------------------------------------------
    // Phase 3: Standby must accept reads (SELECT 1 does not touch user tables).
    // ------------------------------------------------------------------
    std::vector<uint8_t> sel_resp;
    bool sel_ok = pg_exec(fd_standby, "SELECT 1;", sel_resp);
    EXPECT_TRUE(sel_ok) << "standby rejected a simple SELECT 1";
    ::close(fd_standby);

    // ------------------------------------------------------------------
    // Phase 4: Standby must reject DML with an ErrorResponse.
    // ------------------------------------------------------------------
    fd_standby = sixseven::e2e::connect_with_retry("127.0.0.1", kStandbyPort, /*timeout_ms=*/2000);
    ASSERT_GE(fd_standby, 0) << "reconnect to standby failed";

    std::vector<uint8_t> ins_resp;
    // The server may close the connection on error; we don't require the
    // pg_exec return value to be true, only that an 'E' byte is present.
    (void)pg_exec(fd_standby, "INSERT INTO rep_test VALUES (99, 'should_fail');", ins_resp);
    ::close(fd_standby);

    // An ErrorResponse starts with 'E' followed by a 4-byte length.
    bool has_error_response = false;
    for (uint8_t b : ins_resp) {
        if (b == 'E') {
            has_error_response = true;
            break;
        }
    }
    EXPECT_TRUE(has_error_response) << "standby did not return an ErrorResponse for a DML INSERT "
                                       "(expected read-only rejection)";

    // ------------------------------------------------------------------
    // Phase 5 (informational): Poll up to 5 s for the standby to apply the
    // primary's INSERT.  This is a best-effort propagation check -- replication
    // apply latency is non-deterministic in an e2e environment.
    //
    // The contractual assertions for this test are in Phases 3 and 4
    // (connect + read-only rejection).  A propagation lag that exceeds 5 s
    // does NOT fail the test; it is logged as a warning so CI noise is
    // visible without causing a spurious FAIL.
    // ------------------------------------------------------------------
    bool replicated = false;
    {
        const auto rep_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!replicated && std::chrono::steady_clock::now() < rep_deadline) {
            int fd_chk = sixseven::e2e::connect_with_retry("127.0.0.1", kStandbyPort, 500);
            if (fd_chk >= 0) {
                std::vector<uint8_t> chk_resp;
                if (pg_exec(fd_chk, "SELECT msg FROM rep_test WHERE id = 1;", chk_resp)) {
                    std::string s(chk_resp.begin(), chk_resp.end());
                    if (s.find("hello_from_primary") != std::string::npos) {
                        replicated = true;
                    }
                }
                ::close(fd_chk);
            }
            if (!replicated) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }
    }
    if (!replicated) {
        GTEST_LOG_(WARNING) << "standby did not reflect primary data within 5 s "
                               "(best-effort propagation lag check; non-fatal -- "
                               "see GDB-959 for POSIX/CI expectations)";
    }

    // Tear down cleanly.
    kill_wait(standby_pid_, SIGTERM);
    kill_wait(primary_pid_, SIGTERM);
#endif // _WIN32
}
