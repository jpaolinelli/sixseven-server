#include "giodb/server/promotion_manager.h"

#include "giodb/common/logging.h"

#include <cstring>
#include <fstream>

namespace giodb {

PromotionManager::PromotionManager(Config& config,
                                   WalReceiver* receiver,
                                   WalWriter& wal_writer,
                                   DiskManager& disk_mgr,
                                   std::filesystem::path wal_dir)
    : config_(config), receiver_(receiver), wal_writer_(wal_writer), disk_mgr_(disk_mgr),
      wal_dir_(std::move(wal_dir)) {}

void PromotionManager::set_on_promoted(OnPromotedCallback callback) {
    on_promoted_ = std::move(callback);
}

void PromotionManager::set_readonly_file_ids(std::vector<FileId> file_ids) {
    readonly_file_ids_ = std::move(file_ids);
}

Result<void> PromotionManager::promote() {
    // Guard: cannot promote if already primary.
    if (!config_.standby_mode) {
        return make_error(StatusCode::REPLICATION_ERROR, "server is already primary");
    }

    // Guard: cannot promote if promotion is already in progress.
    if (promotion_in_progress_) {
        return make_error(StatusCode::REPLICATION_ERROR, "promotion already in progress");
    }

    promotion_in_progress_ = true;

    // --- Step 1: Check replay lag guard rail ---
    if (receiver_ != nullptr) {
        auto state = receiver_->get_state();
        if (state.received_lsn != invalid_lsn && state.applied_lsn != invalid_lsn &&
            state.received_lsn > state.applied_lsn) {
            auto lag_bytes = static_cast<int64_t>(state.received_lsn - state.applied_lsn);

            if (config_.replication_promote_max_lag_bytes > 0 &&
                lag_bytes > config_.replication_promote_max_lag_bytes) {
                promotion_in_progress_ = false;
                return make_error(
                    StatusCode::REPLICATION_ERROR,
                    "replay lag too high for promotion: " + std::to_string(lag_bytes) +
                        " bytes exceeds threshold of " +
                        std::to_string(config_.replication_promote_max_lag_bytes) + " bytes");
            }

            if (lag_bytes > 0) {
                GIODB_LOG_WARN("promoting with {} bytes of unapplied WAL", lag_bytes);
            }
        }
    }

    // --- Step 2: Stop the WAL receiver ---
    if (receiver_ != nullptr) {
        GIODB_LOG_INFO("promotion: stopping WAL receiver");
        receiver_->stop();
    }

    // --- Step 3: Replay all received but unapplied WAL ---
    // The WAL receiver replays records as they arrive, so after stop() returns
    // (which joins the receiver thread), all processing is complete. Any records
    // that were received but not yet replayed have been handled by the receiver's
    // handle_wal_data which writes and replays synchronously before processing
    // the next message.
    GIODB_LOG_INFO("promotion: WAL receiver stopped, all received WAL replayed");

    // --- Step 4: Increment timeline ID ---
    ++timeline_id_;
    GIODB_LOG_INFO("promotion: new timeline ID = {}", timeline_id_);

    // --- Step 5: Write PROMOTE WAL record ---
    WalRecord promote_record;
    promote_record.type = WalRecordType::PROMOTE;
    promote_record.txn_id = 0;
    promote_record.data = serialize_promote_data(timeline_id_);

    auto append_result = wal_writer_.append(promote_record);
    if (!append_result) {
        promotion_in_progress_ = false;
        return make_error(append_result.error().code,
                          "failed to write PROMOTE WAL record: " + append_result.error().message);
    }

    auto flush_result = wal_writer_.flush();
    if (!flush_result) {
        promotion_in_progress_ = false;
        return make_error(flush_result.error().code,
                          "failed to flush PROMOTE WAL record: " + flush_result.error().message);
    }

    GIODB_LOG_INFO("promotion: PROMOTE WAL record written at LSN {}", *append_result);

    // --- Step 6: Persist timeline ID ---
    auto save_result = save_timeline();
    if (!save_result) {
        GIODB_LOG_WARN("promotion: failed to persist timeline ID: {}", save_result.error().message);
        // Non-fatal: the PROMOTE record in WAL is the authoritative source.
    }

    // --- Step 7: Switch server mode ---
    config_.standby_mode = false;
    GIODB_LOG_INFO("promotion: server mode switched to primary");

    // --- Step 8: Re-open data files in read/write mode ---
    for (FileId fid : readonly_file_ids_) {
        auto reopen_result = disk_mgr_.reopen_file_readwrite(fid);
        if (!reopen_result) {
            GIODB_LOG_WARN("promotion: failed to reopen file {} in read/write mode: {}",
                           fid,
                           reopen_result.error().message);
        }
    }

    // --- Step 9: Invoke promotion callback ---
    if (on_promoted_) {
        on_promoted_();
    }

    promotion_in_progress_ = false;
    GIODB_LOG_INFO("promotion: complete — server is now primary with timeline {}", timeline_id_);

    return ok();
}

uint64_t PromotionManager::timeline_id() const {
    return timeline_id_;
}

bool PromotionManager::is_standby() const {
    return config_.standby_mode;
}

Result<void> PromotionManager::load_timeline() {
    auto path = timeline_path();
    if (!std::filesystem::exists(path)) {
        timeline_id_ = 1;
        return ok();
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error(StatusCode::IO_ERROR, "failed to open timeline file: " + path.string());
    }

    uint64_t id = 0;
    if (!(file >> id) || id == 0) {
        return make_error(StatusCode::IO_ERROR, "invalid timeline ID in file: " + path.string());
    }

    timeline_id_ = id;
    GIODB_LOG_INFO("loaded timeline ID {} from {}", timeline_id_, path.string());
    return ok();
}

std::vector<uint8_t> PromotionManager::serialize_promote_data(uint64_t new_timeline_id) {
    std::vector<uint8_t> data(sizeof(uint64_t));
    std::memcpy(data.data(), &new_timeline_id, sizeof(uint64_t));
    return data;
}

Result<uint64_t> PromotionManager::deserialize_promote_data(std::span<const uint8_t> data) {
    if (data.size() < sizeof(uint64_t)) {
        return make_error(StatusCode::PARSE_ERROR,
                          "PROMOTE record data too short: expected " +
                              std::to_string(sizeof(uint64_t)) + " bytes, got " +
                              std::to_string(data.size()));
    }
    uint64_t timeline_id = 0;
    std::memcpy(&timeline_id, data.data(), sizeof(uint64_t));
    return ok(timeline_id);
}

Result<void> PromotionManager::save_timeline() const {
    auto path = timeline_path();
    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error(StatusCode::IO_ERROR, "failed to write timeline file: " + path.string());
    }
    file << timeline_id_;
    file.flush();
    if (!file.good()) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to write timeline ID to file: " + path.string());
    }
    return ok();
}

std::filesystem::path PromotionManager::timeline_path() const {
    return wal_dir_ / "timeline_id";
}

} // namespace giodb
