#include "sixseven/table/tuple.h"

#include <cstring>

namespace sixseven {

// -- Schema -------------------------------------------------------------------

Schema::Schema(std::vector<ColumnDef> columns) : columns_(std::move(columns)) {
    col_offsets_.resize(columns_.size());
    col_is_fixed_.resize(columns_.size());

    size_t fixed_offset = 0;
    size_t var_index = 0;

    for (size_t i = 0; i < columns_.size(); ++i) {
        auto fs = fixed_size(columns_[i].type);
        if (fs.has_value()) {
            col_is_fixed_[i] = true;
            col_offsets_[i] = fixed_offset;
            fixed_offset += *fs;
            ++fixed_count_;
        } else {
            col_is_fixed_[i] = false;
            col_offsets_[i] = var_index;
            ++var_index;
            ++var_count_;
        }
    }

    fixed_region_size_ = fixed_offset;
}

size_t Schema::null_bitmap_size() const {
    return (columns_.size() + 7) / 8;
}

std::optional<size_t> Schema::fixed_field_offset(size_t col_index) const {
    if (col_index >= columns_.size() || !col_is_fixed_[col_index]) {
        return std::nullopt;
    }
    return col_offsets_[col_index];
}

std::optional<size_t> Schema::var_field_index(size_t col_index) const {
    if (col_index >= columns_.size() || col_is_fixed_[col_index]) {
        return std::nullopt;
    }
    return col_offsets_[col_index];
}

// -- Helpers ------------------------------------------------------------------

namespace {

// Write a little-endian uint16_t into a buffer.
void write_u16(uint8_t* dest, uint16_t value) {
    std::memcpy(dest, &value, sizeof(uint16_t));
}

// Read a little-endian uint16_t from a buffer.
uint16_t read_u16(const uint8_t* src) {
    uint16_t value = 0;
    std::memcpy(&value, src, sizeof(uint16_t));
    return value;
}

// Serialize a fixed-size Value's raw bytes into dest. Returns bytes written.
size_t write_fixed_value(uint8_t* dest, const Value& value) {
    switch (value.type_id()) {
    case TypeId::INT8: {
        auto v = value.as_int8();
        std::memcpy(dest, &v, 1);
        return 1;
    }
    case TypeId::UINT8: {
        auto v = value.as_uint8();
        std::memcpy(dest, &v, 1);
        return 1;
    }
    case TypeId::BOOL: {
        uint8_t v = value.as_bool() ? 1 : 0;
        dest[0] = v;
        return 1;
    }
    case TypeId::INT16: {
        auto v = value.as_int16();
        std::memcpy(dest, &v, 2);
        return 2;
    }
    case TypeId::UINT16: {
        auto v = value.as_uint16();
        std::memcpy(dest, &v, 2);
        return 2;
    }
    case TypeId::INT32: {
        auto v = value.as_int32();
        std::memcpy(dest, &v, 4);
        return 4;
    }
    case TypeId::UINT32: {
        auto v = value.as_uint32();
        std::memcpy(dest, &v, 4);
        return 4;
    }
    case TypeId::FLOAT32: {
        auto v = value.as_float32();
        std::memcpy(dest, &v, 4);
        return 4;
    }
    case TypeId::DATE: {
        auto v = value.as_date().days_since_epoch;
        std::memcpy(dest, &v, 4);
        return 4;
    }
    case TypeId::INT64: {
        auto v = value.as_int64();
        std::memcpy(dest, &v, 8);
        return 8;
    }
    case TypeId::UINT64: {
        auto v = value.as_uint64();
        std::memcpy(dest, &v, 8);
        return 8;
    }
    case TypeId::FLOAT64: {
        auto v = value.as_float64();
        std::memcpy(dest, &v, 8);
        return 8;
    }
    case TypeId::TIME: {
        auto v = value.as_time().microseconds;
        std::memcpy(dest, &v, 8);
        return 8;
    }
    case TypeId::TIMESTAMP: {
        auto v = value.as_timestamp().microseconds;
        std::memcpy(dest, &v, 8);
        return 8;
    }
    case TypeId::DECIMAL: {
        auto d = value.as_decimal();
        std::memcpy(dest, &d.hi, 8);
        std::memcpy(dest + 8, &d.lo, 8);
        return 16;
    }
    case TypeId::INTERVAL: {
        auto iv = value.as_interval();
        std::memcpy(dest, &iv.months, 8);
        std::memcpy(dest + 8, &iv.microseconds, 8);
        return 16;
    }
    case TypeId::POINT: {
        auto p = value.as_point();
        std::memcpy(dest, &p.x, 8);
        std::memcpy(dest + 8, &p.y, 8);
        return 16;
    }
    case TypeId::UUID: {
        const auto& u = value.as_uuid();
        std::memcpy(dest, u.data(), 16);
        return 16;
    }
    default:
        return 0; // Variable-length types are not handled here.
    }
}

// Read a fixed-size Value from raw bytes.
Value read_fixed_value(const uint8_t* src, TypeId type) {
    switch (type) {
    case TypeId::INT8: {
        int8_t v = 0;
        std::memcpy(&v, src, 1);
        return Value(v);
    }
    case TypeId::UINT8: {
        uint8_t v = 0;
        std::memcpy(&v, src, 1);
        return Value(v);
    }
    case TypeId::BOOL: {
        return Value(src[0] != 0);
    }
    case TypeId::INT16: {
        int16_t v = 0;
        std::memcpy(&v, src, 2);
        return Value(v);
    }
    case TypeId::UINT16: {
        uint16_t v = 0;
        std::memcpy(&v, src, 2);
        return Value(v);
    }
    case TypeId::INT32: {
        int32_t v = 0;
        std::memcpy(&v, src, 4);
        return Value(v);
    }
    case TypeId::UINT32: {
        uint32_t v = 0;
        std::memcpy(&v, src, 4);
        return Value(v);
    }
    case TypeId::FLOAT32: {
        float v = 0;
        std::memcpy(&v, src, 4);
        return Value(v);
    }
    case TypeId::DATE: {
        int32_t v = 0;
        std::memcpy(&v, src, 4);
        return Value(Date{v});
    }
    case TypeId::INT64: {
        int64_t v = 0;
        std::memcpy(&v, src, 8);
        return Value(v);
    }
    case TypeId::UINT64: {
        uint64_t v = 0;
        std::memcpy(&v, src, 8);
        return Value(v);
    }
    case TypeId::FLOAT64: {
        double v = 0;
        std::memcpy(&v, src, 8);
        return Value(v);
    }
    case TypeId::TIME: {
        int64_t v = 0;
        std::memcpy(&v, src, 8);
        return Value(Time{v});
    }
    case TypeId::TIMESTAMP: {
        int64_t v = 0;
        std::memcpy(&v, src, 8);
        return Value(Timestamp{v});
    }
    case TypeId::DECIMAL: {
        Decimal128 d{};
        std::memcpy(&d.hi, src, 8);
        std::memcpy(&d.lo, src + 8, 8);
        return Value(d);
    }
    case TypeId::INTERVAL: {
        Interval iv{};
        std::memcpy(&iv.months, src, 8);
        std::memcpy(&iv.microseconds, src + 8, 8);
        return Value(iv);
    }
    case TypeId::POINT: {
        Point p{};
        std::memcpy(&p.x, src, 8);
        std::memcpy(&p.y, src + 8, 8);
        return Value(p);
    }
    case TypeId::UUID: {
        Uuid u{};
        std::memcpy(u.data(), src, 16);
        return Value(u);
    }
    default:
        return Value::make_null();
    }
}

// Get the raw bytes of a variable-length Value.
// Returns a span pointing into the value's internal data.
std::span<const uint8_t> var_value_bytes(const Value& value) {
    switch (value.type_id()) {
    case TypeId::STRING: {
        const auto& s = value.as_string();
        return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
    }
    case TypeId::BLOB: {
        const auto& b = value.as_blob();
        return {b.data(), b.size()};
    }
    case TypeId::JSON: {
        const auto& j = value.as_json().data;
        return {reinterpret_cast<const uint8_t*>(j.data()), j.size()};
    }
    case TypeId::EMBEDDING: {
        const auto& e = value.as_embedding();
        return {reinterpret_cast<const uint8_t*>(e.data()), e.size() * sizeof(float)};
    }
    case TypeId::PATH: {
        const auto& p = value.as_path();
        return {reinterpret_cast<const uint8_t*>(p.steps.data()),
                p.steps.size() * sizeof(PathStep)};
    }
    default:
        return {};
    }
}

// Read a variable-length Value from raw bytes.
Value read_var_value(const uint8_t* src, size_t length, TypeId type) {
    switch (type) {
    case TypeId::STRING:
        return Value(std::string(reinterpret_cast<const char*>(src), length));
    case TypeId::BLOB:
        return Value(Blob(src, src + length));
    case TypeId::JSON:
        return Value(JsonString{std::string(reinterpret_cast<const char*>(src), length)});
    case TypeId::EMBEDDING: {
        size_t count = length / sizeof(float);
        Embedding emb(count);
        std::memcpy(emb.data(), src, length);
        return Value(std::move(emb));
    }
    case TypeId::PATH: {
        size_t count = length / sizeof(PathStep);
        Path path;
        path.steps.resize(count);
        std::memcpy(path.steps.data(), src, length);
        return Value(std::move(path));
    }
    default:
        return Value::make_null();
    }
}

} // namespace

// -- TupleSerializer ----------------------------------------------------------

namespace TupleSerializer {

Result<std::vector<uint8_t>> serialize(const std::vector<Value>& values, const Schema& schema) {
    if (values.size() != schema.column_count()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "value count (" + std::to_string(values.size()) +
                              ") does not match schema column count (" +
                              std::to_string(schema.column_count()) + ")");
    }

    size_t n = schema.column_count();

    // Compute sizes for each region.
    size_t bitmap_size = schema.null_bitmap_size();
    size_t fixed_size_total = schema.fixed_region_size();
    size_t var_offset_table_size = schema.variable_column_count() * var_entry_size;

    // Compute total variable data size.
    size_t var_data_size = 0;
    for (size_t i = 0; i < n; ++i) {
        if (values[i].is_null()) {
            continue;
        }
        auto vi = schema.var_field_index(i);
        if (vi.has_value()) {
            var_data_size += var_value_bytes(values[i]).size();
        }
    }

    size_t total = bitmap_size + fixed_size_total + var_offset_table_size + var_data_size;

    // Guard against uint16_t offset overflow. Variable-length offsets and lengths
    // are stored as uint16_t, so the total tuple size cannot exceed 65,535 bytes.
    // Callers must route large values through OverflowManager before serializing.
    if (total > max_tuple_size) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "tuple size (" + std::to_string(total) + " bytes) exceeds maximum (" +
                              std::to_string(max_tuple_size) +
                              " bytes); route large values through OverflowManager");
    }

    std::vector<uint8_t> buf(total, 0);

    // 1. Write null bitmap.
    for (size_t i = 0; i < n; ++i) {
        if (values[i].is_null()) {
            buf[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
        }
    }

    // 2. Write fixed-size fields.
    size_t fixed_base = bitmap_size;
    for (size_t i = 0; i < n; ++i) {
        if (values[i].is_null()) {
            continue;
        }
        auto fo = schema.fixed_field_offset(i);
        if (fo.has_value()) {
            write_fixed_value(&buf[fixed_base + *fo], values[i]);
        }
    }

    // 3. Write variable-length offset table and data.
    size_t var_table_base = bitmap_size + fixed_size_total;
    size_t var_data_base = var_table_base + var_offset_table_size;
    size_t var_write_pos = var_data_base;

    for (size_t i = 0; i < n; ++i) {
        auto vi = schema.var_field_index(i);
        if (!vi.has_value()) {
            continue;
        }

        size_t entry_pos = var_table_base + (*vi) * var_entry_size;

        if (values[i].is_null()) {
            // Null variable-length field: offset=0, length=0.
            write_u16(&buf[entry_pos], 0);
            write_u16(&buf[entry_pos + 2], 0);
            continue;
        }

        auto bytes = var_value_bytes(values[i]);
        // Safe to cast: total tuple size was validated above to fit in uint16_t.
        auto offset = static_cast<uint16_t>(var_write_pos);
        auto length = static_cast<uint16_t>(bytes.size());

        write_u16(&buf[entry_pos], offset);
        write_u16(&buf[entry_pos + 2], length);

        if (!bytes.empty()) {
            std::memcpy(&buf[var_write_pos], bytes.data(), bytes.size());
            var_write_pos += bytes.size();
        }
    }

    return ok(std::move(buf));
}

Result<std::vector<Value>> deserialize(std::span<const uint8_t> data, const Schema& schema) {
    size_t n = schema.column_count();
    size_t bitmap_size = schema.null_bitmap_size();

    if (data.size() < bitmap_size) {
        return make_error(StatusCode::INVALID_ARGUMENT, "tuple data too short for null bitmap");
    }

    std::vector<Value> values;
    values.reserve(n);

    size_t fixed_base = bitmap_size;

    for (size_t i = 0; i < n; ++i) {
        // Check null bitmap.
        bool is_null = (data[i / 8] & (1 << (i % 8))) != 0;
        if (is_null) {
            values.push_back(Value::make_null());
            continue;
        }

        auto fo = schema.fixed_field_offset(i);
        if (fo.has_value()) {
            // Fixed-size field.
            size_t offset = fixed_base + *fo;
            auto fs = fixed_size(schema.column(i).type);
            if (!fs || offset + *fs > data.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "tuple data too short for fixed field at column " +
                                      std::to_string(i));
            }
            values.push_back(read_fixed_value(&data[offset], schema.column(i).type));
        } else {
            // Variable-length field.
            auto vi = schema.var_field_index(i);
            if (!vi) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "column " + std::to_string(i) +
                                      " is neither fixed nor variable-length");
            }
            size_t var_table_base = bitmap_size + schema.fixed_region_size();
            size_t entry_pos = var_table_base + (*vi) * var_entry_size;

            if (entry_pos + var_entry_size > data.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "tuple data too short for var-length offset entry at column " +
                                      std::to_string(i));
            }

            uint16_t offset = read_u16(&data[entry_pos]);
            uint16_t length = read_u16(&data[entry_pos + 2]);

            if (offset == 0 && length == 0) {
                // Null variable-length field (already handled by bitmap, but be safe).
                values.push_back(Value::make_null());
                continue;
            }

            if (offset + length > data.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "variable-length data extends beyond tuple at column " +
                                      std::to_string(i));
            }

            values.push_back(read_var_value(&data[offset], length, schema.column(i).type));
        }
    }

    return ok(std::move(values));
}

Result<Value> get_field(std::span<const uint8_t> data, const Schema& schema, size_t col_index) {
    if (col_index >= schema.column_count()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "column index " + std::to_string(col_index) +
                              " out of range (schema has " + std::to_string(schema.column_count()) +
                              " columns)");
    }

    size_t bitmap_size = schema.null_bitmap_size();
    if (data.size() < bitmap_size) {
        return make_error(StatusCode::INVALID_ARGUMENT, "tuple data too short for null bitmap");
    }

    // Check null bitmap.
    bool is_null = (data[col_index / 8] & (1 << (col_index % 8))) != 0;
    if (is_null) {
        return ok(Value::make_null());
    }

    auto fo = schema.fixed_field_offset(col_index);
    if (fo.has_value()) {
        // Fixed-size field.
        size_t offset = bitmap_size + *fo;
        auto fs = fixed_size(schema.column(col_index).type);
        if (!fs || offset + *fs > data.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "tuple data too short for fixed field at column " +
                                  std::to_string(col_index));
        }
        return ok(read_fixed_value(&data[offset], schema.column(col_index).type));
    }

    // Variable-length field.
    auto vi = schema.var_field_index(col_index);
    if (!vi) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "column " + std::to_string(col_index) +
                              " is neither fixed nor variable-length");
    }
    size_t var_table_base = bitmap_size + schema.fixed_region_size();
    size_t entry_pos = var_table_base + (*vi) * var_entry_size;

    if (entry_pos + var_entry_size > data.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "tuple data too short for var-length offset entry at column " +
                              std::to_string(col_index));
    }

    uint16_t offset = read_u16(&data[entry_pos]);
    uint16_t length = read_u16(&data[entry_pos + 2]);

    if (offset == 0 && length == 0) {
        return ok(Value::make_null());
    }

    if (offset + length > data.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "variable-length data extends beyond tuple at column " +
                              std::to_string(col_index));
    }

    return ok(read_var_value(&data[offset], length, schema.column(col_index).type));
}

size_t compute_tuple_size(const std::vector<Value>& values, const Schema& schema) {
    size_t n = schema.column_count();
    size_t bitmap_size = schema.null_bitmap_size();
    size_t fixed_total = schema.fixed_region_size();
    size_t var_table = schema.variable_column_count() * var_entry_size;

    size_t var_data = 0;
    for (size_t i = 0; i < n && i < values.size(); ++i) {
        if (values[i].is_null()) {
            continue;
        }
        auto vi = schema.var_field_index(i);
        if (vi.has_value()) {
            var_data += var_value_bytes(values[i]).size();
        }
    }

    return bitmap_size + fixed_total + var_table + var_data;
}

} // namespace TupleSerializer

} // namespace sixseven
