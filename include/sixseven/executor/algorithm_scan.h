#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/algorithm_registry.h"

#include <string>
#include <vector>

namespace sixseven {

/// Volcano iterator that materialises the result of a graph algorithm.
///
/// On open(), the algorithm is executed eagerly and all result rows are
/// stored.  next() then returns them one at a time.
class AlgorithmScanOperator : public Iterator {
public:
    AlgorithmScanOperator(const AlgorithmRegistry::Entry& entry,
                          AlgorithmContext context,
                          OutputSchema output_schema);

    [[nodiscard]] const OutputSchema& output_schema() const override { return output_schema_; }

    [[nodiscard]] std::string plan_node_name() const override { return "Algorithm Scan"; }
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    [[nodiscard]] Result<void> do_open() override;
    [[nodiscard]] Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    const AlgorithmRegistry::Entry& entry_;
    AlgorithmContext context_;
    OutputSchema output_schema_;
    std::vector<AlgorithmRow> rows_;
    size_t cursor_ = 0;
};

} // namespace sixseven
