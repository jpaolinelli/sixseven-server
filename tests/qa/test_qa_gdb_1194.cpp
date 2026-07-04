// QA adversarial tests for GDB-1194: WalArchiveManager::copy_and_verify CRC32C
// verification path.
//
// The dev suite (tests/unit/test_wal_archive.cpp) now has
// ArchiveSegmentFailsOnCorruptedCopy, which exercises a single-bit flip at
// byte 0 via the test-only post-copy hook. This QA file goes further:
// corruption at the CRC boundary (last byte), truncation, extension, a
// multi-byte scattered mutation, multi-segment archives where only one
// segment is corrupted, repeated archive_segment calls after a corruption
// failure (retry semantics), and confirmation that an intact copy succeeds
// and leaves no stray artifacts on failure.

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_archive.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

class QaTempArchiveDir {
public:
    QaTempArchiveDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("qa_wal_archive_gdb1194_" + std::to_string(counter_++));
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

std::filesystem::path segment_file_name(const std::filesystem::path& dir, uint64_t seg_id) {
    std::string num = std::to_string(seg_id);
    std::string padded = std::string(6 - num.length(), '0') + num;
    return dir / ("wal_" + padded);
}

// -- Boundary corruption: last byte (CRC boundary) ---------------------------

TEST(QA_GDB1194, ArchiveSegmentFailsOnLastByteCorruption) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        auto size = std::filesystem::file_size(dst);
        ASSERT_GT(size, 0u);
        std::fstream f(dst, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekg(static_cast<std::streamoff>(size) - 1);
        char byte = 0;
        f.read(&byte, 1);
        f.seekp(static_cast<std::streamoff>(size) - 1);
        char flipped = static_cast<char>(~byte);
        f.write(&flipped, 1);
    });

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(result.has_value())
        << "corruption at the final byte (CRC boundary) must be detected";
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
    EXPECT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value())
        << "bad archived copy must not be left behind after last-byte corruption";

    ASSERT_TRUE(writer.close().has_value());
}

// -- Scattered multi-byte mutation (not just a single bit) -------------------

TEST(QA_GDB1194, ArchiveSegmentFailsOnScatteredMultiByteCorruption) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        auto size = std::filesystem::file_size(dst);
        ASSERT_GT(size, 4u);
        std::fstream f(dst, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        // Flip bytes scattered across the file: start, quarter, middle, end.
        std::vector<std::streamoff> offsets = {
            0,
            static_cast<std::streamoff>(size / 4),
            static_cast<std::streamoff>(size / 2),
            static_cast<std::streamoff>(size - 1),
        };
        for (auto off : offsets) {
            f.seekg(off);
            char byte = 0;
            f.read(&byte, 1);
            f.seekp(off);
            char flipped = static_cast<char>(byte ^ 0xFF);
            f.write(&flipped, 1);
        }
    });

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
    EXPECT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value());

    ASSERT_TRUE(writer.close().has_value());
}

// -- Truncation of the archived copy (shrinks size, would fail a size check
// too, but must ALSO fail the checksum path with the right error code) -----

TEST(QA_GDB1194, ArchiveSegmentFailsOnTruncatedCopy) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        auto size = std::filesystem::file_size(dst);
        ASSERT_GT(size, 1u);
        std::filesystem::resize_file(dst, size - 1);
    });

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
    EXPECT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value())
        << "truncated archived copy must be removed, not left as a partial file";

    ASSERT_TRUE(writer.close().has_value());
}

// -- Extension of the archived copy (grows size) -----------------------------

TEST(QA_GDB1194, ArchiveSegmentFailsOnExtendedCopy) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        std::ofstream f(dst, std::ios::app | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        char extra = 'X';
        f.write(&extra, 1);
    });

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
    EXPECT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value())
        << "extended archived copy must be removed, not left as a corrupted file";

    ASSERT_TRUE(writer.close().has_value());
}

// -- Multi-segment archive: only one of several segments is corrupted -------
// A regression here (e.g. the hook or the check accidentally applying
// globally, or a previous segment's failure state leaking into the next)
// would be a critical data-integrity bug.

TEST(QA_GDB1194, MultiSegmentArchiveOnlyCorruptedSegmentFails) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto seg1 = write_and_rotate(writer);
    auto seg2 = write_and_rotate(writer);
    auto seg3 = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    ASSERT_NE(seg1.segment_id, seg2.segment_id);
    ASSERT_NE(seg2.segment_id, seg3.segment_id);

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    // Archive segment 1 and 3 cleanly (no hook installed yet).
    auto r1 = mgr.archive_segment(seg1.segment_id, seg1.last_lsn);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    // Now install a corruption hook and archive segment 2 -- only this one
    // should fail.
    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        std::fstream f(dst, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        char byte = 0;
        f.seekg(0);
        f.read(&byte, 1);
        f.seekp(0);
        char flipped = static_cast<char>(~byte);
        f.write(&flipped, 1);
    });
    auto r2 = mgr.archive_segment(seg2.segment_id, seg2.last_lsn);
    ASSERT_FALSE(r2.has_value()) << "segment 2 was corrupted post-copy and must fail";
    EXPECT_EQ(r2.error().code, StatusCode::IO_ERROR);

    // Clear the hook before archiving segment 3 -- it must succeed cleanly.
    mgr.set_test_post_copy_hook(nullptr);
    auto r3 = mgr.archive_segment(seg3.segment_id, seg3.last_lsn);
    ASSERT_TRUE(r3.has_value()) << r3.error().message;

    // Segment 1 and 3 present and intact; segment 2 must be absent.
    EXPECT_TRUE(mgr.get_archived_segment(seg1.segment_id).has_value());
    EXPECT_FALSE(mgr.get_archived_segment(seg2.segment_id).has_value());
    EXPECT_TRUE(mgr.get_archived_segment(seg3.segment_id).has_value());

    auto archived_list = mgr.list_archived_segments();
    ASSERT_TRUE(archived_list.has_value());
    for (auto id : *archived_list) {
        EXPECT_NE(id, seg2.segment_id)
            << "corrupted segment 2 must never appear in the archived list";
    }

    ASSERT_TRUE(writer.close().has_value());
}

// -- Retry after corruption failure: a subsequent clean archive_segment call
// for the SAME segment id must succeed once the hook is removed. This
// verifies the cleanup left no stale/partial state blocking a retry.

TEST(QA_GDB1194, RetryAfterCorruptionFailureSucceeds) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());

    mgr.set_test_post_copy_hook([](const std::filesystem::path& dst) {
        std::fstream f(dst, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        char byte = 0;
        f.read(&byte, 1);
        f.seekp(0);
        char flipped = static_cast<char>(~byte);
        f.write(&flipped, 1);
    });

    auto first = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_FALSE(first.has_value());
    ASSERT_FALSE(mgr.get_archived_segment(completed.segment_id).has_value());

    // Remove the hook and retry -- should now succeed cleanly.
    mgr.set_test_post_copy_hook(nullptr);
    auto second = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_TRUE(mgr.get_archived_segment(completed.segment_id).has_value());

    // Verify the retried copy is byte-identical to the source (not just
    // present) using an independent check.
    auto src_path = segment_file_name(wal_dir.path(), completed.segment_id);
    auto archived_path = *mgr.get_archived_segment(completed.segment_id);
    ASSERT_TRUE(std::filesystem::exists(src_path));
    ASSERT_TRUE(std::filesystem::exists(archived_path));
    EXPECT_EQ(std::filesystem::file_size(src_path), std::filesystem::file_size(archived_path));

    std::ifstream src_f(src_path, std::ios::binary);
    std::ifstream dst_f(archived_path, std::ios::binary);
    std::vector<char> src_bytes((std::istreambuf_iterator<char>(src_f)),
                                std::istreambuf_iterator<char>());
    std::vector<char> dst_bytes((std::istreambuf_iterator<char>(dst_f)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(src_bytes, dst_bytes);

    ASSERT_TRUE(writer.close().has_value());
}

// -- Intact copy must verify successfully (positive control) ----------------

TEST(QA_GDB1194, IntactCopyVerifiesSuccessfullyWithNoHook) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    // Deliberately do NOT install a hook -- confirm the happy path holds.
    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto archived = mgr.get_archived_segment(completed.segment_id);
    ASSERT_TRUE(archived.has_value());
    EXPECT_TRUE(std::filesystem::exists(*archived));

    ASSERT_TRUE(writer.close().has_value());
}

// -- No-op hook (explicit empty std::function) must behave identically to no
// hook at all -- guards against the hook itself accidentally corrupting a
// clean copy or the empty-function check being wrong (e.g. `if (hook)` vs
// always invoking a default-constructed std::function).

TEST(QA_GDB1194, EmptyHookBehavesAsNoOp) {
    TempWalDir wal_dir;
    QaTempArchiveDir archive_dir;

    WalWriterOptions opts;
    opts.segment_size = 256;
    opts.enable_group_commit = false;
    WalWriter writer(wal_dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    auto completed = write_and_rotate(writer);
    ASSERT_TRUE(writer.flush().has_value());

    WalArchiveManager mgr(wal_dir.path(), archive_dir.path());
    mgr.set_test_post_copy_hook({}); // explicit empty std::function

    auto result = mgr.archive_segment(completed.segment_id, completed.last_lsn);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(mgr.get_archived_segment(completed.segment_id).has_value());

    ASSERT_TRUE(writer.close().has_value());
}

} // namespace
} // namespace sixseven
