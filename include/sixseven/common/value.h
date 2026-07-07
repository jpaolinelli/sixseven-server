#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace sixseven {

// -- Wrapper types for semantically distinct types with overlapping C++ types --

/// 128-bit decimal: high 64 bits (signed) + low 64 bits (unsigned).
struct Decimal128 {
    int64_t hi = 0;
    uint64_t lo = 0;

    bool operator==(const Decimal128& other) const = default;
};

/// Date stored as days since Unix epoch (1970-01-01).
struct Date {
    int32_t days_since_epoch = 0;

    bool operator==(const Date& other) const = default;
};

/// Time stored as microseconds since midnight.
struct Time {
    int64_t microseconds = 0;

    bool operator==(const Time& other) const = default;
};

/// Timestamp stored as microseconds since Unix epoch.
struct Timestamp {
    int64_t microseconds = 0;

    bool operator==(const Timestamp& other) const = default;
};

/// Interval with month and microsecond components.
struct Interval {
    int64_t months = 0;
    int64_t microseconds = 0;

    bool operator==(const Interval& other) const = default;
};

/// 2D point with x/y coordinates.
struct Point {
    double x = 0.0;
    double y = 0.0;

    bool operator==(const Point& other) const = default;
};

/// JSON value stored as a string.
struct JsonString {
    std::string data;

    bool operator==(const JsonString& other) const = default;
};

/// UUID as a 16-byte array.
using Uuid = std::array<uint8_t, 16>;

/// Binary large object.
using Blob = std::vector<uint8_t>;

/// Vector embedding.
using Embedding = std::vector<float>;

// Forward declaration: Value is defined below (it is a variant that holds
// Path, which holds PathStep, which holds Value -- a genuine circular
// dependency). PathStep/Path are declared here as incomplete-friendly shells;
// their Value-dependent members are implemented out-of-line after Value's
// full definition, near the bottom of this file.
class Value;

/// A step in a graph path: a node PK followed by an optional edge ID.
///
/// GDB-1292: node_pk was widened from int64_t to Value so that graph paths
/// can carry STRING (and other non-integer) primary keys end-to-end, not just
/// the 8 integer types. node_pk is held via unique_ptr because Value is not
/// yet a complete type at this point in the header (Value's variant holds
/// Path, which holds PathStep -- a genuine circular dependency); the pointer
/// is fully hidden behind value-semantics accessors so callers use
/// PathStep exactly like a plain struct with a Value member.
struct PathStep {
    PathStep();
    PathStep(Value pk, int64_t eid);
    /// Convenience overload for the very common integer-PK case (mirrors the
    /// pre-GDB-1292 aggregate-init call sites like `{1, 100}` throughout the
    /// test suite): builds node_pk() as Value(int64_t{pk}).
    PathStep(int64_t pk, int64_t eid);
    PathStep(const PathStep&);
    PathStep(PathStep&&) noexcept;
    PathStep& operator=(const PathStep&);
    PathStep& operator=(PathStep&&) noexcept;
    ~PathStep();

    /// The node's primary key (any PK-eligible type).
    [[nodiscard]] const Value& node_pk() const { return *node_pk_; }
    [[nodiscard]] Value& node_pk() { return *node_pk_; }
    void set_node_pk(Value v);

    /// Widen an integer-typed node_pk to int64_t (mirrors
    /// graph_traversal_core.h's pk_to_int64 widening for INT8..INT64/
    /// UINT8..UINT64). Throws std::bad_variant_access if node_pk() holds a
    /// non-integer type (e.g. STRING) -- callers that may see non-integer
    /// PKs (post-GDB-1292) should branch on node_pk().type_id() instead.
    /// Provided as a convenience for call sites (mostly tests) that only
    /// ever deal with integer-PK graphs.
    [[nodiscard]] int64_t node_pk_as_int64() const;

    int64_t edge_id = -1; ///< -1 means no edge (terminal node).

    bool operator==(const PathStep& other) const;

private:
    std::unique_ptr<Value> node_pk_;
};

/// An ordered graph path: a sequence of (node_pk, edge_id) steps.
struct Path {
    std::vector<PathStep> steps;
    double total_weight = 0.0; ///< Total weight for weighted shortest path (0.0 for unweighted).

    /// Return the number of edges (hops) in this path.
    [[nodiscard]] int64_t length() const {
        if (steps.empty())
            return 0;
        // Length = number of edges = steps - 1 (the last step has no outgoing edge).
        return static_cast<int64_t>(steps.size()) - 1;
    }

    bool operator==(const Path& other) const;
};

// -- Value variant type -------------------------------------------------------

/// The internal variant holding all possible value representations.
/// std::monostate represents NULL.
using ValueData = std::variant<std::monostate, // NULL
                               int8_t,         // INT8
                               int16_t,        // INT16
                               int32_t,        // INT32
                               int64_t,        // INT64
                               uint8_t,        // UINT8
                               uint16_t,       // UINT16
                               uint32_t,       // UINT32
                               uint64_t,       // UINT64
                               float,          // FLOAT32
                               double,         // FLOAT64
                               Decimal128,     // DECIMAL
                               bool,           // BOOL
                               std::string,    // STRING
                               Blob,           // BLOB
                               Date,           // DATE
                               Time,           // TIME
                               Timestamp,      // TIMESTAMP
                               Interval,       // INTERVAL
                               Point,          // POINT
                               JsonString,     // JSON
                               Uuid,           // UUID
                               Embedding,      // EMBEDDING
                               Path            // PATH
                               >;

/// A polymorphic value that can hold any of the 23 SixSevenDB data types, or NULL.
class Value {
public:
    /// Construct a NULL value.
    Value() = default;

    /// Construct from typed data.
    explicit Value(int8_t v) : data_(v) {}
    explicit Value(int16_t v) : data_(v) {}
    explicit Value(int32_t v) : data_(v) {}
    explicit Value(int64_t v) : data_(v) {}
    explicit Value(uint8_t v) : data_(v) {}
    explicit Value(uint16_t v) : data_(v) {}
    explicit Value(uint32_t v) : data_(v) {}
    explicit Value(uint64_t v) : data_(v) {}
    explicit Value(float v) : data_(v) {}
    explicit Value(double v) : data_(v) {}
    explicit Value(Decimal128 v) : data_(v) {}
    explicit Value(bool v) : data_(v) {}
    explicit Value(std::string v) : data_(std::move(v)) {}
    explicit Value(Blob v) : data_(std::move(v)) {}
    explicit Value(Date v) : data_(v) {}
    explicit Value(Time v) : data_(v) {}
    explicit Value(Timestamp v) : data_(v) {}
    explicit Value(Interval v) : data_(v) {}
    explicit Value(Point v) : data_(v) {}
    explicit Value(JsonString v) : data_(std::move(v)) {}
    explicit Value(Uuid v) : data_(v) {}
    explicit Value(Embedding v) : data_(std::move(v)) {}
    explicit Value(Path v) : data_(std::move(v)) {}

    /// Create a NULL value explicitly.
    static Value make_null() { return Value{}; }

    /// Return true if this value is NULL.
    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(data_); }

    /// Return the TypeId of the held value.
    /// WARNING: For NULL values, this returns TypeId::INT8 as a placeholder — callers MUST
    /// check is_null() before relying on type_id(). This is a known design trade-off to avoid
    /// adding a 23rd TypeId::NULL_TYPE that would complicate switch exhaustiveness everywhere.
    [[nodiscard]] TypeId type_id() const {
        // Variant index maps directly to our type order.
        // Index 0 = monostate (NULL) — maps to INT8 as a dummy value.
        // IMPORTANT: Always check is_null() before using this result.
        static constexpr std::array<TypeId, 24> index_to_type = {{
            TypeId::INT8,      // 0: monostate (NULL — MUST check is_null() first!)
            TypeId::INT8,      // 1: int8_t
            TypeId::INT16,     // 2: int16_t
            TypeId::INT32,     // 3: int32_t
            TypeId::INT64,     // 4: int64_t
            TypeId::UINT8,     // 5: uint8_t
            TypeId::UINT16,    // 6: uint16_t
            TypeId::UINT32,    // 7: uint32_t
            TypeId::UINT64,    // 8: uint64_t
            TypeId::FLOAT32,   // 9: float
            TypeId::FLOAT64,   // 10: double
            TypeId::DECIMAL,   // 11: Decimal128
            TypeId::BOOL,      // 12: bool
            TypeId::STRING,    // 13: std::string
            TypeId::BLOB,      // 14: Blob
            TypeId::DATE,      // 15: Date
            TypeId::TIME,      // 16: Time
            TypeId::TIMESTAMP, // 17: Timestamp
            TypeId::INTERVAL,  // 18: Interval
            TypeId::POINT,     // 19: Point
            TypeId::JSON,      // 20: JsonString
            TypeId::UUID,      // 21: Uuid
            TypeId::EMBEDDING, // 22: Embedding
            TypeId::PATH,      // 23: Path
        }};
        return index_to_type.at(data_.index());
    }

    // -- Typed accessors (throw std::bad_variant_access on type mismatch) -----

    [[nodiscard]] const int8_t& as_int8() const { return std::get<int8_t>(data_); }
    [[nodiscard]] const int16_t& as_int16() const { return std::get<int16_t>(data_); }
    [[nodiscard]] const int32_t& as_int32() const { return std::get<int32_t>(data_); }
    [[nodiscard]] const int64_t& as_int64() const { return std::get<int64_t>(data_); }
    [[nodiscard]] const uint8_t& as_uint8() const { return std::get<uint8_t>(data_); }
    [[nodiscard]] const uint16_t& as_uint16() const { return std::get<uint16_t>(data_); }
    [[nodiscard]] const uint32_t& as_uint32() const { return std::get<uint32_t>(data_); }
    [[nodiscard]] const uint64_t& as_uint64() const { return std::get<uint64_t>(data_); }
    [[nodiscard]] const float& as_float32() const { return std::get<float>(data_); }
    [[nodiscard]] const double& as_float64() const { return std::get<double>(data_); }
    [[nodiscard]] const Decimal128& as_decimal() const { return std::get<Decimal128>(data_); }
    [[nodiscard]] const bool& as_bool() const { return std::get<bool>(data_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(data_); }
    [[nodiscard]] const Blob& as_blob() const { return std::get<Blob>(data_); }
    [[nodiscard]] const Date& as_date() const { return std::get<Date>(data_); }
    [[nodiscard]] const Time& as_time() const { return std::get<Time>(data_); }
    [[nodiscard]] const Timestamp& as_timestamp() const { return std::get<Timestamp>(data_); }
    [[nodiscard]] const Interval& as_interval() const { return std::get<Interval>(data_); }
    [[nodiscard]] const Point& as_point() const { return std::get<Point>(data_); }
    [[nodiscard]] const JsonString& as_json() const { return std::get<JsonString>(data_); }
    [[nodiscard]] const Uuid& as_uuid() const { return std::get<Uuid>(data_); }
    [[nodiscard]] const Embedding& as_embedding() const { return std::get<Embedding>(data_); }
    [[nodiscard]] const Path& as_path() const { return std::get<Path>(data_); }

    // -- Mutable accessors ----------------------------------------------------

    [[nodiscard]] int8_t& as_int8() { return std::get<int8_t>(data_); }
    [[nodiscard]] int16_t& as_int16() { return std::get<int16_t>(data_); }
    [[nodiscard]] int32_t& as_int32() { return std::get<int32_t>(data_); }
    [[nodiscard]] int64_t& as_int64() { return std::get<int64_t>(data_); }
    [[nodiscard]] uint8_t& as_uint8() { return std::get<uint8_t>(data_); }
    [[nodiscard]] uint16_t& as_uint16() { return std::get<uint16_t>(data_); }
    [[nodiscard]] uint32_t& as_uint32() { return std::get<uint32_t>(data_); }
    [[nodiscard]] uint64_t& as_uint64() { return std::get<uint64_t>(data_); }
    [[nodiscard]] float& as_float32() { return std::get<float>(data_); }
    [[nodiscard]] double& as_float64() { return std::get<double>(data_); }
    [[nodiscard]] Decimal128& as_decimal() { return std::get<Decimal128>(data_); }
    [[nodiscard]] bool& as_bool() { return std::get<bool>(data_); }
    [[nodiscard]] std::string& as_string() { return std::get<std::string>(data_); }
    [[nodiscard]] Blob& as_blob() { return std::get<Blob>(data_); }
    [[nodiscard]] Date& as_date() { return std::get<Date>(data_); }
    [[nodiscard]] Time& as_time() { return std::get<Time>(data_); }
    [[nodiscard]] Timestamp& as_timestamp() { return std::get<Timestamp>(data_); }
    [[nodiscard]] Interval& as_interval() { return std::get<Interval>(data_); }
    [[nodiscard]] Point& as_point() { return std::get<Point>(data_); }
    [[nodiscard]] JsonString& as_json() { return std::get<JsonString>(data_); }
    [[nodiscard]] Uuid& as_uuid() { return std::get<Uuid>(data_); }
    [[nodiscard]] Embedding& as_embedding() { return std::get<Embedding>(data_); }
    [[nodiscard]] Path& as_path() { return std::get<Path>(data_); }

    // -- Result-based accessors (return Error instead of throwing) -----------
    // Use these in hot paths or when callers prefer Result<T> error handling
    // over catching std::bad_variant_access.

    [[nodiscard]] Result<const int8_t*> try_as_int8() const {
        return try_get<int8_t>(TypeId::INT8);
    }
    [[nodiscard]] Result<const int16_t*> try_as_int16() const {
        return try_get<int16_t>(TypeId::INT16);
    }
    [[nodiscard]] Result<const int32_t*> try_as_int32() const {
        return try_get<int32_t>(TypeId::INT32);
    }
    [[nodiscard]] Result<const int64_t*> try_as_int64() const {
        return try_get<int64_t>(TypeId::INT64);
    }
    [[nodiscard]] Result<const uint8_t*> try_as_uint8() const {
        return try_get<uint8_t>(TypeId::UINT8);
    }
    [[nodiscard]] Result<const uint16_t*> try_as_uint16() const {
        return try_get<uint16_t>(TypeId::UINT16);
    }
    [[nodiscard]] Result<const uint32_t*> try_as_uint32() const {
        return try_get<uint32_t>(TypeId::UINT32);
    }
    [[nodiscard]] Result<const uint64_t*> try_as_uint64() const {
        return try_get<uint64_t>(TypeId::UINT64);
    }
    [[nodiscard]] Result<const float*> try_as_float32() const {
        return try_get<float>(TypeId::FLOAT32);
    }
    [[nodiscard]] Result<const double*> try_as_float64() const {
        return try_get<double>(TypeId::FLOAT64);
    }
    [[nodiscard]] Result<const Decimal128*> try_as_decimal() const {
        return try_get<Decimal128>(TypeId::DECIMAL);
    }
    [[nodiscard]] Result<const bool*> try_as_bool() const { return try_get<bool>(TypeId::BOOL); }
    [[nodiscard]] Result<const std::string*> try_as_string() const {
        return try_get<std::string>(TypeId::STRING);
    }
    [[nodiscard]] Result<const Blob*> try_as_blob() const { return try_get<Blob>(TypeId::BLOB); }
    [[nodiscard]] Result<const Date*> try_as_date() const { return try_get<Date>(TypeId::DATE); }
    [[nodiscard]] Result<const Time*> try_as_time() const { return try_get<Time>(TypeId::TIME); }
    [[nodiscard]] Result<const Timestamp*> try_as_timestamp() const {
        return try_get<Timestamp>(TypeId::TIMESTAMP);
    }
    [[nodiscard]] Result<const Interval*> try_as_interval() const {
        return try_get<Interval>(TypeId::INTERVAL);
    }
    [[nodiscard]] Result<const Point*> try_as_point() const {
        return try_get<Point>(TypeId::POINT);
    }
    [[nodiscard]] Result<const JsonString*> try_as_json() const {
        return try_get<JsonString>(TypeId::JSON);
    }
    [[nodiscard]] Result<const Uuid*> try_as_uuid() const { return try_get<Uuid>(TypeId::UUID); }
    [[nodiscard]] Result<const Embedding*> try_as_embedding() const {
        return try_get<Embedding>(TypeId::EMBEDDING);
    }
    [[nodiscard]] Result<const Path*> try_as_path() const { return try_get<Path>(TypeId::PATH); }

    /// Access the underlying variant data.
    [[nodiscard]] const ValueData& data() const { return data_; }
    [[nodiscard]] ValueData& data() { return data_; }

    /// Value equality (defined out-of-line, near PathStep/Path definitions
    /// below, since Path/PathStep's own operator== must be complete first).
    bool operator==(const Value& other) const;

private:
    template <typename T>
    [[nodiscard]] Result<const T*> try_get(TypeId expected) const {
        auto* ptr = std::get_if<T>(&data_);
        if (ptr == nullptr) {
            return make_error(StatusCode::TYPE_ERROR,
                              "expected " + std::string(type_name(expected)) + ", got " +
                                  (is_null() ? "NULL" : std::string(type_name(type_id()))));
        }
        return ok(ptr);
    }

    ValueData data_;
};

// -- Out-of-line definitions for PathStep/Path/Value ------------------------
// These require Value to be a complete type (Value::operator==), which in
// turn requires PathStep/Path to be complete (Path is a Value variant
// alternative). Defining them here, after Value's full class body, breaks
// the circular dependency between Value <-> Path <-> PathStep (GDB-1292).

inline bool Value::operator==(const Value& other) const {
    return data_ == other.data_;
}

inline PathStep::PathStep() : node_pk_(std::make_unique<Value>()) {}

inline PathStep::PathStep(Value pk, int64_t eid)
    : edge_id(eid), node_pk_(std::make_unique<Value>(std::move(pk))) {}

inline PathStep::PathStep(int64_t pk, int64_t eid)
    : edge_id(eid), node_pk_(std::make_unique<Value>(pk)) {}

inline PathStep::PathStep(const PathStep& other)
    : edge_id(other.edge_id), node_pk_(std::make_unique<Value>(*other.node_pk_)) {}

inline PathStep::PathStep(PathStep&&) noexcept = default;

inline PathStep& PathStep::operator=(const PathStep& other) {
    if (this != &other) {
        edge_id = other.edge_id;
        node_pk_ = std::make_unique<Value>(*other.node_pk_);
    }
    return *this;
}

inline PathStep& PathStep::operator=(PathStep&&) noexcept = default;

inline PathStep::~PathStep() = default;

inline void PathStep::set_node_pk(Value v) {
    node_pk_ = std::make_unique<Value>(std::move(v));
}

inline int64_t PathStep::node_pk_as_int64() const {
    const Value& pk = *node_pk_;
    switch (pk.type_id()) {
    case TypeId::INT8:
        return static_cast<int64_t>(pk.as_int8());
    case TypeId::INT16:
        return static_cast<int64_t>(pk.as_int16());
    case TypeId::INT32:
        return static_cast<int64_t>(pk.as_int32());
    case TypeId::INT64:
        return pk.as_int64();
    case TypeId::UINT8:
        return static_cast<int64_t>(pk.as_uint8());
    case TypeId::UINT16:
        return static_cast<int64_t>(pk.as_uint16());
    case TypeId::UINT32:
        return static_cast<int64_t>(pk.as_uint32());
    case TypeId::UINT64:
        return static_cast<int64_t>(pk.as_uint64());
    default:
        // Non-integer PK type (e.g. STRING): std::get<int64_t> below throws
        // std::bad_variant_access, matching this method's documented
        // contract for non-integer node_pk() values.
        return std::get<int64_t>(pk.data());
    }
}

inline bool PathStep::operator==(const PathStep& other) const {
    return node_pk() == other.node_pk() && edge_id == other.edge_id;
}

inline bool Path::operator==(const Path& other) const {
    return steps == other.steps && total_weight == other.total_weight;
}

} // namespace sixseven
