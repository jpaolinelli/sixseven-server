#pragma once

#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/common/value_hash.h"
#include "giodb/index/btree_key.h"
#include "giodb/index/rid.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace giodb {

// Forward declaration.
class WalWriter;

/// Configuration for creating a hash index.
struct HashIndexConfig {
    /// Column types in the composite key.
    std::vector<TypeId> key_types;

    /// Maximum entries per bucket before a split is triggered.
    uint32_t bucket_capacity = 64;

    /// Whether this index enforces uniqueness.
    bool is_unique = false;
};

/// An entry stored in a hash bucket: the full key, its hash, and the RID.
struct HashBucketEntry {
    size_t hash = 0;
    KeyType key;
    RID rid;
};

/// A hash bucket stores entries and tracks its local depth for extensible hashing.
struct HashBucket {
    /// Local depth: number of hash bits used to distinguish this bucket.
    uint32_t local_depth = 0;

    /// The entries stored in this bucket.
    std::vector<HashBucketEntry> entries;
};

/// Extensible hash index: supports insert, point lookup, and delete for
/// equality predicates only. Does NOT support range scans.
///
/// Uses extensible hashing with a directory of bucket pointers. When a bucket
/// overflows, it is split and the directory is doubled if needed.
///
/// Thread safety: All public methods are safe to call from multiple threads.
/// Reads (search) acquire a shared lock; writes (insert, remove) acquire an
/// exclusive lock on the index.
class HashIndex {
public:
    /// Construct a hash index.
    /// @param config Index configuration.
    /// @param wal Optional WAL writer for logging structural changes.
    explicit HashIndex(HashIndexConfig config, WalWriter* wal = nullptr);

    /// Insert a (key, rid) pair into the index.
    /// Returns CONSTRAINT_VIOLATION if is_unique and key already exists.
    [[nodiscard]] Result<void> insert(const KeyType& key, const RID& rid);

    /// Point lookup: find the RID for an exact key match.
    /// Returns nullopt if the key is not found.
    [[nodiscard]] Result<std::optional<RID>> search(const KeyType& key) const;

    /// Find all RIDs matching an exact key (for non-unique indexes).
    [[nodiscard]] Result<std::vector<RID>> search_all(const KeyType& key) const;

    /// Delete the entry with the given key (and optionally matching RID).
    /// Returns true if an entry was found and deleted.
    [[nodiscard]] Result<bool> remove(const KeyType& key);

    /// Delete the entry with the given key and specific RID.
    /// Returns true if the exact (key, rid) pair was found and deleted.
    [[nodiscard]] Result<bool> remove(const KeyType& key, const RID& rid);

    /// Return the total number of entries in the index.
    [[nodiscard]] uint64_t size() const;

    /// Return whether the index is empty.
    [[nodiscard]] bool empty() const;

    /// Return the current global depth (log2 of directory size).
    [[nodiscard]] uint32_t global_depth() const;

    /// Return the number of buckets currently allocated.
    [[nodiscard]] uint32_t bucket_count() const;

    /// Return the index configuration.
    [[nodiscard]] const HashIndexConfig& config() const;

private:
    /// Compute the hash for a composite key.
    [[nodiscard]] size_t hash_key(const KeyType& key) const;

    /// Extract the directory index from a hash value using the current global depth.
    [[nodiscard]] uint32_t directory_index(size_t hash) const;

    /// Compare two composite keys for equality.
    [[nodiscard]] bool keys_equal(const KeyType& a, const KeyType& b) const;

    /// Split a bucket that has overflowed, doubling the directory if needed.
    [[nodiscard]] Result<void> split_bucket(uint32_t bucket_idx);

    /// Log a bucket split WAL record if a WAL writer is configured.
    void log_split(uint32_t original_bucket_idx, uint32_t new_bucket_idx);

    /// Log a directory growth WAL record if a WAL writer is configured.
    void log_directory_growth(uint32_t old_depth, uint32_t new_depth);

    // -- State ---
    HashIndexConfig config_;
    WalWriter* wal_;
    uint32_t global_depth_ = 0;
    uint64_t size_ = 0;

    /// Directory: maps hash prefix → bucket. Multiple directory slots can
    /// point to the same bucket when local_depth < global_depth.
    std::vector<std::shared_ptr<HashBucket>> directory_;

    /// Index-level latch for concurrent access.
    mutable std::shared_mutex index_latch_;

    /// Hash functor for individual values.
    ValueHash value_hasher_;
    ValueEqual value_equal_;
};

} // namespace giodb
