#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sixseven {

/// A simple iterator that returns pre-loaded tuples. Supports close/open
/// for re-scanning (needed when used as the left child in nested joins).
class VectorIterator : public Iterator {
public:
    VectorIterator(OutputSchema schema, std::vector<Tuple> tuples)
        : schema_(std::move(schema)), tuples_(std::move(tuples)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        cursor_ = 0;
        return ok();
    }

    Result<std::optional<Tuple>> do_next() override {
        if (cursor_ >= tuples_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        // Return a copy so re-scanning works.
        size_t idx = cursor_++;
        return ok(std::optional<Tuple>(Tuple{tuples_[idx].values, tuples_[idx].rid}));
    }

    void do_close() override { cursor_ = 0; }

private:
    OutputSchema schema_;
    std::vector<Tuple> tuples_;
    size_t cursor_ = 0;
};

/// Collect all tuples from an iterator into a vector.
inline std::vector<Tuple> collect_all(Iterator& iter) {
    std::vector<Tuple> result;
    auto open = iter.open();
    if (!open.has_value()) {
        return result;
    }
    while (true) {
        auto row = iter.next();
        if (!row.has_value() || !row->has_value()) {
            break;
        }
        result.push_back(std::move(row->value()));
    }
    iter.close();
    return result;
}

} // namespace sixseven
