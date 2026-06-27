// GDB-936: WalArchiveOptions (enabled, cleanup_policy, keep_last_n) honored end-to-end.
//
// Regression tests verifying:
// 1. RETENTION: KEEP_LAST_N bounds the archive to at most N segments (the old
//    bug: the archive grew unbounded because cleanup was never applied).
// 2. ENABLED GATE: archive_enabled=false -> start() no-op, no thread, no work.
// 3. KEEP_ALL: no segments are removed regardless of how many are archived.
// 4. CALLBACK WIRING: WalWriter::set_on_segment_rotated wired to a manager
//    -> a real rotation enqueues and archives the segment end-to-end.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Unique temp directory, cleaned up on destruction.
class TempDir {
public:
    TempDir(const std::string& label) {
        path_ = std::filesystem::temp_directory_path() /
                ("sixseven_qa_gdb936_" + label + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

/// Write a minimal non-empty file to simulate a WAL segment.
void write_fake_segment(const std::filesystem::path& dir, uint64_t seg_id) {
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg_id;
    std::filesystem::path p = dir / oss.str();
    std::ofstream f(p, std::ios::binary);
    // Must be non-empty (archive_segment rejects empty files).
    const char payload[] = "FAKE_WAL_SEGMENT_GDB936";
    f.write(payload, static_cast<std::streamsize>(sizeof(payload)));
    f.flush();
}

/// Poll list_archived_segments() until count reaches expected or timeout elapses.
/// Returns true if the expected count was reached.
bool poll_archived_count(WalArchiveManager& mgr,
                         size_t expected,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto list = mgr.list_archived_segments();
        if (list.has_value() && list->size() >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

/// Build a simple WalRecord for use with WalWriter.
WalRecord make_wal_record(const std::string& payload = "GDB936-test-payload") {
    WalRecord r;
    r.type = WalRecordType::INSERT;
    r.txn_id = 1;
    r.table_id = 42;
    r.data.assign(payload.begin(), payload.end());
    return r;
}

// ---------------------------------------------------------------------------
// Test 1: RETENTION - KEEP_LAST_N bounds the archive (core regression)
//
// Pre-fix: cleanup was never called after archive, so the archive grew
// without bound regardless of the cleanup_policy setting.
// ---------------------------------------------------------------------------

TEST(WalArchiveGDB936, RetentionKeepLastN_BoundsArchiveToN) {
    constexpr uint64_t keep_n = 3;
    constexpr uint64_t total_segments = 7; // Archive more than keep_n.

    TempDir wal_dir("wal_keepn");
    TempDir archive_dir("arc_keepn");

    // Create fake WAL segment files in wal_dir.
    for (uint64_t i = 1; i <= total_segments; ++i) {
        write_fake_segment(wal_dir.path(), i);
    }

    WalArchiveOptions opts;
    opts.enabled = true;
    opts.cleanup_policy = ArchiveCleanupPolicy::KEEP_LAST_N;
    opts.keep_last_n = keep_n;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());

    // Enqueue all segments asynchronously.
    for (uint64_t i = 1; i <= total_segments; ++i) {
        mgr.enqueue_segment(i, static_cast<lsn_t>(i * 100));
    }

    // Wait for all segments to be processed (total_segments enqueued, but
    // after cleanup, only keep_n survive in the archive).
    // We poll until the queue drains by calling stop() and checking the result.
    ASSERT_TRUE(mgr.stop().has_value());

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());

    // Core regression assertion: must not exceed keep_n.
    EXPECT_LE(list->size(), keep_n)
        << "Archive grew beyond keep_last_n=" << keep_n << " (pre-fix bug reproduced)";

    // The retained segments must be the NEWEST ones (highest IDs).
    if (!list->empty()) {
        // list is sorted ascending.
        uint64_t oldest_retained = list->front();
        uint64_t newest_retained = list->back();
        EXPECT_EQ(newest_retained, total_segments) << "The newest segment must always be retained";
        EXPECT_EQ(newest_retained - oldest_retained + 1, list->size())
            << "Retained segments should be contiguous (oldest N kept)";
    }
}

// ---------------------------------------------------------------------------
// Test 2: ENABLED GATE - archive_enabled=false -> no thread, no archive
// ---------------------------------------------------------------------------

TEST(WalArchiveGDB936, EnabledGate_DisabledManagerDoesNothing) {
    TempDir wal_dir("wal_disabled");
    TempDir archive_dir("arc_disabled");

    // Write a fake segment so enqueue_segment has something to archive IF it ran.
    write_fake_segment(wal_dir.path(), 1);

    WalArchiveOptions opts;
    opts.enabled = false; // THE DEFAULT.
    opts.cleanup_policy = ArchiveCleanupPolicy::KEEP_LAST_N;
    opts.keep_last_n = 2;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());

    // is_running() must be false: no thread was spawned.
    EXPECT_FALSE(mgr.is_running());

    // enqueue_segment is a no-op when not running.
    mgr.enqueue_segment(1, 100);

    // Give a brief moment (should be instant; just defensive).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Archive directory must remain empty.
    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 0u) << "Disabled manager must not archive anything";

    // stop() on a disabled (not-running) manager must succeed silently.
    ASSERT_TRUE(mgr.stop().has_value());
}

// ---------------------------------------------------------------------------
// Test 3: KEEP_ALL - no segments are removed
// ---------------------------------------------------------------------------

TEST(WalArchiveGDB936, KeepAll_NoSegmentsRemoved) {
    constexpr uint64_t total_segments = 5;

    TempDir wal_dir("wal_keepall");
    TempDir archive_dir("arc_keepall");

    for (uint64_t i = 1; i <= total_segments; ++i) {
        write_fake_segment(wal_dir.path(), i);
    }

    WalArchiveOptions opts;
    opts.enabled = true;
    opts.cleanup_policy = ArchiveCleanupPolicy::KEEP_ALL;
    opts.keep_last_n = 2; // keep_last_n is irrelevant for KEEP_ALL.

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());

    for (uint64_t i = 1; i <= total_segments; ++i) {
        mgr.enqueue_segment(i, static_cast<lsn_t>(i * 10));
    }

    ASSERT_TRUE(mgr.stop().has_value());

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), total_segments) << "KEEP_ALL must never remove any archived segment";
}

// ---------------------------------------------------------------------------
// Test 4: CALLBACK WIRING - WalWriter::set_on_segment_rotated -> manager
//
// Drives a real WalWriter with a small segment size so that appending records
// forces a rotation. The callback must enqueue the completed segment into the
// archive manager, which then archives it asynchronously. Proves the
// writer->manager wire (not just the manager in isolation).
// ---------------------------------------------------------------------------

TEST(WalArchiveGDB936, CallbackWiring_WalWriterRotationArchivesSegment) {
    TempDir wal_dir("wal_callback");
    TempDir archive_dir("arc_callback");

    // Use a tiny segment (256 bytes) so writes quickly trigger a rotation.
    WalWriterOptions wal_opts;
    wal_opts.segment_size = 256;
    wal_opts.enable_group_commit = false;

    WalWriter writer(wal_dir.path(), wal_opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions opts;
    opts.enabled = true;
    opts.cleanup_policy = ArchiveCleanupPolicy::KEEP_ALL;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), opts);
    ASSERT_TRUE(mgr.start().has_value());

    // Wire the rotation callback: completed segment -> enqueue in manager.
    writer.set_on_segment_rotated(
        [&mgr](uint64_t segment_id, lsn_t last_lsn) { mgr.enqueue_segment(segment_id, last_lsn); });

    // Write records until at least one rotation happens (segment fills up).
    const uint64_t seg_before = writer.current_segment_id();
    int written = 0;
    while (writer.current_segment_id() == seg_before && written < 200) {
        auto r = make_wal_record();
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
        ++written;
    }
    ASSERT_GT(writer.current_segment_id(), seg_before)
        << "Expected at least one segment rotation after writing " << written << " records";

    ASSERT_TRUE(writer.flush().has_value());

    // Poll until at least one segment is archived (async path).
    bool got_archive = poll_archived_count(mgr, 1);

    // Drain and stop.
    ASSERT_TRUE(mgr.stop().has_value());

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());

    // Either the poll found it or stop() drained the queue.
    EXPECT_GT(list->size(), 0u)
        << "Rotation callback -> enqueue_segment -> archive_segment pipeline did not fire. "
           "got_archive="
        << got_archive;

    ASSERT_TRUE(writer.close().has_value());
}

} // namespace
} // namespace sixseven
