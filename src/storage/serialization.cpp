#include "sixseven/storage/serialization.h"

#include <cstring>

namespace sixseven {

namespace {

// -- Portable big-endian detection --------------------------------------------
// GCC/Clang provide __BYTE_ORDER__, MSVC is always little-endian (x86/ARM).

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#define SIXSEVEN_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#elif defined(_MSC_VER)
#define SIXSEVEN_BIG_ENDIAN 0
#else
#define SIXSEVEN_BIG_ENDIAN 0
#endif

// -- Little-endian helpers ----------------------------------------------------

// Write a trivially copyable value in little-endian byte order.
template <typename T>
void write_le(std::vector<uint8_t>& buf, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
#if SIXSEVEN_BIG_ENDIAN
    for (size_t i = 0; i < sizeof(T) / 2; ++i) {
        std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
#endif
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

// Read a trivially copyable value from little-endian byte order.
template <typename T>
T read_le(const uint8_t* src) {
    static_assert(std::is_trivially_copyable_v<T>);
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, src, sizeof(T));
#if SIXSEVEN_BIG_ENDIAN
    for (size_t i = 0; i < sizeof(T) / 2; ++i) {
        std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
#endif
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

// Write a variable-length blob: 4-byte LE length prefix + raw data.
void write_var(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    write_le<uint32_t>(buf, static_cast<uint32_t>(len));
    buf.insert(buf.end(), data, data + len);
}

// -- Serialized size for the value payload (excluding null flag) --------------

size_t payload_size(const Value& value) {
    switch (value.type_id()) {
    case TypeId::INT8:
    case TypeId::UINT8:
    case TypeId::BOOL:
        return 1;
    case TypeId::INT16:
    case TypeId::UINT16:
        return 2;
    case TypeId::INT32:
    case TypeId::UINT32:
    case TypeId::FLOAT32:
    case TypeId::DATE:
        return 4;
    case TypeId::INT64:
    case TypeId::UINT64:
    case TypeId::FLOAT64:
    case TypeId::TIME:
    case TypeId::TIMESTAMP:
        return 8;
    case TypeId::DECIMAL:
    case TypeId::INTERVAL:
    case TypeId::POINT:
    case TypeId::UUID:
        return 16;
    case TypeId::STRING:
        return 4 + value.as_string().size();
    case TypeId::BLOB:
        return 4 + value.as_blob().size();
    case TypeId::JSON:
        return 4 + value.as_json().data.size();
    case TypeId::EMBEDDING:
        return 4 + value.as_embedding().size() * sizeof(float);
    case TypeId::PATH:
        return 4 + value.as_path().steps.size() * (sizeof(int64_t) * 2);
    }
    return 0;
}

} // namespace

// -- serialize ----------------------------------------------------------------

std::vector<uint8_t> serialize(const Value& value) {
    std::vector<uint8_t> buf;

    if (value.is_null()) {
        buf.push_back(0x00);
        return buf;
    }

    buf.reserve(1 + payload_size(value));
    buf.push_back(0x01);

    switch (value.type_id()) {
    case TypeId::INT8:
        write_le(buf, value.as_int8());
        break;
    case TypeId::INT16:
        write_le(buf, value.as_int16());
        break;
    case TypeId::INT32:
        write_le(buf, value.as_int32());
        break;
    case TypeId::INT64:
        write_le(buf, value.as_int64());
        break;
    case TypeId::UINT8:
        write_le(buf, value.as_uint8());
        break;
    case TypeId::UINT16:
        write_le(buf, value.as_uint16());
        break;
    case TypeId::UINT32:
        write_le(buf, value.as_uint32());
        break;
    case TypeId::UINT64:
        write_le(buf, value.as_uint64());
        break;
    case TypeId::FLOAT32:
        write_le(buf, value.as_float32());
        break;
    case TypeId::FLOAT64:
        write_le(buf, value.as_float64());
        break;
    case TypeId::DECIMAL:
        write_le(buf, value.as_decimal().hi);
        write_le(buf, value.as_decimal().lo);
        break;
    case TypeId::BOOL:
        buf.push_back(value.as_bool() ? 0x01 : 0x00);
        break;
    case TypeId::STRING: {
        const auto& s = value.as_string();
        write_var(buf, reinterpret_cast<const uint8_t*>(s.data()), s.size());
        break;
    }
    case TypeId::BLOB: {
        const auto& b = value.as_blob();
        write_var(buf, b.data(), b.size());
        break;
    }
    case TypeId::DATE:
        write_le(buf, value.as_date().days_since_epoch);
        break;
    case TypeId::TIME:
        write_le(buf, value.as_time().microseconds);
        break;
    case TypeId::TIMESTAMP:
        write_le(buf, value.as_timestamp().microseconds);
        break;
    case TypeId::INTERVAL:
        write_le(buf, value.as_interval().months);
        write_le(buf, value.as_interval().microseconds);
        break;
    case TypeId::POINT:
        write_le(buf, value.as_point().x);
        write_le(buf, value.as_point().y);
        break;
    case TypeId::JSON: {
        const auto& j = value.as_json().data;
        write_var(buf, reinterpret_cast<const uint8_t*>(j.data()), j.size());
        break;
    }
    case TypeId::UUID: {
        const auto& u = value.as_uuid();
        buf.insert(buf.end(), u.begin(), u.end());
        break;
    }
    case TypeId::EMBEDDING: {
        const auto& e = value.as_embedding();
        write_le<uint32_t>(buf, static_cast<uint32_t>(e.size()));
        for (float f : e) {
            write_le(buf, f);
        }
        break;
    }
    case TypeId::PATH: {
        const auto& p = value.as_path();
        write_le<uint32_t>(buf, static_cast<uint32_t>(p.steps.size()));
        for (const auto& step : p.steps) {
            write_le(buf, step.node_pk);
            write_le(buf, step.edge_id);
        }
        break;
    }
    }

    return buf;
}

// -- deserialize --------------------------------------------------------------

Result<Value> deserialize(std::span<const uint8_t> data, TypeId type_id) {
    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "empty buffer in deserialize");
    }

    // Null flag
    if (data[0] == 0x00) {
        return ok(Value::make_null());
    }

    if (data[0] != 0x01) {
        return make_error(StatusCode::INVALID_ARGUMENT, "invalid null flag in deserialize");
    }

    const uint8_t* p = data.data() + 1;
    size_t remaining = data.size() - 1;

    // Helper: check minimum remaining bytes.
    auto check_size = [&](size_t need) -> bool { return remaining >= need; };

    switch (type_id) {
    case TypeId::INT8:
        if (!check_size(1)) {
            break;
        }
        return ok(Value(read_le<int8_t>(p)));
    case TypeId::INT16:
        if (!check_size(2)) {
            break;
        }
        return ok(Value(read_le<int16_t>(p)));
    case TypeId::INT32:
        if (!check_size(4)) {
            break;
        }
        return ok(Value(read_le<int32_t>(p)));
    case TypeId::INT64:
        if (!check_size(8)) {
            break;
        }
        return ok(Value(read_le<int64_t>(p)));
    case TypeId::UINT8:
        if (!check_size(1)) {
            break;
        }
        return ok(Value(read_le<uint8_t>(p)));
    case TypeId::UINT16:
        if (!check_size(2)) {
            break;
        }
        return ok(Value(read_le<uint16_t>(p)));
    case TypeId::UINT32:
        if (!check_size(4)) {
            break;
        }
        return ok(Value(read_le<uint32_t>(p)));
    case TypeId::UINT64:
        if (!check_size(8)) {
            break;
        }
        return ok(Value(read_le<uint64_t>(p)));
    case TypeId::FLOAT32:
        if (!check_size(4)) {
            break;
        }
        return ok(Value(read_le<float>(p)));
    case TypeId::FLOAT64:
        if (!check_size(8)) {
            break;
        }
        return ok(Value(read_le<double>(p)));
    case TypeId::DECIMAL: {
        if (!check_size(16)) {
            break;
        }
        Decimal128 d;
        d.hi = read_le<int64_t>(p);
        d.lo = read_le<uint64_t>(p + 8);
        return ok(Value(d));
    }
    case TypeId::BOOL:
        if (!check_size(1)) {
            break;
        }
        return ok(Value(p[0] != 0x00));
    case TypeId::STRING: {
        if (!check_size(4)) {
            break;
        }
        uint32_t len = read_le<uint32_t>(p);
        if (!check_size(4 + len)) {
            break;
        }
        return ok(Value(std::string(reinterpret_cast<const char*>(p + 4), len)));
    }
    case TypeId::BLOB: {
        if (!check_size(4)) {
            break;
        }
        uint32_t len = read_le<uint32_t>(p);
        if (!check_size(4 + len)) {
            break;
        }
        return ok(Value(Blob(p + 4, p + 4 + len)));
    }
    case TypeId::DATE: {
        if (!check_size(4)) {
            break;
        }
        return ok(Value(Date{read_le<int32_t>(p)}));
    }
    case TypeId::TIME: {
        if (!check_size(8)) {
            break;
        }
        return ok(Value(Time{read_le<int64_t>(p)}));
    }
    case TypeId::TIMESTAMP: {
        if (!check_size(8)) {
            break;
        }
        return ok(Value(Timestamp{read_le<int64_t>(p)}));
    }
    case TypeId::INTERVAL: {
        if (!check_size(16)) {
            break;
        }
        Interval iv;
        iv.months = read_le<int64_t>(p);
        iv.microseconds = read_le<int64_t>(p + 8);
        return ok(Value(iv));
    }
    case TypeId::POINT: {
        if (!check_size(16)) {
            break;
        }
        Point pt;
        pt.x = read_le<double>(p);
        pt.y = read_le<double>(p + 8);
        return ok(Value(pt));
    }
    case TypeId::JSON: {
        if (!check_size(4)) {
            break;
        }
        uint32_t len = read_le<uint32_t>(p);
        if (!check_size(4 + len)) {
            break;
        }
        return ok(Value(JsonString{std::string(reinterpret_cast<const char*>(p + 4), len)}));
    }
    case TypeId::UUID: {
        if (!check_size(16)) {
            break;
        }
        Uuid u;
        std::memcpy(u.data(), p, 16);
        return ok(Value(u));
    }
    case TypeId::EMBEDDING: {
        if (!check_size(4)) {
            break;
        }
        uint32_t count = read_le<uint32_t>(p);
        if (!check_size(4 + count * sizeof(float))) {
            break;
        }
        Embedding e(count);
        for (uint32_t i = 0; i < count; ++i) {
            e[i] = read_le<float>(p + 4 + i * sizeof(float));
        }
        return ok(Value(std::move(e)));
    }
    case TypeId::PATH: {
        if (!check_size(4)) {
            break;
        }
        uint32_t count = read_le<uint32_t>(p);
        constexpr size_t step_size = sizeof(int64_t) * 2;
        if (!check_size(4 + count * step_size)) {
            break;
        }
        Path path;
        path.steps.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            size_t off = 4 + i * step_size;
            path.steps[i].node_pk = read_le<int64_t>(p + off);
            path.steps[i].edge_id = read_le<int64_t>(p + off + sizeof(int64_t));
        }
        return ok(Value(std::move(path)));
    }
    }

    return make_error(StatusCode::INVALID_ARGUMENT,
                      "insufficient data for deserialize of type " +
                          std::string(type_name(type_id)));
}

// -- serialized_size ----------------------------------------------------------

size_t serialized_size(const Value& value) {
    if (value.is_null()) {
        return 1; // null flag only
    }
    return 1 + payload_size(value);
}

} // namespace sixseven
