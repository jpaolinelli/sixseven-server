#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/storage/buffer_pool.h"

#include <memory>

namespace sixseven {

/// Serializes and deserializes B+ tree indexes to/from disk pages.
///
/// Uses the existing BufferPoolManager and slotted page infrastructure.
/// Each B+ tree node is stored as a tuple on a dedicated page.
/// A meta page (page 1) stores the tree configuration and page directory.
class BTreePersistence {
public:
    /// Serialize a B+ tree index to disk pages via the given BPM.
    /// The BPM should be for a freshly created index file.
    /// Returns the meta page ID (always 1).
    [[nodiscard]] static Result<PageId> persist(BufferPoolManager& bpm,
                                                const BTreeIndex& index);

    /// Deserialize a B+ tree index from disk pages via the given BPM.
    /// @param meta_page_id The page ID of the meta page (normally 1).
    [[nodiscard]] static Result<std::unique_ptr<BTreeIndex>> load(BufferPoolManager& bpm,
                                                                   PageId meta_page_id);
};

} // namespace sixseven
