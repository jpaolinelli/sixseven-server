#pragma once

#include "giodb/common/result.h"
#include "giodb/server/replication_connection.h"
#include "giodb/server/wal_sender.h"
#include "giodb/storage/wal.h"
#include "giodb/storage/wal_archive.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace giodb {

/// Manages all active WalSender instances on the primary.
///
/// Responsibilities:
/// - Accept new replication connections (within max_wal_senders limit).
/// - Broadcast notify_new_wal() to all active senders.
/// - Remove stopped senders.
class WalSenderManager {
public:
    WalSenderManager(std::filesystem::path wal_dir,
                     WalArchiveManager* archive_mgr,
                     WalWriter& writer,
                     uint32_t max_wal_senders = 10,
                     WalSenderOptions sender_options = {});
    ~WalSenderManager();

    WalSenderManager(const WalSenderManager&) = delete;
    WalSenderManager& operator=(const WalSenderManager&) = delete;

    /// Accept a new replication connection and start streaming from start_lsn.
    /// Returns REPLICATION_ERROR if max_wal_senders would be exceeded.
    [[nodiscard]] Result<void> accept_connection(std::unique_ptr<ReplicationConnection> connection,
                                                 lsn_t start_lsn);

    /// Broadcast a WAL flush notification to all active senders.
    void notify_new_wal(lsn_t flush_lsn);

    /// Remove stopped senders (housekeeping).
    void cleanup_stopped();

    /// Stop all active senders and clean up.
    void stop_all();

    /// Return the number of currently active senders.
    [[nodiscard]] size_t active_sender_count() const;

    /// Return the maximum allowed senders.
    [[nodiscard]] uint32_t max_wal_senders() const;

    /// Status of a single sender for monitoring.
    struct SenderStatus {
        std::string peer;
        WalSender::State state;
        lsn_t received_lsn;
        lsn_t applied_lsn;
        lsn_t flushed_lsn;
    };

    /// Get status of all active senders.
    [[nodiscard]] std::vector<SenderStatus> get_sender_statuses() const;

private:
    std::filesystem::path wal_dir_;
    WalArchiveManager* archive_mgr_;
    WalWriter& writer_;
    uint32_t max_wal_senders_;
    WalSenderOptions sender_options_;

    mutable std::mutex senders_mutex_;
    std::vector<std::unique_ptr<WalSender>> senders_;
};

} // namespace giodb
