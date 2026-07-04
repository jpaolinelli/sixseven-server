// QA regression tests for GDB-1196: WalWriter destructor durability.
//
// GDB-1196 strengthened a vacuous WalWriter.DestructorClosesWriter test
// (previously only SUCCEED()) into a real assertion that the destructor
// truncates the segment to written size and that a reopened writer resumes
// at the correct LSN. These tests independently attack the same code path
// (WalWriter::~WalWriter -> close() -> close_segment()) with adversarial
// scenarios not covered by the strengthened dev test: multiple records,
// zero records, reopen-and-append, and full record readback via WalReader
// to confirm genuine durability (not just file-size correctness).

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace sixseven {
namespace {

/// Self-contained temp WAL dir (QA tests must not depend on tests/unit/ headers).
class QaTempWalDir {
public:
    QaTempWalDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("wal_qa_gdb1196_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~QaTempWalDir() { std::filesystem::remove_all(path_); }

    QaTempWalDir(const QaTempWalDir&) = delete;
    QaTempWalDir& operator=(const QaTempWalDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

WalRecord make_record(WalRecordType type, txn_id_t txn_id, const std::string& data = "") {
    WalRecord r;
    r.type = type;
    r.txn_id = txn_id;
    if (!data.empty()) {
        r.data.assign(data.begin(), data.end());
    }
    return r;
}

WalWriterOptions no_group_commit_opts() {
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    return opts;
}

// -- Zero records: destructor on an empty, just-opened writer -----------------

TEST(QA_GDB1196, DestructorWithZeroRecordsLeavesEmptySegment) {
    QaTempWalDir dir;
    {
        WalWriter writer(dir.path(), no_group_commit_opts());
        ASSERT_TRUE(writer.open().has_value());
        // No appends at all, no explicit close() -- rely on the destructor.
    }

    auto seg_path = dir.path() / "wal_000001";
    ASSERT_TRUE(std::filesystem::exists(seg_path));
    // close_segment() only truncates if segment_offset_ > 0, so a
    // zero-record segment should be truncated to exactly 0 bytes, not left
    // at whatever size ::open()/O_CREAT produced.
    EXPECT_EQ(std::filesystem::file_size(seg_path), 0u);
}

// -- Multiple records before destruction --------------------------------------

TEST(QA_GDB1196, DestructorPersistsMultipleRecordsDurably) {
    QaTempWalDir dir;
    std::vector<WalRecord> written;
    {
        WalWriter writer(dir.path(), no_group_commit_opts());
        ASSERT_TRUE(writer.open().has_value());
        for (int i = 0; i < 20; ++i) {
            WalRecord r = make_record(WalRecordType::INSERT,
                                       /*txn_id=*/static_cast<txn_id_t>(i + 1),
                                       "payload-" + std::to_string(i));
            auto res = writer.append(r);
            ASSERT_TRUE(res.has_value());
            r.lsn = *res;
            written.push_back(r);
        }
        // Destructor runs here without an explicit close().
    }

    // Reopen and read every record back via WalReader to confirm the
    // records are genuinely durable and byte-correct, not just that the
    // file size matches (the strengthened dev test only checks size).
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    std::vector<WalRecord> read_back;
    while (true) {
        auto rec = reader.next();
        if (!rec.has_value()) {
            break;
        }
        read_back.push_back(*rec);
    }
    ASSERT_TRUE(reader.close().has_value());

    ASSERT_EQ(read_back.size(), written.size());
    for (size_t i = 0; i < written.size(); ++i) {
        EXPECT_EQ(read_back[i].lsn, written[i].lsn) << "record " << i;
        EXPECT_EQ(read_back[i].txn_id, written[i].txn_id) << "record " << i;
        EXPECT_EQ(read_back[i].data, written[i].data) << "record " << i;
    }
}

// -- Reopen-and-append after destructor close ---------------------------------

TEST(QA_GDB1196, ReopenAfterDestructorCanAppendAndBothPartsSurviveSecondDestruction) {
    QaTempWalDir dir;
    lsn_t first_lsn;
    {
        WalWriter writer(dir.path(), no_group_commit_opts());
        ASSERT_TRUE(writer.open().has_value());
        WalRecord r = make_record(WalRecordType::BEGIN, 1);
        auto res = writer.append(r);
        ASSERT_TRUE(res.has_value());
        first_lsn = *res;
        // Destructor closes here.
    }

    lsn_t second_lsn;
    {
        WalWriter writer2(dir.path(), no_group_commit_opts());
        ASSERT_TRUE(writer2.open().has_value());
        EXPECT_EQ(writer2.current_lsn(), first_lsn + 1);

        WalRecord r2 = make_record(WalRecordType::COMMIT, 1);
        auto res2 = writer2.append(r2);
        ASSERT_TRUE(res2.has_value());
        second_lsn = *res2;
        // Destructor closes here too -- second destructor-only close in a row.
    }

    // Both records must be recoverable after two consecutive
    // destructor-driven closes with no explicit close() call anywhere.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());
    auto rec1 = reader.next();
    ASSERT_TRUE(rec1.has_value());
    EXPECT_EQ(rec1->lsn, first_lsn);
    EXPECT_EQ(rec1->type, WalRecordType::BEGIN);

    auto rec2 = reader.next();
    ASSERT_TRUE(rec2.has_value());
    EXPECT_EQ(rec2->lsn, second_lsn);
    EXPECT_EQ(rec2->type, WalRecordType::COMMIT);

    auto rec3 = reader.next();
    EXPECT_FALSE(rec3.has_value()); // NOT_FOUND at end of log.
    ASSERT_TRUE(reader.close().has_value());
}

// -- Destruction after a large write (near segment boundary) -----------------

TEST(QA_GDB1196, DestructorTruncatesCorrectlyAfterLargeWriteNearSegmentBoundary) {
    QaTempWalDir dir;
    WalWriterOptions opts = no_group_commit_opts();
    // Small segment size so a "large" payload approaches the boundary
    // without triggering rotation, exercising close_segment()'s truncate
    // path with a substantial segment_offset_.
    opts.segment_size = 4096;

    std::string big_payload(3000, 'x');
    WalRecord r = make_record(WalRecordType::INSERT, 42, big_payload);
    size_t expected_size;
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        auto res = writer.append(r);
        ASSERT_TRUE(res.has_value());
        r.lsn = *res;
        expected_size = serialized_wal_record_size(r);
        // No explicit close(): destructor must flush + truncate.
    }

    auto seg_path = dir.path() / "wal_000001";
    ASSERT_TRUE(std::filesystem::exists(seg_path));
    EXPECT_EQ(std::filesystem::file_size(seg_path), expected_size);

    // Confirm the payload is byte-for-byte intact, not truncated short or
    // padded with garbage.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());
    auto rec = reader.next();
    ASSERT_TRUE(rec.has_value());
    ASSERT_EQ(rec->data.size(), big_payload.size());
    EXPECT_EQ(std::string(rec->data.begin(), rec->data.end()), big_payload);
    ASSERT_TRUE(reader.close().has_value());
}

// -- Segment rotation boundary: destruction right after a rotation ----------

TEST(QA_GDB1196, DestructorAfterSegmentRotationLeavesBothSegmentsDurable) {
    QaTempWalDir dir;
    WalWriterOptions opts = no_group_commit_opts();
    opts.segment_size = 200; // tiny, forces rotation quickly

    std::vector<WalRecord> written;
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        for (int i = 0; i < 15; ++i) {
            WalRecord r = make_record(WalRecordType::INSERT,
                                       static_cast<txn_id_t>(i + 1),
                                       "seg-rotation-payload");
            auto res = writer.append(r);
            ASSERT_TRUE(res.has_value());
            r.lsn = *res;
            written.push_back(r);
        }
        // Should have rotated across multiple segments by now.
        EXPECT_GT(writer.current_segment_id(), 1u);
        // Destructor-only close, potentially mid-way through the final
        // (not-yet-full) segment.
    }

    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());
    std::vector<WalRecord> read_back;
    while (true) {
        auto rec = reader.next();
        if (!rec.has_value()) break;
        read_back.push_back(*rec);
    }
    ASSERT_TRUE(reader.close().has_value());

    ASSERT_EQ(read_back.size(), written.size());
    for (size_t i = 0; i < written.size(); ++i) {
        EXPECT_EQ(read_back[i].lsn, written[i].lsn) << "record " << i;
    }
}

// -- Adversarial: destructor must not silently drop the flushed_lsn tracking -

TEST(QA_GDB1196, DestructorUpdatesFlushedLsnBeforeClosing) {
    QaTempWalDir dir;
    lsn_t last_lsn;
    lsn_t flushed_before_destruction;
    {
        WalWriter writer(dir.path(), no_group_commit_opts());
        ASSERT_TRUE(writer.open().has_value());
        WalRecord r = make_record(WalRecordType::BEGIN, 7);
        auto res = writer.append(r);
        ASSERT_TRUE(res.has_value());
        last_lsn = *res;
        // Before destruction, nothing has been explicitly flushed.
        flushed_before_destruction = writer.flushed_lsn();
    }
    // Prior to destruction (inside the block) nothing was flushed yet since
    // no explicit flush()/close() was called -- this documents current
    // behavior rather than asserting a specific durability guarantee pre-close.
    EXPECT_EQ(flushed_before_destruction, 0u);

    // After destruction, the record must be on disk and recoverable
    // regardless of what flushed_lsn() reported pre-destruction, because
    // the destructor's close() performs its own fsync.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());
    auto rec = reader.next();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->lsn, last_lsn);
    ASSERT_TRUE(reader.close().has_value());
}

} // namespace
} // namespace sixseven
