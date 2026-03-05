#pragma once

#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace sixseven {
namespace test {

/// Temporary WAL directory with automatic cleanup.
/// Shared between test_wal.cpp and test_wal_recovery.cpp.
class TempWalDir {
public:
    TempWalDir() {
        path_ = std::filesystem::temp_directory_path() / ("wal_test_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempWalDir() { std::filesystem::remove_all(path_); }

    // Non-copyable, non-movable.
    TempWalDir(const TempWalDir&) = delete;
    TempWalDir& operator=(const TempWalDir&) = delete;
    TempWalDir(TempWalDir&&) = delete;
    TempWalDir& operator=(TempWalDir&&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

/// Create writer options with group commit disabled for deterministic tests.
inline WalWriterOptions test_wal_opts() {
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    return opts;
}

/// Write a simple committed transaction to the WAL: BEGIN, INSERT, COMMIT.
inline void write_committed_txn(WalWriter& writer,
                                txn_id_t txn_id,
                                uint32_t table_id,
                                const std::string& data) {
    WalRecord begin;
    begin.type = WalRecordType::BEGIN;
    begin.txn_id = txn_id;
    ASSERT_TRUE(writer.append(begin).has_value());

    WalRecord insert;
    insert.type = WalRecordType::INSERT;
    insert.txn_id = txn_id;
    insert.table_id = table_id;
    insert.data.assign(data.begin(), data.end());
    ASSERT_TRUE(writer.append(insert).has_value());

    WalRecord commit;
    commit.type = WalRecordType::COMMIT;
    commit.txn_id = txn_id;
    ASSERT_TRUE(writer.append(commit).has_value());
}

/// Write an aborted transaction to the WAL: BEGIN, INSERT, ABORT.
inline void
write_aborted_txn(WalWriter& writer, txn_id_t txn_id, uint32_t table_id, const std::string& data) {
    WalRecord begin;
    begin.type = WalRecordType::BEGIN;
    begin.txn_id = txn_id;
    ASSERT_TRUE(writer.append(begin).has_value());

    WalRecord insert;
    insert.type = WalRecordType::INSERT;
    insert.txn_id = txn_id;
    insert.table_id = table_id;
    insert.data.assign(data.begin(), data.end());
    ASSERT_TRUE(writer.append(insert).has_value());

    WalRecord abort;
    abort.type = WalRecordType::ABORT;
    abort.txn_id = txn_id;
    ASSERT_TRUE(writer.append(abort).has_value());
}

} // namespace test
} // namespace sixseven
