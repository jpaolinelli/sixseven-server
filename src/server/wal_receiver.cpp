#include "giodb/server/wal_receiver.h"

#include "giodb/common/logging.h"
#include "giodb/storage/wal_record.h"

#include <algorithm>
#include <chrono>

namespace giodb {

WalReceiver::WalReceiver(ConnectionFactory factory,
                         std::filesystem::path wal_dir,
                         RecoveryHandler& handler,
                         WalReceiverOptions options)
    : factory_(std::move(factory)), wal_dir_(std::move(wal_dir)), handler_(handler),
      options_(options) {}

WalReceiver::~WalReceiver() {
    stop();
}

Result<void> WalReceiver::start(const std::string& primary_host, uint16_t primary_port) {
    if (receiver_thread_.joinable()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WAL receiver already started");
    }

    primary_host_ = primary_host;
    primary_port_ = primary_port;
    stop_requested_.store(false);

    // Open local WAL writer for persisting received records.
    WalWriterOptions writer_opts;
    writer_opts.enable_group_commit = false; // We flush explicitly after each batch.
    local_writer_ = std::make_unique<WalWriter>(wal_dir_, writer_opts);
    auto open_result = local_writer_->open();
    if (!open_result) {
        return make_error(open_result.error().code,
                          "failed to open local WAL writer: " + open_result.error().message);
    }

    receiver_thread_ = std::thread([this] { receiver_loop(); });
    return ok();
}

void WalReceiver::stop() {
    stop_requested_.store(true);
    {
        std::lock_guard lock(stop_mutex_);
        stop_cv_.notify_all();
    }

    if (connection_ && connection_->is_open()) {
        connection_->close();
    }

    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }

    is_streaming_.store(false);

    if (local_writer_) {
        auto close_result = local_writer_->close();
        if (!close_result) {
            GIODB_LOG_WARN("failed to close local WAL writer: {}", close_result.error().message);
        }
        local_writer_.reset();
    }
}

ReplicationState WalReceiver::get_state() const {
    std::lock_guard lock(state_mutex_);
    ReplicationState state;
    state.received_lsn = received_lsn_;
    state.applied_lsn = applied_lsn_;
    state.flushed_lsn = flushed_lsn_;
    state.is_streaming = is_streaming_.load();

    // Compute approximate lag from the received vs applied difference.
    if (received_lsn_ != invalid_lsn && applied_lsn_ != invalid_lsn &&
        received_lsn_ > applied_lsn_) {
        // Rough estimate: each LSN unit is one record.
        state.replication_lag =
            std::chrono::milliseconds(static_cast<int64_t>(received_lsn_ - applied_lsn_));
    }

    return state;
}

// -- Private ------------------------------------------------------------------

void WalReceiver::receiver_loop() {
    auto retry_interval = options_.retry_interval;

    while (!stop_requested_.load()) {
        // Attempt to connect.
        auto connect_result = connect();
        if (!connect_result) {
            GIODB_LOG_WARN("WAL receiver: connection to {}:{} failed: {}",
                           primary_host_,
                           primary_port_,
                           connect_result.error().message);

            // Wait with exponential backoff before retrying.
            {
                std::unique_lock lock(stop_mutex_);
                stop_cv_.wait_for(lock, retry_interval, [this] { return stop_requested_.load(); });
            }
            if (stop_requested_.load()) {
                break;
            }

            // Exponential backoff: double the interval up to the max.
            retry_interval = std::min(retry_interval * 2, options_.max_retry_interval);
            continue;
        }

        // Reset retry interval on successful connection.
        retry_interval = options_.retry_interval;
        is_streaming_.store(true);
        GIODB_LOG_INFO("WAL receiver: connected to {}:{}", primary_host_, primary_port_);

        // Process messages until connection drops or stop requested.
        auto process_result = process_messages();
        is_streaming_.store(false);

        if (stop_requested_.load()) {
            break;
        }

        if (!process_result) {
            GIODB_LOG_WARN("WAL receiver: streaming interrupted: {}",
                           process_result.error().message);
        }

        // Close the connection before retrying.
        if (connection_ && connection_->is_open()) {
            connection_->close();
        }
        connection_.reset();
    }
}

Result<void> WalReceiver::connect() {
    auto conn_result = factory_(primary_host_, primary_port_);
    if (!conn_result) {
        return make_error(conn_result.error().code, conn_result.error().message);
    }
    connection_ = std::move(*conn_result);
    return ok();
}

Result<void> WalReceiver::process_messages() {
    while (!stop_requested_.load()) {
        // Receive data from the primary.
        auto data = connection_->receive(64 * 1024, options_.receive_timeout);
        if (!data) {
            return make_error(data.error().code, "receive failed: " + data.error().message);
        }

        if (data->empty()) {
            // Timeout with no data — send status and continue.
            auto status_result = send_status();
            if (!status_result) {
                return make_error(status_result.error().code, status_result.error().message);
            }
            continue;
        }

        // Parse the message type.
        auto type = peek_message_type(*data);
        if (!type) {
            return make_error(type.error().code,
                              "failed to parse message type: " + type.error().message);
        }

        switch (*type) {
        case ReplicationMessageType::WAL_DATA: {
            auto msg = deserialize_wal_data(*data);
            if (!msg) {
                return make_error(msg.error().code,
                                  "failed to deserialize WAL data: " + msg.error().message);
            }
            auto result = handle_wal_data(*msg);
            if (!result) {
                return make_error(result.error().code, result.error().message);
            }
            break;
        }

        case ReplicationMessageType::KEEPALIVE: {
            auto msg = deserialize_keepalive(*data);
            if (!msg) {
                return make_error(msg.error().code,
                                  "failed to deserialize keepalive: " + msg.error().message);
            }
            auto result = handle_keepalive(*msg);
            if (!result) {
                return make_error(result.error().code, result.error().message);
            }
            break;
        }

        case ReplicationMessageType::CATCHUP_COMPLETE: {
            GIODB_LOG_INFO("WAL receiver: catch-up complete, now streaming in real-time");
            break;
        }

        case ReplicationMessageType::SEGMENT_START: {
            // Informational — no action required on standby.
            break;
        }

        default:
            GIODB_LOG_WARN("WAL receiver: unexpected message type: {}", static_cast<int>(*type));
            break;
        }
    }

    return ok();
}

Result<void> WalReceiver::handle_wal_data(const WalDataMessage& msg) {
    // Deserialize individual WAL records from the batch payload.
    std::vector<WalRecord> records;
    records.reserve(msg.record_count);

    size_t offset = 0;
    while (offset < msg.data.size()) {
        auto remaining =
            std::span<const uint8_t>(msg.data.data() + offset, msg.data.size() - offset);
        if (remaining.size() < sizeof(uint32_t)) {
            break;
        }

        // Read record length from first 4 bytes.
        uint32_t record_len = 0;
        std::memcpy(&record_len, remaining.data(), sizeof(uint32_t));
        size_t total_size = sizeof(uint32_t) + record_len;

        if (remaining.size() < total_size) {
            break;
        }

        auto record = deserialize_wal_record(remaining.subspan(0, total_size));
        if (!record) {
            GIODB_LOG_WARN("WAL receiver: failed to deserialize record at offset {}: {}",
                           offset,
                           record.error().message);
            break;
        }

        records.push_back(std::move(*record));
        offset += total_size;
    }

    if (records.empty()) {
        return ok();
    }

    // Write to local WAL.
    auto write_result = write_local_wal(records);
    if (!write_result) {
        return make_error(write_result.error().code, write_result.error().message);
    }

    // Update received LSN.
    {
        std::lock_guard lock(state_mutex_);
        received_lsn_ = msg.end_lsn;
    }

    // Replay the records.
    auto replay_result = replay_records(records);
    if (!replay_result) {
        return make_error(replay_result.error().code, replay_result.error().message);
    }

    // Send status update to primary.
    return send_status();
}

Result<void> WalReceiver::handle_keepalive(const KeepaliveMessage& msg) {
    // Update received LSN from the keepalive.
    {
        std::lock_guard lock(state_mutex_);
        if (msg.current_lsn > received_lsn_ || received_lsn_ == invalid_lsn) {
            received_lsn_ = msg.current_lsn;
        }
    }

    // Send status if reply requested.
    if (msg.reply_requested) {
        return send_status();
    }
    return ok();
}

Result<void> WalReceiver::send_status() {
    if (!connection_ || !connection_->is_open()) {
        return ok();
    }

    StandbyStatusMessage status;
    {
        std::lock_guard lock(state_mutex_);
        status.received_lsn = received_lsn_;
        status.applied_lsn = applied_lsn_;
        status.flushed_lsn = flushed_lsn_;
    }
    status.timestamp_us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());

    auto bytes = serialize_standby_status(status);
    return connection_->send(bytes);
}

Result<void> WalReceiver::write_local_wal(const std::vector<WalRecord>& records) {
    if (!local_writer_) {
        return ok();
    }

    for (const auto& record : records) {
        auto mutable_record = record;
        auto result = local_writer_->append(mutable_record);
        if (!result) {
            return make_error(result.error().code,
                              "failed to write local WAL record: " + result.error().message);
        }
    }

    auto flush_result = local_writer_->flush();
    if (!flush_result) {
        return make_error(flush_result.error().code,
                          "failed to flush local WAL: " + flush_result.error().message);
    }

    {
        std::lock_guard lock(state_mutex_);
        flushed_lsn_ = local_writer_->flushed_lsn();
    }

    return ok();
}

Result<void> WalReceiver::replay_records(const std::vector<WalRecord>& records) {
    for (const auto& record : records) {
        // Only replay data-modifying records.
        switch (record.type) {
        case WalRecordType::INSERT:
        case WalRecordType::UPDATE:
        case WalRecordType::DELETE:
        case WalRecordType::PAGE_SPLIT:
        case WalRecordType::CREATE_TABLE:
        case WalRecordType::DROP_TABLE: {
            auto result = handler_.redo(record);
            if (!result) {
                GIODB_LOG_WARN("WAL receiver: redo failed for record LSN {}: {}",
                               record.lsn,
                               result.error().message);
                // Continue replaying — redo must be idempotent.
            }
            break;
        }
        default:
            // Transaction-level records (BEGIN, COMMIT, ABORT, CHECKPOINT)
            // are not data-modifying and are skipped during continuous replay.
            break;
        }
    }

    // Update applied LSN.
    if (!records.empty()) {
        std::lock_guard lock(state_mutex_);
        applied_lsn_ = records.back().lsn;
    }

    return ok();
}

} // namespace giodb
