#include "giodb/server/wal_sender.h"

#include "giodb/common/logging.h"

#include <chrono>

namespace giodb {

WalSender::WalSender(std::unique_ptr<ReplicationConnection> connection,
                     std::filesystem::path wal_dir,
                     WalArchiveManager* archive_mgr,
                     WalWriter& writer,
                     WalSenderOptions options,
                     ReplicationSlotManager* slot_mgr,
                     std::string slot_name)
    : connection_(std::move(connection)), wal_dir_(std::move(wal_dir)), archive_mgr_(archive_mgr),
      writer_(writer), options_(options), slot_mgr_(slot_mgr), slot_name_(std::move(slot_name)),
      last_status_time_(std::chrono::steady_clock::now()) {}

WalSender::~WalSender() {
    stop();
}

Result<void> WalSender::start_streaming(lsn_t start_lsn) {
    auto expected = State::CREATED;
    if (!state_.compare_exchange_strong(expected, State::CATCHING_UP)) {
        return make_error(StatusCode::REPLICATION_ERROR, "WalSender already started or stopped");
    }

    sent_lsn_.store(start_lsn > 0 ? start_lsn - 1 : 0);
    last_status_time_ = std::chrono::steady_clock::now();

    stream_thread_ = std::thread([this] { streaming_loop(); });

    GIODB_LOG_INFO("WalSender started streaming to {} from LSN {}",
                   connection_->peer_description(),
                   start_lsn);
    return ok();
}

void WalSender::notify_new_wal(lsn_t flush_lsn) {
    notified_lsn_.store(flush_lsn, std::memory_order_release);
    notify_cv_.notify_one();
}

void WalSender::handle_standby_status(const StandbyStatusMessage& status) {
    {
        std::lock_guard lock(status_mutex_);
        replica_received_lsn_ = status.received_lsn;
        replica_applied_lsn_ = status.applied_lsn;
        replica_flushed_lsn_ = status.flushed_lsn;
        last_status_time_ = std::chrono::steady_clock::now();
    }

    // Forward confirmed progress to the replication slot manager.
    if (slot_mgr_ != nullptr && !slot_name_.empty() && status.flushed_lsn != invalid_lsn) {
        slot_mgr_->update_confirmed_lsn(slot_name_, status.flushed_lsn);
    }
}

void WalSender::handle_hot_standby_feedback(const HotStandbyFeedbackMessage& feedback) {
    std::lock_guard lock(status_mutex_);
    replica_xmin_ = feedback.xmin;
    last_status_time_ = std::chrono::steady_clock::now();
}

void WalSender::stop() {
    stop_requested_.store(true);
    notify_cv_.notify_one();
    if (stream_thread_.joinable()) {
        stream_thread_.join();
    }
}

WalSender::State WalSender::state() const {
    return state_.load();
}

lsn_t WalSender::replica_received_lsn() const {
    std::lock_guard lock(status_mutex_);
    return replica_received_lsn_;
}

lsn_t WalSender::replica_applied_lsn() const {
    std::lock_guard lock(status_mutex_);
    return replica_applied_lsn_;
}

lsn_t WalSender::replica_flushed_lsn() const {
    std::lock_guard lock(status_mutex_);
    return replica_flushed_lsn_;
}

std::string WalSender::peer_description() const {
    return connection_->peer_description();
}

uint64_t WalSender::replica_xmin() const {
    std::lock_guard lock(status_mutex_);
    return replica_xmin_;
}

lsn_t WalSender::sent_lsn() const {
    return sent_lsn_.load();
}

uint64_t WalSender::now_us() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
    return static_cast<uint64_t>(us.count());
}

// -- Private: streaming loop --------------------------------------------------

void WalSender::streaming_loop() {
    // Compute start LSN from sent_lsn (which was set to start_lsn - 1).
    lsn_t start_lsn = sent_lsn_.load() + 1;

    // Activate the replication slot if configured.
    if (slot_mgr_ != nullptr && !slot_name_.empty()) {
        auto activate_result = slot_mgr_->activate_slot(slot_name_, start_lsn);
        if (!activate_result) {
            GIODB_LOG_WARN("WalSender failed to activate slot '{}': {}",
                           slot_name_,
                           activate_result.error().message);
        }
    }

    auto catchup_result = run_catchup(start_lsn);
    if (!catchup_result) {
        GIODB_LOG_ERROR("WalSender catch-up failed for {}: {}",
                        connection_->peer_description(),
                        catchup_result.error().message);
        state_.store(State::STOPPED);
        if (slot_mgr_ != nullptr && !slot_name_.empty()) {
            (void)slot_mgr_->deactivate_slot(slot_name_);
        }
        connection_->close();
        return;
    }

    state_.store(State::STREAMING);

    // Send CatchupComplete.
    CatchupCompleteMessage cc;
    cc.current_lsn = writer_.current_lsn();
    auto cc_result = send_message(serialize_catchup_complete(cc));
    if (!cc_result) {
        GIODB_LOG_ERROR("WalSender failed to send CatchupComplete to {}: {}",
                        connection_->peer_description(),
                        cc_result.error().message);
        state_.store(State::STOPPED);
        if (slot_mgr_ != nullptr && !slot_name_.empty()) {
            (void)slot_mgr_->deactivate_slot(slot_name_);
        }
        connection_->close();
        return;
    }

    GIODB_LOG_INFO("WalSender catch-up complete for {}, switching to real-time streaming",
                   connection_->peer_description());

    auto stream_result = run_streaming();
    if (!stream_result) {
        GIODB_LOG_WARN("WalSender streaming ended for {}: {}",
                       connection_->peer_description(),
                       stream_result.error().message);
    }

    // Deactivate the replication slot on disconnect.
    if (slot_mgr_ != nullptr && !slot_name_.empty()) {
        (void)slot_mgr_->deactivate_slot(slot_name_);
    }

    state_.store(State::STOPPED);
    connection_->close();
}

Result<void> WalSender::run_catchup(lsn_t start_lsn) {
    // Determine the target LSN to catch up to.
    lsn_t target_lsn = writer_.flushed_lsn();

    if (start_lsn > target_lsn) {
        // Already caught up — nothing to send.
        return ok();
    }

    // Phase 1: Send from archived segments if available.
    if (archive_mgr_ != nullptr) {
        auto segments_result = archive_mgr_->list_archived_segments();
        if (segments_result && !segments_result->empty()) {
            for (uint64_t seg_id : *segments_result) {
                if (stop_requested_.load()) {
                    return make_error(StatusCode::REPLICATION_ERROR, "stop requested");
                }

                auto seg_path = archive_mgr_->get_archived_segment(seg_id);
                if (!seg_path) {
                    continue;
                }

                // Send SegmentStart.
                SegmentStartMessage ss;
                ss.segment_number = seg_id;
                auto ss_result = send_message(serialize_segment_start(ss));
                if (!ss_result) {
                    return ss_result;
                }

                WalReader reader(seg_path->parent_path());
                auto open_result = reader.open(seg_id);
                if (!open_result) {
                    continue;
                }

                auto batch_result = send_wal_batch(reader, target_lsn);
                if (!batch_result) {
                    return make_error(batch_result.error().code, batch_result.error().message);
                }

                (void)reader.close();

                // Process any incoming replica messages between segments.
                auto msg_result = process_replica_messages();
                if (!msg_result) {
                    return msg_result;
                }
            }
        }
    }

    // Phase 2: Send from the active WAL directory.
    WalReader reader(wal_dir_);
    auto open_result = reader.open();
    if (!open_result) {
        return make_error(open_result.error().code,
                          "failed to open WAL for catch-up: " + open_result.error().message);
    }

    // Re-check target — more WAL may have been written during archive catch-up.
    target_lsn = writer_.flushed_lsn();

    auto batch_result = send_wal_batch(reader, target_lsn);
    if (!batch_result) {
        (void)reader.close();
        return make_error(batch_result.error().code, batch_result.error().message);
    }

    (void)reader.close();
    return ok();
}

Result<void> WalSender::run_streaming() {
    while (!stop_requested_.load()) {
        // Wait for new WAL or keepalive timeout.
        {
            std::unique_lock lock(notify_mutex_);
            notify_cv_.wait_for(lock, options_.keepalive_interval, [this] {
                return stop_requested_.load() ||
                       notified_lsn_.load(std::memory_order_acquire) > sent_lsn_.load();
            });
        }

        if (stop_requested_.load()) {
            break;
        }

        lsn_t target = notified_lsn_.load(std::memory_order_acquire);
        if (target > sent_lsn_.load()) {
            // New WAL available — read and send.
            WalReader reader(wal_dir_);
            auto open_result = reader.open();
            if (!open_result) {
                return make_error(open_result.error().code,
                                  "failed to open WAL for streaming: " +
                                      open_result.error().message);
            }

            auto batch_result = send_wal_batch(reader, target);
            (void)reader.close();

            if (!batch_result) {
                return make_error(batch_result.error().code, batch_result.error().message);
            }
        } else {
            // Timeout — send keepalive.
            auto ka_result = send_keepalive(true);
            if (!ka_result) {
                return ka_result;
            }
        }

        // Check for replica messages.
        auto msg_result = process_replica_messages();
        if (!msg_result) {
            return msg_result;
        }

        // Check for replica timeout.
        {
            std::lock_guard lock(status_mutex_);
            auto elapsed = std::chrono::steady_clock::now() - last_status_time_;
            if (elapsed > options_.sender_timeout) {
                return make_error(StatusCode::REPLICATION_ERROR,
                                  "replica timeout: no status received within " +
                                      std::to_string(options_.sender_timeout.count()) + "ms");
            }
        }

        if (!connection_->is_open()) {
            return make_error(StatusCode::NETWORK_ERROR, "connection closed");
        }
    }

    return ok();
}

Result<lsn_t> WalSender::send_wal_batch(WalReader& reader, lsn_t up_to_lsn) {
    lsn_t current_sent = sent_lsn_.load();

    while (!stop_requested_.load()) {
        std::vector<uint8_t> batch_data;
        uint32_t record_count = 0;
        lsn_t batch_start_lsn = invalid_lsn;
        lsn_t batch_end_lsn = invalid_lsn;

        // Collect records into a batch.
        while (record_count < options_.max_batch_records &&
               batch_data.size() < options_.max_batch_bytes) {
            auto rec_result = reader.next();
            if (!rec_result) {
                if (rec_result.error().code == StatusCode::NOT_FOUND) {
                    break; // End of WAL.
                }
                return make_error(rec_result.error().code, rec_result.error().message);
            }

            const auto& record = rec_result.value();

            // Skip records we've already sent.
            if (record.lsn <= current_sent) {
                continue;
            }

            // Stop if we've reached the target.
            if (record.lsn > up_to_lsn) {
                break;
            }

            if (batch_start_lsn == invalid_lsn) {
                batch_start_lsn = record.lsn;
            }
            batch_end_lsn = record.lsn;

            auto serialized = serialize_wal_record(record);
            batch_data.insert(batch_data.end(), serialized.begin(), serialized.end());
            ++record_count;
        }

        if (record_count == 0) {
            break; // Nothing more to send.
        }

        WalDataMessage msg;
        msg.start_lsn = batch_start_lsn;
        msg.end_lsn = batch_end_lsn;
        msg.record_count = record_count;
        msg.data = std::move(batch_data);

        auto send_result = send_message(serialize_wal_data(msg));
        if (!send_result) {
            return make_error(send_result.error().code, send_result.error().message);
        }

        sent_lsn_.store(batch_end_lsn);
        current_sent = batch_end_lsn;

        if (batch_end_lsn >= up_to_lsn) {
            break;
        }
    }

    return ok(sent_lsn_.load());
}

Result<void> WalSender::send_keepalive(bool reply_requested) {
    KeepaliveMessage msg;
    msg.current_lsn = writer_.current_lsn();
    msg.timestamp_us = now_us();
    msg.reply_requested = reply_requested;
    return send_message(serialize_keepalive(msg));
}

Result<void> WalSender::process_replica_messages() {
    // Non-blocking receive — try to read available data.
    auto recv_result = connection_->receive(4096, std::chrono::milliseconds(0));
    if (!recv_result) {
        // Timeout or no data is fine — just return.
        if (recv_result.error().code == StatusCode::NOT_FOUND ||
            recv_result.error().code == StatusCode::NETWORK_ERROR) {
            return ok();
        }
        return make_error(recv_result.error().code, recv_result.error().message);
    }

    auto& data = recv_result.value();
    if (data.empty()) {
        return ok();
    }

    // Parse message(s) from the received data.
    size_t offset = 0;
    while (offset < data.size()) {
        std::span<const uint8_t> remaining(data.data() + offset, data.size() - offset);

        if (remaining.size() < replication_header_size) {
            break;
        }

        auto type_result = peek_message_type(remaining);
        if (!type_result) {
            break;
        }

        auto size_result = message_total_size(remaining);
        if (!size_result) {
            break;
        }

        if (remaining.size() < *size_result) {
            break;
        }

        std::span<const uint8_t> msg_buf(remaining.data(), *size_result);

        switch (*type_result) {
        case ReplicationMessageType::STANDBY_STATUS: {
            auto msg = deserialize_standby_status(msg_buf);
            if (msg) {
                handle_standby_status(*msg);
            }
            break;
        }
        case ReplicationMessageType::HOT_STANDBY_FEEDBACK: {
            auto msg = deserialize_hot_standby_feedback(msg_buf);
            if (msg) {
                handle_hot_standby_feedback(*msg);
            }
            break;
        }
        default:
            GIODB_LOG_WARN("WalSender received unexpected message type {} from {}",
                           static_cast<int>(*type_result),
                           connection_->peer_description());
            break;
        }

        offset += *size_result;
    }

    return ok();
}

Result<void> WalSender::send_message(std::span<const uint8_t> data) {
    if (!connection_->is_open()) {
        return make_error(StatusCode::NETWORK_ERROR, "connection closed");
    }
    return connection_->send(data);
}

} // namespace giodb
