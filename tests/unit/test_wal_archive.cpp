#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

/// Helper: temporary archive directory with automatic cleanup.
class TempArchiveDir {
public:
    TempArchiveDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("wal_archive_test_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempArchiveDir() { std::filesystem::remove_all(path_); }

    TempArchiveDir(const TempArchiveDir&) = delete;
    TempArchiveDir& operator=(const TempArchiveDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

/// Create a simple WAL record for testing.
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

/// Write WAL records to create a completed segment (force rotation).
/// Returns the segment ID of the completed segment and its last LSN.
struct CompletedSegment {
    uint64_t segment_id;
    lsn_t last_lsn;
};

CompletedSegment write_and_rotate(WalWriter& writer) {
    // Write enough data to trigger a segment rotation.
    // With a 256-byte segment, a few records will overflow.
    lsn_t last_lsn = 0;
    uint64_t seg_before = writer.current_segment_id();

    while (writer.current_segment_id() == seg_before) {
        auto r = make_record(WalRecordType::INSERT, 1, 10, "payload-for-rotation");
        auto lsn = writer.append(r);
        if (lsn.has_value()) {
            last_lsn = *lsn;
        }
    }

    // The completed segment is the one before the current.
    return {seg_before, last_lsn - 1};
}

// -- Policy parsing -----------------------------------------------------------

TEST(WalArchivePolicy, ParseCleanupPolicy) {
    EXPECT_EQ(parse_cleanup_policy("keep_all"), ArchiveCleanupPolicy::KEEP_ALL);
    EXPECT_EQ(parse_cleanup_policy("keep_last_n"), ArchiveCleanupPolicy::KEEP_LAST_N);
    EXPECT_EQ(parse_cleanup_policy("keep_since_lsn"), ArchiveCleanupPolicy::KEEP_SINCE_LSN);
    // Unknown strings default to KEEP_ALL.
    EXPECT_EQ(parse_cleanup_policy("unknown"), ArchiveCleanupPolicy::KEEP_ALL);
    EXPECT_EQ(parse_cleanup_policy(""), ArchiveCleanupPolicy::KEEP_ALL);
}

TEST(WalArchivePolicy, CleanupPolicyName) {
    EXPECT_STREQ(cleanup_policy_name(ArchiveCleanupPolicy::KEEP_ALL), "keep_all");
    EXPECT_STREQ(cleanup_policy_name(ArchiveCleanupPolicy::KEEP_LAST_N), "keep_last_n");
    EXPECT_STREQ(cleanup_policy_name(ArchiveCleanupPolicy::KEEP_SINCE_LSN), "keep_since_lsn");
}

// -- Start / Stop -------------------------------------------------------------

TEST(WalArchiveManager, StartCreatesDirectoryAndStops) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    // Remove archive dir so start() must create it.
    std::filesystem::remove_all(archive_dir.path());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    auto result = mgr.start();
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(std::filesystem::exists(archive_dir.path()));
    EXPECT_TRUE(mgr.is_running());

    auto stop_result = mgr.stop();
    ASSERT_TRUE(stop_result.has_value()) << stop_result.error().message;
    EXPECT_FALSE(mgr.is_running());
}

TEST(WalArchiveManager, StartTwiceFails) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    ASSERT_TRUE(mgr.start().has_value());

    auto result = mgr.start();
    EXPECT_FALSE(result.has_value());

    ASSERT_TRUE(mgr.stop().has_value());
}

TEST(WalArchiveManager, StopIdempotent) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    ASSERT_TRUE(mgr.start().has_value());
    ASSERT_TRUE(mgr.stop().has_value());
    ASSERT_TRUE(mgr.stop().has_value()); // Second stop is OK.
}

// GDB-1197: the original version of this test enqueued nothing and ended
// with a bare SUCCEED(), so it could not distinguish a destructor that
// correctly drains/joins the background archival thread from one that leaks
// it (UB that frequently doesn't crash in a non-sanitizer build). This
// version enqueues real segments (mirroring StopDrainsQueue), lets the
// manager go out of scope, and then -- after destruction -- opens a second,
// independent WalArchiveManager over the same archive directory and asserts
// the segments are actually present on disk. That is only possible if the
// destructor synchronously drained the queue and joined the worker before
// returning; a leaked/detached thread racing the destructor would not
// reliably have finished the copy+fsync+rename by the time we re-open here.
TEST(WalArchiveManager, DestructorStopsCleanly) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveOptions archive_opts;
    archive_opts.enabled = true;

    std::vector<CompletedSegment> segments;
    {
        WalArchiveManager mgr(wal_dir.path(), archive_dir.path(), archive_opts);
        ASSERT_TRUE(mgr.start().has_value());
        ASSERT_TRUE(mgr.is_running());

        for (int i = 0; i < 3; ++i) {
            auto seg = write_and_rotate(writer);
            segments.push_back(seg);
            mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
        }

        // Destructor runs here (no explicit stop()) and must drain the
        // queue and join the background thread before returning.
    }

    // A fresh manager reading the same archive directory should see all
    // segments archived -- proving the destructor's drain/join completed
    // synchronously rather than leaking the thread.
    WalArchiveManager verify_mgr(wal_dir.path(), archive_dir.path());
    auto list = verify_mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value()) << list.error().message;
    ASSERT_EQ(list->size(), 3u);
    for (const auto& seg : segments) {
        auto archived = verify_mgr.get_archived_segment(seg.segment_id);
        ASSERT_TRUE(archived.has_value()) << archived.error().message;
        EXPECT_TRUE(std::filesystem::exists(*archived));
    }

    ASSERT_TRUE(writer.close().has_value());
}

// -- Synchronous archive_segment ----------------------------------------------

TEST(WalArchiveManager, ArchiveSegmentCopiesFile) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    // Write a completed WAL segment.
    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Archive synchronously (no need to start the background thread).
    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // The archived segment should exist in the archive directory.
    auto archived = mgr.get_archived_segment(completed.segment_id);
    ASSERT_TRUE(archived.has_value()) << archived.error().message;
    EXPECT_TRUE(std::filesystem::exists(*archived));

    // The last archived LSN should be updated.
    EXPECT_EQ(mgr.last_archived_lsn(), completed.last_lsn);

    ASSERT_TRUE(writer.close().has_value());
}

// GDB-782: a failed metadata sidecar write must fail archive_segment and undo
// the copy, instead of silently leaving a segment that cleanup_before() will
// skip forever.
TEST(WalArchiveManager, ArchiveSegmentFailsWhenMetaSidecarUnwritable) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Occupy the sidecar path with a directory so the ofstream open fails.
    auto seg_name = "wal_" + std::string(6 - std::to_string(completed.segment_id).length(), '0') +
                    std::to_string(completed.segment_id);
    auto meta_path = archive_dir.path() / (seg_name + ".meta");
    std::filesystem::create_directories(meta_path);

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);

    // The copied segment must have been removed so a retry can re-archive it.
    EXPECT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value());

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalArchiveManager, ArchiveSegmentCopiesIntactFile) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    // Write a completed WAL segment.
    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Archive and verify the file is intact.
    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify archived file size matches source.
    auto src_path = wal_dir.path() /
                    ("wal_" + std::string(6 - std::to_string(completed.segment_id).length(), '0') +
                     std::to_string(completed.segment_id));
    auto archived_path = *mgr.get_archived_segment(completed.segment_id);
    EXPECT_EQ(std::filesystem::file_size(src_path), std::filesystem::file_size(archived_path));

    ASSERT_TRUE(writer.close().has_value());
}

// GDB-1194: the checksum-verification failure path (CRC32C mismatch between
// source and archived copy) was never actually exercised anywhere in the
// suite -- the old ArchiveSegmentVerifiesChecksum test only compared file
// sizes, which a same-size-but-corrupted copy would also pass. This test
// uses a test-only hook (set_test_post_copy_hook) to deterministically
// corrupt the archived copy's bytes -- same size, different content -- right
// after the file copy and before the destination checksum is computed, so
// archive_segment() must detect the CRC32C mismatch and fail.
TEST(WalArchiveManager, ArchiveSegmentFailsOnCorruptedCopy) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Flip a byte in the destination file right after the copy completes,
    // without changing its size, so a size-only check would still pass but
    // the CRC32C comparison must catch it.
    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        std::fstream f(dst, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(0);
        char byte = 0;
        f.read(&byte, 1);
        f.seekp(0);
        char flipped = static_cast<char>(~byte);
        f.write(&flipped, 1);
    });

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(result.has_value())
        << "archive_segment should fail when the copied bytes differ from the source "
           "even though the file size matches";
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);

    // The corrupted copy must be removed so a retry can re-archive cleanly.
    EXPECT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value());

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalArchiveManager, ArchiveNonExistentSegmentFails) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    auto result = mgr.archive_segment(999, 100);
    EXPECT_FALSE(result.has_value());
}

// -- list_archived_segments ---------------------------------------------------

TEST(WalArchiveManager, ListArchivedSegmentsEmpty) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_TRUE(list->empty());
}

TEST(WalArchiveManager, ListArchivedSegmentsInOrder) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    // Write enough data to create multiple completed segments.
    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Force 3 rotations.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 3; ++i) {
        segments.push_back(write_and_rotate(writer));
    }

    // Archive all.
    for (const auto& seg : segments) {
        auto result = mgr.archive_segment(seg.segment_id, seg.last_lsn);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    ASSERT_EQ(list->size(), 3u);

    // Should be in ascending order.
    for (size_t i = 1; i < list->size(); ++i) {
        EXPECT_LT((*list)[i - 1], (*list)[i]);
    }

    ASSERT_TRUE(writer.close().has_value());
}

// -- get_archived_segment -----------------------------------------------------

TEST(WalArchiveManager, GetArchivedSegmentNotFound) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    auto result = mgr.get_archived_segment(42);
    EXPECT_FALSE(result.has_value());
}

// -- Cleanup policies ---------------------------------------------------------

TEST(WalArchiveManager, CleanupBeforeLsn) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Create and archive 3 segments.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 3; ++i) {
        segments.push_back(write_and_rotate(writer));
    }

    for (const auto& seg : segments) {
        auto result = mgr.archive_segment(seg.segment_id, seg.last_lsn);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    // Verify all 3 exist.
    auto list_before = mgr.list_archived_segments();
    ASSERT_TRUE(list_before.has_value());
    ASSERT_EQ(list_before->size(), 3u);

    // Cleanup segments with last_lsn < segments[2].last_lsn.
    // This should remove segment[0] and segment[1].
    auto cleanup_result = mgr.cleanup_before(segments[2].last_lsn);
    ASSERT_TRUE(cleanup_result.has_value()) << cleanup_result.error().message;

    auto list_after = mgr.list_archived_segments();
    ASSERT_TRUE(list_after.has_value());
    EXPECT_EQ(list_after->size(), 1u);
    EXPECT_EQ((*list_after)[0], segments[2].segment_id);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalArchiveManager, CleanupKeepLastN) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Create and archive 5 segments.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 5; ++i) {
        segments.push_back(write_and_rotate(writer));
    }

    for (const auto& seg : segments) {
        ASSERT_TRUE(mgr.archive_segment(seg.segment_id, seg.last_lsn).has_value());
    }

    // Keep only last 2.
    auto result = mgr.cleanup_keep_last_n(2);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 2u);

    // Should have the last two segments.
    EXPECT_EQ((*list)[0], segments[3].segment_id);
    EXPECT_EQ((*list)[1], segments[4].segment_id);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalArchiveManager, CleanupKeepLastNWhenFewerThanN) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Archive 2 segments.
    auto seg = write_and_rotate(writer);
    ASSERT_TRUE(mgr.archive_segment(seg.segment_id, seg.last_lsn).has_value());
    auto seg2 = write_and_rotate(writer);
    ASSERT_TRUE(mgr.archive_segment(seg2.segment_id, seg2.last_lsn).has_value());

    // Keep last 10 — should not remove anything.
    auto result = mgr.cleanup_keep_last_n(10);
    ASSERT_TRUE(result.has_value());

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 2u);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Async archival (enqueue_segment) -----------------------------------------

TEST(WalArchiveManager, AsyncArchival) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    ASSERT_TRUE(mgr.start().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    // Enqueue for async archival.
    mgr.enqueue_segment(completed.segment_id, completed.last_lsn);

    // Wait for the background thread to process.
    for (int i = 0; i < 100; ++i) {
        if (mgr.last_archived_lsn() >= completed.last_lsn) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(mgr.last_archived_lsn(), completed.last_lsn);

    auto archived = mgr.get_archived_segment(completed.segment_id);
    ASSERT_TRUE(archived.has_value()) << archived.error().message;
    EXPECT_TRUE(std::filesystem::exists(*archived));

    ASSERT_TRUE(mgr.stop().has_value());
    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalArchiveManager, AsyncArchivalMultipleSegments) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    ASSERT_TRUE(mgr.start().has_value());

    // Create and enqueue multiple segments.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 4; ++i) {
        auto seg = write_and_rotate(writer);
        segments.push_back(seg);
        mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
    }

    lsn_t expected_last = segments.back().last_lsn;

    // Wait for all to be archived.
    for (int i = 0; i < 200; ++i) {
        if (mgr.last_archived_lsn() >= expected_last) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(mgr.last_archived_lsn(), expected_last);

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 4u);

    ASSERT_TRUE(mgr.stop().has_value());
    ASSERT_TRUE(writer.close().has_value());
}

// -- WalWriter integration with on_segment_rotated callback -------------------

TEST(WalArchiveManager, WalWriterCallbackTriggersArchival) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    ASSERT_TRUE(mgr.start().has_value());

    // Register the archive callback.
    writer.set_on_segment_rotated(
        [&mgr](uint64_t segment_id, lsn_t last_lsn) { mgr.enqueue_segment(segment_id, last_lsn); });

    ASSERT_TRUE(writer.open().has_value());

    // Write enough data to trigger at least 2 rotations.
    for (int i = 0; i < 40; ++i) {
        auto r = make_record(WalRecordType::INSERT, 1, 10, "archival-integration-test-data");
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
    }

    ASSERT_TRUE(writer.flush().has_value());

    // Wait for archives to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // There should be archived segments.
    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_GT(list->size(), 0u);

    // last_archived_lsn should be > 0.
    EXPECT_GT(mgr.last_archived_lsn(), 0u);

    ASSERT_TRUE(mgr.stop().has_value());
    ASSERT_TRUE(writer.close().has_value());
}

// -- last_archived_lsn tracking -----------------------------------------------

TEST(WalArchiveManager, LastArchivedLsnMonotonicallyIncreases) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    EXPECT_EQ(mgr.last_archived_lsn(), 0u);

    auto seg1 = write_and_rotate(writer);
    ASSERT_TRUE(mgr.archive_segment(seg1.segment_id, seg1.last_lsn).has_value());
    lsn_t lsn1 = mgr.last_archived_lsn();
    EXPECT_EQ(lsn1, seg1.last_lsn);

    auto seg2 = write_and_rotate(writer);
    ASSERT_TRUE(mgr.archive_segment(seg2.segment_id, seg2.last_lsn).has_value());
    lsn_t lsn2 = mgr.last_archived_lsn();
    EXPECT_GT(lsn2, lsn1);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Metadata sidecar ---------------------------------------------------------

TEST(WalArchiveManager, MetadataSidecarWritten) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    auto seg = write_and_rotate(writer);
    ASSERT_TRUE(mgr.archive_segment(seg.segment_id, seg.last_lsn).has_value());

    // Check that the metadata sidecar exists.
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg.segment_id << ".meta";
    auto meta_path = archive_dir.path() / oss.str();
    ASSERT_TRUE(std::filesystem::exists(meta_path));

    // Read and verify the metadata.
    std::ifstream meta_file(meta_path);
    lsn_t stored_lsn = 0;
    meta_file >> stored_lsn;
    EXPECT_EQ(stored_lsn, seg.last_lsn);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Edge cases ---------------------------------------------------------------

TEST(WalArchiveManager, CleanupOnEmptyArchive) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Both cleanup operations should succeed on empty archive.
    EXPECT_TRUE(mgr.cleanup_before(100).has_value());
    EXPECT_TRUE(mgr.cleanup_keep_last_n(5).has_value());
}

TEST(WalArchiveManager, StopDrainsQueue) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    ASSERT_TRUE(mgr.start().has_value());

    // Enqueue several segments quickly then stop.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 3; ++i) {
        auto seg = write_and_rotate(writer);
        segments.push_back(seg);
        mgr.enqueue_segment(seg.segment_id, seg.last_lsn);
    }

    // Stop should drain remaining items.
    ASSERT_TRUE(mgr.stop().has_value());

    // All segments should be archived after stop.
    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 3u);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Retention LSN provider integration (GDB-196) ----------------------------

TEST(WalArchiveManager, CleanupRespectsRetentionProvider) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Archive a few segments.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 3; ++i) {
        auto seg = write_and_rotate(writer);
        segments.push_back(seg);
        ASSERT_TRUE(mgr.archive_segment(seg.segment_id, seg.last_lsn).has_value());
    }

    auto list = mgr.list_archived_segments();
    ASSERT_TRUE(list.has_value());
    ASSERT_EQ(list->size(), 3u);

    // Set a retention provider that returns the 2nd segment's last LSN.
    // This should prevent cleanup of segments 1 and 2, even if the caller
    // requests cleanup of everything.
    lsn_t retention_lsn = segments[1].last_lsn;
    mgr.set_retention_lsn_provider([retention_lsn]() { return retention_lsn; });

    // Request cleanup of everything (high LSN), but the provider clamps it.
    ASSERT_TRUE(mgr.cleanup_before(999999).has_value());

    // Segment 0 should be removed (last_lsn < retention_lsn),
    // but segments 1 and 2 should be retained -- exactly, not just "at least
    // 2 remain". A weaker size-only check would pass even if cleanup were a
    // no-op (all 3 retained) or if it wrongly removed segment 1 or 2 instead
    // of segment 0 (the GDB-196 replication-slot-retention failure mode).
    auto remaining = mgr.list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 2u);
    EXPECT_EQ((*remaining)[0], segments[1].segment_id);
    EXPECT_EQ((*remaining)[1], segments[2].segment_id);

    ASSERT_TRUE(writer.close().has_value());
}

// Fault-injection sibling: confirms the strengthened assertions above would
// actually catch a no-op cleanup. If cleanup_before() is never invoked, all 3
// archived segments remain -- demonstrating that the exact-size/exact-id
// checks in CleanupRespectsRetentionProvider discriminate a no-op from a
// correctly-clamped cleanup (the old EXPECT_GE(size, 2) would not have).
TEST(WalArchiveManager, CleanupBeforeNoOpLeavesAllSegments) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 3; ++i) {
        auto seg = write_and_rotate(writer);
        segments.push_back(seg);
        ASSERT_TRUE(mgr.archive_segment(seg.segment_id, seg.last_lsn).has_value());
    }

    // Deliberately do NOT call cleanup_before(). This simulates the no-op
    // failure mode: all 3 segments remain, which must NOT satisfy the exact
    // expectations asserted in CleanupRespectsRetentionProvider above.
    auto remaining = mgr.list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 3u);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(WalArchiveManager, CleanupWithoutProviderRemovesAll) {
    TempWalDir wal_dir;
    TempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Archive segments.
    std::vector<CompletedSegment> segments;
    for (int i = 0; i < 3; ++i) {
        auto seg = write_and_rotate(writer);
        segments.push_back(seg);
        ASSERT_TRUE(mgr.archive_segment(seg.segment_id, seg.last_lsn).has_value());
    }

    // No retention provider — cleanup should remove all below the LSN.
    ASSERT_TRUE(mgr.cleanup_before(999999).has_value());

    auto remaining = mgr.list_archived_segments();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 0u);

    ASSERT_TRUE(writer.close().has_value());
}

} // namespace
} // namespace sixseven
