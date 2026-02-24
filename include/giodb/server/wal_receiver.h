#pragma once

#include "giodb/common/result.h"
#include "giodb/server/replication_connection.h"
#include "giodb/server/replication_message.h"
#include "giodb/storage/wal.h"
#include "giodb/storage/wal_record.h"
#include "giodb/storage/wal_recovery.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace giodb {

/// Current replication state of the standby.
struct ReplicationState {
    lsn_t received_lsn = invalid_lsn; ///< Last LSN received from primary.
    lsn_t applied_lsn = invalid_lsn;  ///< Last LSN applied to local storage.
    lsn_t flushed_lsn = invalid_lsn;  ///< Last LSN flushed to disk.
    bool is_streaming = false;        ///< True if actively receiving.
    std::chrono::milliseconds replication_lag{0};
};

/// Configuration for the WalReceiver.
struct WalReceiverOptions {
    /// Initial retry interval for reconnection.
    std::chrono::milliseconds retry_interval{5000};

    /// Maximum retry interval (exponential backoff cap).
    std::chrono::milliseconds max_retry_interval{60000};

    /// Timeout for receiving messages from the primary.
    std::chrono::milliseconds receive_timeout{30000};
};

/// Factory for creating connections to the primary server.
/// Abstracted to allow in-memory testing without real TCP.
using ConnectionFactory = std::function<Result<std::unique_ptr<ReplicationConnection>>(
    const std::string& host, uint16_t port)>;

/// Receives WAL from the primary and replays it continuously on the standby.
///
/// Lifecycle:
///   1. Construct with connection factory, WAL dir, recovery handler, and options.
///   2. Call start() to begin receiving in a background thread.
///   3. WAL records are received, written locally, and replayed via the handler.
///   4. Call stop() to terminate (or it reconnects on connection errors).
///
/// Reconnection: If the connection to the primary drops, the receiver retries
/// with exponential backoff. During disconnection, the standby serves queries
/// from its last applied state (stale reads).
class WalReceiver {
public:
    /// Construct a WalReceiver.
    /// @param factory        Factory to create connections to the primary.
    /// @param wal_dir        Path to the local WAL directory.
    /// @param handler        Recovery handler for redo operations.
    /// @param options        Configuration.
    WalReceiver(ConnectionFactory factory,
                std::filesystem::path wal_dir,
                RecoveryHandler& handler,
                WalReceiverOptions options = {});
    ~WalReceiver();

    WalReceiver(const WalReceiver&) = delete;
    WalReceiver& operator=(const WalReceiver&) = delete;
    WalReceiver(WalReceiver&&) = delete;
    WalReceiver& operator=(WalReceiver&&) = delete;

    /// Connect to primary and start receiving WAL.
    /// @param primary_host  Hostname/IP of the primary.
    /// @param primary_port  Port of the primary.
    [[nodiscard]] Result<void> start(const std::string& primary_host, uint16_t primary_port);

    /// Stop receiving (for shutdown or promotion).
    void stop();

    /// Get current replication state.
    [[nodiscard]] ReplicationState get_state() const;

private:
    /// Main receiver loop (runs in background thread).
    void receiver_loop();

    /// Attempt to connect to the primary.
    [[nodiscard]] Result<void> connect();

    /// Process incoming messages from the primary.
    [[nodiscard]] Result<void> process_messages();

    /// Handle a WAL data batch from the primary.
    [[nodiscard]] Result<void> handle_wal_data(const WalDataMessage& msg);

    /// Handle a keepalive from the primary.
    [[nodiscard]] Result<void> handle_keepalive(const KeepaliveMessage& msg);

    /// Send a standby status report to the primary.
    [[nodiscard]] Result<void> send_status();

    /// Write received WAL records to the local WAL directory.
    [[nodiscard]] Result<void> write_local_wal(const std::vector<WalRecord>& records);

    /// Replay WAL records using the recovery handler.
    [[nodiscard]] Result<void> replay_records(const std::vector<WalRecord>& records);

    ConnectionFactory factory_;
    std::filesystem::path wal_dir_;
    RecoveryHandler& handler_;
    WalReceiverOptions options_;

    std::string primary_host_;
    uint16_t primary_port_ = 0;

    std::unique_ptr<ReplicationConnection> connection_;

    std::thread receiver_thread_;
    std::atomic<bool> stop_requested_{false};
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    mutable std::mutex state_mutex_;
    lsn_t received_lsn_{invalid_lsn};
    lsn_t applied_lsn_{invalid_lsn};
    lsn_t flushed_lsn_{invalid_lsn};
    std::atomic<bool> is_streaming_{false};

    std::unique_ptr<WalWriter> local_writer_;
};

} // namespace giodb
