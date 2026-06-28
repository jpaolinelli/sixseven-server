#include "sixseven/executor/virtual_catalog_scan.h"

#include "sixseven/common/parse_utils.h"
#include "sixseven/common/value.h"

namespace sixseven {

namespace {

Value string_to_value(const std::string& s, TypeId type_id) {
    switch (type_id) {
    case TypeId::INT16: {
        auto pv = safe_stoi(s);
        return pv ? Value(static_cast<int16_t>(*pv)) : Value(s);
    }
    case TypeId::INT32: {
        auto pv = safe_stoi(s);
        return pv ? Value(static_cast<int32_t>(*pv)) : Value(s);
    }
    case TypeId::INT64: {
        auto pv = safe_stoll(s);
        return pv ? Value(static_cast<int64_t>(*pv)) : Value(s);
    }
    case TypeId::BOOL:
        return Value(s == "true" || s == "1");
    case TypeId::STRING:
        return Value(s);
    case TypeId::FLOAT64: {
        auto pv = safe_stod(s);
        return pv ? Value(*pv) : Value(s);
    }
    default:
        return Value(s);
    }
}

} // namespace

VirtualCatalogScanOperator::VirtualCatalogScanOperator(VirtualTableDef def,
                                                       OutputSchema output_schema)
    : def_(std::move(def)), schema_(std::move(output_schema)) {}

const OutputSchema& VirtualCatalogScanOperator::output_schema() const {
    return schema_;
}

std::string VirtualCatalogScanOperator::plan_node_name() const {
    return "Virtual Catalog Scan";
}

std::string VirtualCatalogScanOperator::plan_node_detail() const {
    return "pg_catalog." + def_.name;
}

Result<void> VirtualCatalogScanOperator::do_open() {
    raw_rows_ = def_.generator();
    cursor_ = 0;
    return ok();
}

Result<std::optional<Tuple>> VirtualCatalogScanOperator::do_next() {
    if (cursor_ >= raw_rows_.size()) {
        return ok(std::nullopt);
    }

    const auto& row = raw_rows_[cursor_++];
    Tuple tuple;
    tuple.values.reserve(def_.columns.size());

    for (size_t i = 0; i < def_.columns.size(); ++i) {
        if (i >= row.size() || row[i].empty()) {
            tuple.values.emplace_back(Value::make_null());
        } else {
            tuple.values.emplace_back(string_to_value(row[i], def_.columns[i].type_id));
        }
    }

    return ok(std::make_optional(std::move(tuple)));
}

void VirtualCatalogScanOperator::do_close() {
    raw_rows_.clear();
    cursor_ = 0;
}

} // namespace sixseven
