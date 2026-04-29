#pragma once

#include "sixseven/catalog/schema.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"

#include <string>
#include <vector>

namespace sixseven {

/// Volcano iterator that generates rows from a virtual catalog table.
/// Calls the VirtualTableGenerator at open() time and yields rows via next().
class VirtualCatalogScanOperator : public Iterator {
public:
    VirtualCatalogScanOperator(VirtualTableDef def, OutputSchema output_schema);

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    VirtualTableDef def_;
    OutputSchema schema_;
    std::vector<std::vector<std::string>> raw_rows_;
    size_t cursor_ = 0;
};

} // namespace sixseven
