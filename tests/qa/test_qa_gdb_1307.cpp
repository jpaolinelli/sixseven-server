// QA regression tests for GDB-1307.
//
// GDB-1307 fixed 6 WalArchiveManager unit tests that were constructing the
// manager via the 2-arg constructor overload, which defaults
// WalArchiveOptions.enabled = false. With archiving disabled, start() is a
// silent no-op (no thread spawned, is_running() stays false), so those tests
// never actually exercised the archival path.
//
// This QA suite is adversarial toward the *now-actually-running* archiving
// path: lifecycle correctness (start/stop/restart, double start, double
// stop, destructor-without-stop), the disabled no-op path (to guard the
// exact regression class this ticket fixed), and stress/hang scenarios that
// were previously masked because start() never spun up a real thread.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

/// Temporary archive directory with automatic cleanup (mirrors the dev-test
/// helper in tests/unit/test_wal_archive.cpp — duplicated here because QA
/// tests must not modify or depend on developer-owned test fixtures).
class QaTempArchiveDir {
public:
    QaTempArchiveDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("wal_archive_qa_gdb1307_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~QaTempArchiveDir() { std::filesystem::remove_all(path_); }

    QaTempArchiveDir(const QaTempArchiveDir&) = delete;
    QaTempArchiveDir& operator=(const QaTempArchiveDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

WalRecord make_record(WalRecordType type,
                      txn_id_t txn_id = 1,
                      uint32_t table_id = 0,
                      const std::string& data = "") {
    WalRecord r;
    r.type = type;
    r.txn_id = txn_id;
    r.table_id = table_id;
    if (!data.empty()) {
        r.data.assign(data.begin(), data.end());
    }
    return r;
}

struct CompletedSegment {
    uint64_t segment_id;
    lsn_t last_lsn;
};

CompletedSegment write_and_rotate(WalWriter& writer) {
    lsn_t last_lsn = 0;
    uint64_t seg_before = writer.current_segment_id();
    while (writer.current_segment_id() == seg_before) {
        auto r = make_record(WalRecordType::INSERT, 1, 10, "payload-for-rotation");
        auto lsn = writer.append(r);
        if (lsn.has_value()) {
            last_lsn = *lsn;
        }
    }
    return {seg_before, last_lsn - 1};
}

// -- Regression guard: the exact bug class GDB-1307 fixed ---------------------

TEST(QA_GDB1307_WalArchive, TwoArgConstructorDefaultsDisabledAndStartIsNoOp) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    // 2-arg constructor: options defaults to WalArchiveOptions{} => enabled=false.
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    auto result = mgr.start();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // start() must be a genuine no-op: no thread, is_running() stays false,
    // and the archive directory is not created.
    EXPECT_FALSE(mgr.is_running());
    EXPECT_FALSE(std::filesystem::exists(archive_dir.path() / "wal_000001"));

    // enqueue_segment() on a disabled manager must not crash and must not
    // populate the archive.
    mgr.enqueue_segment(1, 100);
    auto listed = mgr.list_archived_segments();
    ASSERT_TRUE(listed.has_value());
    EXPECT_TRUE(listed->empty());

    // stop() on a manager that never actually started must be safe.
    auto stop_result = mgr.stop();
    EXPECT_TRUE(stop_result.has_value());
}

TEST(QA_GDB1307_WalArchive, ExplicitEnabledFalseAlsoNoOps) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalArchiveOptions opts;
    opts.enabled = false;
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);

    ASSERT_TRUE(mgr.start().has_value());
    EXPECT_FALSE(mgr.is_running());
}

// -- Lifecycle correctness on the now-actually-enabled path -------------------

TEST(QA_GDB1307_WalArchive, DoubleStopIsIdempotentWhenEnabled) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalArchiveOptions opts;
    opts.enabled = true;
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());
    ASSERT_TRUE(mgr.is_running());

    ASSERT_TRUE(mgr.stop().has_value());
    EXPECT_FALSE(mgr.is_running());

    // Calling stop() again on an already-stopped manager must not hang,
    // crash, or double-join the thread.
    auto second_stop = mgr.stop();
    EXPECT_TRUE(second_stop.has_value());
}

TEST(QA_GDB1307_WalArchive, RestartAfterStopWorks) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalArchiveOptions opts;
    opts.enabled = true;
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);

    ASSERT_TRUE(mgr.start().has_value());
    ASSERT_TRUE(mgr.is_running());
    ASSERT_TRUE(mgr.stop().has_value());
    ASSERT_FALSE(mgr.is_running());

    // Restarting after a clean stop must succeed and actually run again.
    auto restart = mgr.start();
    ASSERT_TRUE(restart.has_value()) << restart.error().message;
    EXPECT_TRUE(mgr.is_running());

    ASSERT_TRUE(mgr.stop().has_value());
}

TEST(QA_GDB1307_WalArchive, StartTwiceWithoutStopFailsAndFirstInstanceKeepsRunning) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalArchiveOptions opts;
    opts.enabled = true;
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);

    ASSERT_TRUE(mgr.start().has_value());
    ASSERT_TRUE(mgr.is_running());

    auto second_start = mgr.start();
    ASSERT_FALSE(second_start.has_value());
    EXPECT_EQ(second_start.error().code, StatusCode::INVALID_ARGUMENT);

    // The manager must still be functional after the rejected second start.
    EXPECT_TRUE(mgr.is_running());
    ASSERT_TRUE(mgr.stop().has_value());
}

TEST(QA_GDB1307_WalArchive, DestructorWithoutExplicitStopDoesNotHang) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    // Write a real segment so there is archival work in flight when the
    // manager is destroyed without an explicit stop() call.
    WalWriterOptions wal_opts;
    wal_opts.segment_size = 256;
    wal_opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), wal_opts);
    ASSERT_TRUE(writer.open().has_value());

    // Run the whole scenario on a bounded-time future so a real hang (e.g. a
    // destructor that fails to join the background thread) fails the test
    // instead of hanging the whole binary.
    auto fut = std::async(std::launch::async, [&]() {
        WalArchiveOptions opts;
        opts.enabled = true;
        {
            WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
            if (!mgr.start().has_value()) {
                return false;
            }

            auto completed = write_and_rotate(writer);
            mgr.enqueue_segment(completed.segment_id, completed.last_lsn);
            // Deliberately do NOT call stop() — rely on the destructor.
        }
        return true;
    });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "WalArchiveManager destructor hung without explicit stop()";
    EXPECT_TRUE(fut.get());

    (void)writer.close();
}

// -- Stress: previously-masked archival path under load -----------------------

TEST(QA_GDB1307_WalArchive, ManySegmentsArchivedAndDrainedUnderStop) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions wal_opts;
    wal_opts.segment_size = 256;
    wal_opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), wal_opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions opts;
    opts.enabled = true;
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());

    constexpr int kSegments = 20;
    for (int i = 0; i < kSegments; ++i) {
        auto completed = write_and_rotate(writer);
        mgr.enqueue_segment(completed.segment_id, completed.last_lsn);
    }

    // stop() must drain the full queue synchronously.
    ASSERT_TRUE(mgr.stop().has_value());

    auto listed = mgr.list_archived_segments();
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed->size(), static_cast<size_t>(kSegments));

    (void)writer.close();
}

TEST(QA_GDB1307_WalArchive, EnqueueAfterStopIsSilentlyDropped) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalArchiveOptions opts;
    opts.enabled = true;
    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());
    ASSERT_TRUE(mgr.stop().has_value());

    // enqueue_segment() after stop() must be a safe no-op (is_running() is
    // false), not a crash or a queue growth that's never drained.
    mgr.enqueue_segment(999, 12345);

    auto listed = mgr.list_archived_segments();
    ASSERT_TRUE(listed.has_value());
    EXPECT_TRUE(listed->empty());
}

} // namespace
} // namespace sixseven
