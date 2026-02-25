#include "giodb/index/hash_index.h"

#include "giodb/common/logging.h"
#include "giodb/storage/wal.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace giodb {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HashIndex::HashIndex(HashIndexConfig config, WalWriter* wal)
    : config_(std::move(config)), wal_(wal), global_depth_(0), size_(0) {
    // Start with a single bucket (directory size = 2^0 = 1).
    auto bucket = std::make_shared<HashBucket>();
    bucket->local_depth = 0;
    directory_.push_back(std::move(bucket));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<void> HashIndex::insert(const KeyType& key, const RID& rid) {
    std::unique_lock lock(index_latch_);

    auto h = hash_key(key);
    auto idx = directory_index(h);
    auto& bucket = directory_[idx];

    // Check uniqueness constraint.
    if (config_.is_unique) {
        for (const auto& entry : bucket->entries) {
            if (entry.hash == h && keys_equal(entry.key, key)) {
                return make_error(StatusCode::CONSTRAINT_VIOLATION, "hash index: duplicate key");
            }
        }
    }

    // Insert into bucket.
    bucket->entries.push_back({h, key, rid});
    ++size_;

    // Split if the bucket exceeds capacity.
    if (bucket->entries.size() > config_.bucket_capacity) {
        auto result = split_bucket(idx);
        if (!result) {
            return result;
        }
    }

    return ok();
}

Result<std::optional<RID>> HashIndex::search(const KeyType& key) const {
    std::shared_lock lock(index_latch_);

    auto h = hash_key(key);
    auto idx = directory_index(h);
    const auto& bucket = directory_[idx];

    for (const auto& entry : bucket->entries) {
        if (entry.hash == h && keys_equal(entry.key, key)) {
            return ok(std::optional<RID>(entry.rid));
        }
    }

    return ok(std::optional<RID>(std::nullopt));
}

Result<std::vector<RID>> HashIndex::search_all(const KeyType& key) const {
    std::shared_lock lock(index_latch_);

    auto h = hash_key(key);
    auto idx = directory_index(h);
    const auto& bucket = directory_[idx];

    std::vector<RID> results;
    for (const auto& entry : bucket->entries) {
        if (entry.hash == h && keys_equal(entry.key, key)) {
            results.push_back(entry.rid);
        }
    }

    return ok(std::move(results));
}

Result<bool> HashIndex::remove(const KeyType& key) {
    std::unique_lock lock(index_latch_);

    auto h = hash_key(key);
    auto idx = directory_index(h);
    auto& bucket = directory_[idx];

    for (auto it = bucket->entries.begin(); it != bucket->entries.end(); ++it) {
        if (it->hash == h && keys_equal(it->key, key)) {
            bucket->entries.erase(it);
            --size_;
            return ok(true);
        }
    }

    return ok(false);
}

Result<bool> HashIndex::remove(const KeyType& key, const RID& rid) {
    std::unique_lock lock(index_latch_);

    auto h = hash_key(key);
    auto idx = directory_index(h);
    auto& bucket = directory_[idx];

    for (auto it = bucket->entries.begin(); it != bucket->entries.end(); ++it) {
        if (it->hash == h && keys_equal(it->key, key) && it->rid == rid) {
            bucket->entries.erase(it);
            --size_;
            return ok(true);
        }
    }

    return ok(false);
}

uint64_t HashIndex::size() const {
    std::shared_lock lock(index_latch_);
    return size_;
}

bool HashIndex::empty() const {
    std::shared_lock lock(index_latch_);
    return size_ == 0;
}

uint32_t HashIndex::global_depth() const {
    std::shared_lock lock(index_latch_);
    return global_depth_;
}

uint32_t HashIndex::bucket_count() const {
    std::shared_lock lock(index_latch_);
    // Count distinct buckets (multiple directory slots may share a bucket).
    uint32_t count = 0;
    for (uint32_t i = 0; i < directory_.size(); ++i) {
        // A bucket "owns" the directory slot whose index matches the bucket's
        // local_depth prefix. We count it at its first occurrence.
        bool is_first = true;
        for (uint32_t j = 0; j < i; ++j) {
            if (directory_[j].get() == directory_[i].get()) {
                is_first = false;
                break;
            }
        }
        if (is_first) {
            ++count;
        }
    }
    return count;
}

const HashIndexConfig& HashIndex::config() const {
    return config_;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

size_t HashIndex::hash_key(const KeyType& key) const {
    // Combine hashes of each value in the composite key.
    size_t combined = 0;
    for (const auto& v : key) {
        auto h = value_hasher_(v);
        // Mix using a variant of boost::hash_combine.
        combined ^= h + 0x9e3779b9 + (combined << 6) + (combined >> 2);
    }
    return combined;
}

uint32_t HashIndex::directory_index(size_t hash) const {
    if (global_depth_ == 0) {
        return 0;
    }
    // Use the lowest global_depth_ bits of the hash as the directory index.
    return static_cast<uint32_t>(hash & ((1U << global_depth_) - 1));
}

bool HashIndex::keys_equal(const KeyType& a, const KeyType& b) const {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!value_equal_(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

Result<void> HashIndex::split_bucket(uint32_t bucket_idx) {
    auto old_bucket = directory_[bucket_idx];
    auto old_local_depth = old_bucket->local_depth;
    auto new_local_depth = old_local_depth + 1;

    // If local depth would exceed global depth, double the directory first.
    if (new_local_depth > global_depth_) {
        auto old_global_depth = global_depth_;
        global_depth_ = new_local_depth;
        auto old_size = directory_.size();
        directory_.resize(1U << global_depth_);

        // Copy pointers: each old slot i is duplicated at i and i + old_size.
        for (uint32_t i = 0; i < old_size; ++i) {
            directory_[i + old_size] = directory_[i];
        }

        log_directory_growth(old_global_depth, global_depth_);
    }

    // Create the new sibling bucket.
    auto new_bucket = std::make_shared<HashBucket>();
    new_bucket->local_depth = new_local_depth;
    old_bucket->local_depth = new_local_depth;

    // The split bit is the bit at position (new_local_depth - 1).
    uint32_t split_bit = 1U << (new_local_depth - 1);

    // Redistribute entries between old and new buckets.
    std::vector<HashBucketEntry> kept;
    for (auto& entry : old_bucket->entries) {
        auto dir_idx = directory_index(entry.hash);
        if (dir_idx & split_bit) {
            new_bucket->entries.push_back(std::move(entry));
        } else {
            kept.push_back(std::move(entry));
        }
    }
    old_bucket->entries = std::move(kept);

    // Update directory pointers: all slots whose split_bit is set and whose
    // lower bits match should now point to the new bucket.
    for (uint32_t i = 0; i < directory_.size(); ++i) {
        if (directory_[i].get() == old_bucket.get() && (i & split_bit)) {
            directory_[i] = new_bucket;
        }
    }

    log_split(bucket_idx, bucket_idx | split_bit);

    // If either bucket is still over capacity, continue splitting.
    // This handles the pathological case where all entries hash to the same value.
    if (old_bucket->entries.size() > config_.bucket_capacity) {
        auto new_idx = directory_index(old_bucket->entries[0].hash);
        // Guard against infinite recursion: if all entries have the same hash,
        // splitting won't help. Stop when the bucket contains entries that
        // all share the same directory index (meaning they can't be separated further).
        bool all_same = std::all_of(
            old_bucket->entries.begin(), old_bucket->entries.end(), [&](const HashBucketEntry& e) {
                return directory_index(e.hash) == new_idx;
            });
        if (!all_same) {
            return split_bucket(new_idx);
        }
        // All entries collide — no further splits will help.
        // Allow the bucket to remain over capacity.
    }

    if (new_bucket->entries.size() > config_.bucket_capacity) {
        auto new_idx = directory_index(new_bucket->entries[0].hash);
        bool all_same = std::all_of(
            new_bucket->entries.begin(), new_bucket->entries.end(), [&](const HashBucketEntry& e) {
                return directory_index(e.hash) == new_idx;
            });
        if (!all_same) {
            return split_bucket(new_idx);
        }
    }

    return ok();
}

void HashIndex::log_split(uint32_t original_bucket_idx, uint32_t new_bucket_idx) {
    if (wal_ == nullptr) {
        return;
    }

    WalRecord record;
    record.type = WalRecordType::PAGE_SPLIT;
    record.page_id = original_bucket_idx;

    // Store the new bucket index in the data payload.
    record.data.resize(sizeof(uint32_t));
    std::memcpy(record.data.data(), &new_bucket_idx, sizeof(uint32_t));

    (void)wal_->append(record); // Best-effort WAL logging.
}

void HashIndex::log_directory_growth(uint32_t old_depth, uint32_t new_depth) {
    if (wal_ == nullptr) {
        return;
    }

    WalRecord record;
    record.type = WalRecordType::PAGE_SPLIT;
    record.page_id = 0; // Directory growth marker.

    // Store old and new depths in the payload.
    record.data.resize(2 * sizeof(uint32_t));
    std::memcpy(record.data.data(), &old_depth, sizeof(uint32_t));
    std::memcpy(record.data.data() + sizeof(uint32_t), &new_depth, sizeof(uint32_t));

    (void)wal_->append(record); // Best-effort WAL logging.
}

} // namespace giodb
