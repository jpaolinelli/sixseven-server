#include "giodb/server/wal_sender_manager.h"

#include "giodb/common/logging.h"

#include <algorithm>

namespace giodb {

WalSenderManager::WalSenderManager(std::filesystem::path wal_dir,
                                   WalArchiveManager* archive_mgr,
                                   WalWriter& writer,
                                   uint32_t max_wal_senders,
                                   WalSenderOptions sender_options,
                                   ReplicationSlotManager* slot_mgr)
    : wal_dir_(std::move(wal_dir)), archive_mgr_(archive_mgr), writer_(writer),
      max_wal_senders_(max_wal_senders), sender_options_(sender_options), slot_mgr_(slot_mgr) {}

WalSenderManager::~WalSenderManager() {
    stop_all();
}

Result<void> WalSenderManager::accept_connection(std::unique_ptr<ReplicationConnection> connection,
                                                 lsn_t start_lsn,
                                                 const std::string& slot_name) {
    std::lock_guard lock(senders_mutex_);

    // Clean up stopped senders before checking the limit.
    std::erase_if(senders_, [](const std::unique_ptr<WalSender>& s) {
        return s->state() == WalSender::State::STOPPED;
    });

    if (senders_.size() >= max_wal_senders_) {
        return make_error(StatusCode::REPLICATION_ERROR,
                          "max_wal_senders limit reached (" + std::to_string(max_wal_senders_) +
                              ")");
    }

    auto peer = connection->peer_description();
    auto sender = std::make_unique<WalSender>(std::move(connection),
                                              wal_dir_,
                                              archive_mgr_,
                                              writer_,
                                              sender_options_,
                                              slot_mgr_,
                                              slot_name);

    auto result = sender->start_streaming(start_lsn);
    if (!result) {
        return make_error(result.error().code,
                          "failed to start streaming to " + peer + ": " + result.error().message);
    }

    senders_.push_back(std::move(sender));
    GIODB_LOG_INFO("accepted replication connection from {} (active: {})", peer, senders_.size());
    return ok();
}

void WalSenderManager::notify_new_wal(lsn_t flush_lsn) {
    std::lock_guard lock(senders_mutex_);
    for (auto& sender : senders_) {
        if (sender->state() != WalSender::State::STOPPED) {
            sender->notify_new_wal(flush_lsn);
        }
    }
}

void WalSenderManager::cleanup_stopped() {
    std::lock_guard lock(senders_mutex_);
    auto removed = std::erase_if(senders_, [](const std::unique_ptr<WalSender>& s) {
        return s->state() == WalSender::State::STOPPED;
    });
    if (removed > 0) {
        GIODB_LOG_DEBUG("cleaned up {} stopped WalSenders", removed);
    }
}

void WalSenderManager::stop_all() {
    std::lock_guard lock(senders_mutex_);
    for (auto& sender : senders_) {
        sender->stop();
    }
    senders_.clear();
}

size_t WalSenderManager::active_sender_count() const {
    std::lock_guard lock(senders_mutex_);
    size_t count = 0;
    for (const auto& sender : senders_) {
        if (sender->state() != WalSender::State::STOPPED) {
            ++count;
        }
    }
    return count;
}

uint32_t WalSenderManager::max_wal_senders() const {
    return max_wal_senders_;
}

std::vector<WalSenderManager::SenderStatus> WalSenderManager::get_sender_statuses() const {
    std::lock_guard lock(senders_mutex_);
    std::vector<SenderStatus> statuses;
    statuses.reserve(senders_.size());
    for (const auto& sender : senders_) {
        statuses.push_back({
            sender->peer_description(),
            sender->state(),
            sender->replica_received_lsn(),
            sender->replica_applied_lsn(),
            sender->replica_flushed_lsn(),
        });
    }
    return statuses;
}

} // namespace giodb
