#pragma once

#include "giodb/common/result.h"
#include "giodb/server/replication_connection.h"
#include "giodb/server/replication_message.h"
#include "giodb/server/replication_slot.h"
#include "giodb/server/sync_replication.h"
#include "giodb/storage/wal.h"
#include "giodb/storage/wal_archive.h"
#include "giodb/storage/wal_record.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace giodb {

/// Configuration for a WalSender.
struct WalSenderOptions {
    /// Keepalive interval.
    std::chrono::milliseconds keepalive_interval{10000};

    /// Sender timeout — disconnect if no standby status within this period.
    std::chrono::milliseconds sender_timeout{60000};

    /// Maximum WAL records per WalData batch.
    uint32_t max_batch_records = 1000;

    /// Maximum byte size of a single WalData payload.
    size_t max_batch_bytes = 1024 * 1024; // 1 MB
};

/// Streams WAL records to a single connected replica.
///
/// Lifecycle:
///   1. Construct with connection, WAL directory, and options.
///   2. Call start_streaming(start_lsn) to begin in a background thread.
///   3. WalSenderManager calls notify_new_wal() when new WAL is flushed.
///   4. Call stop() to terminate (or it stops on connection error / timeout).
///
/// Thread model: The streaming loop runs in its own thread. It blocks on
/// a condition variable waiting for new WAL notifications or the keepalive
/// timer. notify_new_wal() is an atomic store + CV signal — it never
/// blocks the primary's write path.
class WalSender {
public:
    /// Sender state machine.
    enum class State {
        CREATED,
        CATCHING_UP,
        STREAMING,
        STOPPED,
    };

    /// Construct a WalSender.
    /// @param connection   Replication connection (ownership transferred).
    /// @param wal_dir      Path to the WAL directory.
    /// @param archive_mgr  Archive manager for catch-up (may be nullptr).
    /// @param writer       Active WAL writer.
    /// @param options       Configuration.
    /// @param slot_mgr     Replication slot manager (may be nullptr).
    /// @param slot_name    Replication slot name (empty if no slot).
    /// @param sync_mgr     Synchronous replication manager (may be nullptr).
    WalSender(std::unique_ptr<ReplicationConnection> connection,
              std::filesystem::path wal_dir,
              WalArchiveManager* archive_mgr,
              WalWriter& writer,
              WalSenderOptions options = {},
              ReplicationSlotManager* slot_mgr = nullptr,
              std::string slot_name = {},
              SyncReplicationManager* sync_mgr = nullptr);
    ~WalSender();

    WalSender(const WalSender&) = delete;
    WalSender& operator=(const WalSender&) = delete;
    WalSender(WalSender&&) = delete;
    WalSender& operator=(WalSender&&) = delete;

    /// Start streaming from the given LSN. Spawns a background thread.
    [[nodiscard]] Result<void> start_streaming(lsn_t start_lsn);

    /// Signal that new WAL has been flushed up to flush_lsn.
    /// Non-blocking, safe to call from the write path.
    void notify_new_wal(lsn_t flush_lsn);

    /// Process an incoming StandbyStatus from the replica.
    void handle_standby_status(const StandbyStatusMessage& status);

    /// Process an incoming HotStandbyFeedback from the replica.
    void handle_hot_standby_feedback(const HotStandbyFeedbackMessage& feedback);

    /// Stop the sender. Signals the streaming loop and joins the thread.
    void stop();

    /// Return the current state.
    [[nodiscard]] State state() const;

    /// Return the replica's last reported received LSN.
    [[nodiscard]] lsn_t replica_received_lsn() const;

    /// Return the replica's last reported applied LSN.
    [[nodiscard]] lsn_t replica_applied_lsn() const;

    /// Return the replica's last reported flushed LSN.
    [[nodiscard]] lsn_t replica_flushed_lsn() const;

    /// Return the peer description from the connection.
    [[nodiscard]] std::string peer_description() const;

    /// Return the replica's reported xmin.
    [[nodiscard]] uint64_t replica_xmin() const;

    /// Return the last LSN that was sent to the replica.
    [[nodiscard]] lsn_t sent_lsn() const;

private:
    void streaming_loop();
    [[nodiscard]] Result<void> run_catchup(lsn_t start_lsn);
    [[nodiscard]] Result<void> run_streaming();
    [[nodiscard]] Result<lsn_t> send_wal_batch(WalReader& reader, lsn_t up_to_lsn);
    [[nodiscard]] Result<void> send_keepalive(bool reply_requested);
    [[nodiscard]] Result<void> process_replica_messages();
    [[nodiscard]] Result<void> send_message(std::span<const uint8_t> data);

    static uint64_t now_us();

    std::unique_ptr<ReplicationConnection> connection_;
    std::filesystem::path wal_dir_;
    WalArchiveManager* archive_mgr_;
    WalWriter& writer_;
    WalSenderOptions options_;
    ReplicationSlotManager* slot_mgr_;
    std::string slot_name_;
    SyncReplicationManager* sync_mgr_;

    std::thread stream_thread_;
    std::atomic<State> state_{State::CREATED};
    std::atomic<bool> stop_requested_{false};

    std::mutex notify_mutex_;
    std::condition_variable notify_cv_;
    std::atomic<lsn_t> notified_lsn_{0};

    std::atomic<lsn_t> sent_lsn_{0};

    mutable std::mutex status_mutex_;
    lsn_t replica_received_lsn_{invalid_lsn};
    lsn_t replica_applied_lsn_{invalid_lsn};
    lsn_t replica_flushed_lsn_{invalid_lsn};
    uint64_t replica_xmin_{0};
    std::chrono::steady_clock::time_point last_status_time_;
};

} // namespace giodb
