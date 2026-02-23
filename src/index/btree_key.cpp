#include "giodb/index/btree_key.h"

#include "giodb/common/coercion.h"

namespace giodb {

Result<std::strong_ordering> compare_keys(const KeyType& lhs, const KeyType& rhs) {
    if (lhs.size() != rhs.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "key column count mismatch: " + std::to_string(lhs.size()) + " vs " +
                              std::to_string(rhs.size()));
    }

    for (size_t i = 0; i < lhs.size(); ++i) {
        auto cmp = compare(lhs[i], rhs[i]);
        if (!cmp.has_value()) {
            return tl::unexpected(cmp.error());
        }
        if (*cmp != std::strong_ordering::equal) {
            return ok(*cmp);
        }
    }

    return ok(std::strong_ordering::equal);
}

} // namespace giodb
