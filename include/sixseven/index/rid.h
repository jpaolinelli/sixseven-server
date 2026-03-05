#pragma once

#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"

#include <cstdint>

namespace sixseven {

/// Record identifier: locates a tuple within a heap page.
struct RID {
    PageId page_id = 0;
    SlotId slot_id = 0;

    bool operator==(const RID&) const = default;
    auto operator<=>(const RID&) const = default;

    /// Sentinel for "no record".
    static constexpr RID invalid() { return {0, 0}; }
};

} // namespace sixseven
