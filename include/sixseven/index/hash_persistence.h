#pragma once

#include "sixseven/common/result.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/storage/buffer_pool.h"

#include <memory>

namespace sixseven {

/// Serializes and deserializes hash indexes to/from disk pages.
///
/// Each unique bucket is stored on its own page. A meta page stores the
/// directory mapping and configuration. Multiple directory slots that point
/// to the same bucket share the same disk page ID.
class HashPersistence {
public:
    /// Serialize a hash index to disk pages via the given BPM.
    /// Returns the meta page ID.
    [[nodiscard]] static Result<PageId> persist(BufferPoolManager& bpm,
                                                const HashIndex& index);

    /// Deserialize a hash index from disk pages via the given BPM.
    [[nodiscard]] static Result<std::unique_ptr<HashIndex>> load(BufferPoolManager& bpm,
                                                                  PageId meta_page_id);
};

} // namespace sixseven
