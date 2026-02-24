#pragma once

#include "giodb/common/config.h"
#include "giodb/common/result.h"
#include "giodb/server/wal_receiver.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/wal.h"
#include "giodb/storage/wal_record.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace giodb {

/// Callback invoked when the server mode switches from standby to primary.
/// The callback should toggle the query engine out of standby mode and perform
/// any other necessary reconfiguration (e.g., start WalSenderManager).
using OnPromotedCallback = std::function<void()>;

/// Manages the promotion of a read-only standby to become the new primary.
///
/// Promotion steps:
///   1. Validate guard rails (must be standby, replay lag within threshold).
///   2. Stop the WAL receiver (disconnect from old primary).
///   3. Replay all received but unapplied WAL.
///   4. Increment the timeline ID.
///   5. Write a PROMOTE WAL record marking the timeline switch.
///   6. Switch server mode from standby to primary.
///   7. Re-open data files in read/write mode.
///   8. Invoke the on-promoted callback (e.g., start accepting writes).
///
/// Timeline management:
///   - Each promotion increments the timeline ID (starting from 1).
///   - The timeline ID is persisted to a file in the WAL directory.
///   - The PROMOTE WAL record encodes the new timeline ID in its data payload.
class PromotionManager {
public:
    /// Construct a PromotionManager.
    /// @param config       Server configuration (for standby_mode, promote_max_lag_bytes).
    /// @param receiver     WAL receiver to stop during promotion (may be nullptr if not started).
    /// @param wal_writer   WAL writer for writing the PROMOTE record.
    /// @param disk_mgr     Disk manager for re-opening files in read/write mode.
    /// @param wal_dir      Path to the WAL directory (for timeline persistence).
    PromotionManager(Config& config,
                     WalReceiver* receiver,
                     WalWriter& wal_writer,
                     DiskManager& disk_mgr,
                     std::filesystem::path wal_dir);

    /// Set the callback invoked after successful promotion.
    void set_on_promoted(OnPromotedCallback callback);

    /// Set the list of FileIds that should be re-opened in read/write mode
    /// during promotion. These are the data files opened as read-only on standby.
    void set_readonly_file_ids(std::vector<FileId> file_ids);

    /// Promote this standby to primary.
    /// Returns an error if the server is already primary, promotion is already
    /// in progress, or the replay lag exceeds the configured threshold.
    [[nodiscard]] Result<void> promote();

    /// Return the current timeline ID.
    [[nodiscard]] uint64_t timeline_id() const;

    /// Return true if this server is currently in standby mode.
    [[nodiscard]] bool is_standby() const;

    /// Load the timeline ID from the persisted file (if it exists).
    [[nodiscard]] Result<void> load_timeline();

    /// Serialize a timeline ID into the data payload for a PROMOTE WAL record.
    [[nodiscard]] static std::vector<uint8_t> serialize_promote_data(uint64_t new_timeline_id);

    /// Deserialize a timeline ID from a PROMOTE WAL record's data payload.
    [[nodiscard]] static Result<uint64_t> deserialize_promote_data(std::span<const uint8_t> data);

private:
    /// Persist the current timeline ID to the WAL directory.
    [[nodiscard]] Result<void> save_timeline() const;

    /// Path to the timeline persistence file.
    [[nodiscard]] std::filesystem::path timeline_path() const;

    Config& config_;
    WalReceiver* receiver_;
    WalWriter& wal_writer_;
    DiskManager& disk_mgr_;
    std::filesystem::path wal_dir_;

    OnPromotedCallback on_promoted_;
    std::vector<FileId> readonly_file_ids_;
    uint64_t timeline_id_ = 1;
    bool promotion_in_progress_ = false;
};

} // namespace giodb
