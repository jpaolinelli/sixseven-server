#include "sixseven/common/coercion.h"

#include "sixseven/common/uuid.h"

#include <cmath>
#include <compare>
#include <cstdint>

namespace sixseven {

namespace {

// -- Numeric rank for coercion promotion --------------------------------------

// Higher rank = wider type. Used to determine the common type for coercion.
// Returns -1 for non-numeric types.
int numeric_rank(TypeId id) {
    switch (id) {
    case TypeId::INT8:
        return 1;
    case TypeId::UINT8:
        return 2;
    case TypeId::INT16:
        return 3;
    case TypeId::UINT16:
        return 4;
    case TypeId::INT32:
        return 5;
    case TypeId::UINT32:
        return 6;
    case TypeId::INT64:
        return 7;
    case TypeId::UINT64:
        return 8;
    case TypeId::FLOAT32:
        return 9;
    case TypeId::FLOAT64:
        return 10;
    case TypeId::DECIMAL:
        return 11;
    default:
        return -1;
    }
}

// Convert any numeric Value to int64_t for comparison/coercion.
int64_t to_int64(const Value& v) {
    switch (v.type_id()) {
    case TypeId::INT8:
        return v.as_int8();
    case TypeId::INT16:
        return v.as_int16();
    case TypeId::INT32:
        return v.as_int32();
    case TypeId::INT64:
        return v.as_int64();
    case TypeId::UINT8:
        return v.as_uint8();
    case TypeId::UINT16:
        return v.as_uint16();
    case TypeId::UINT32:
        return v.as_uint32();
    case TypeId::UINT64:
        return static_cast<int64_t>(v.as_uint64());
    default:
        return 0;
    }
}

// Convert any numeric Value to double for comparison/coercion.
double to_double(const Value& v) {
    switch (v.type_id()) {
    case TypeId::INT8:
        return v.as_int8();
    case TypeId::INT16:
        return v.as_int16();
    case TypeId::INT32:
        return v.as_int32();
    case TypeId::INT64:
        return static_cast<double>(v.as_int64());
    case TypeId::UINT8:
        return v.as_uint8();
    case TypeId::UINT16:
        return v.as_uint16();
    case TypeId::UINT32:
        return v.as_uint32();
    case TypeId::UINT64:
        return static_cast<double>(v.as_uint64());
    case TypeId::FLOAT32:
        return v.as_float32();
    case TypeId::FLOAT64:
        return v.as_float64();
    case TypeId::DECIMAL: {
        auto d = v.as_decimal();
        // Interpret hi:lo as a signed 128-bit integer and convert to double.
        // hi carries the sign and upper 64 bits; lo is the lower 64 bits.
        return static_cast<double>(d.hi) * 18446744073709551616.0 + static_cast<double>(d.lo);
    }
    default:
        return 0.0;
    }
}

// Create a Value of the target integer type from an int64_t.
Value int64_to_value(int64_t v, TypeId target) {
    switch (target) {
    case TypeId::INT8:
        return Value(static_cast<int8_t>(v));
    case TypeId::INT16:
        return Value(static_cast<int16_t>(v));
    case TypeId::INT32:
        return Value(static_cast<int32_t>(v));
    case TypeId::INT64:
        return Value(v);
    case TypeId::UINT8:
        return Value(static_cast<uint8_t>(v));
    case TypeId::UINT16:
        return Value(static_cast<uint16_t>(v));
    case TypeId::UINT32:
        return Value(static_cast<uint32_t>(v));
    case TypeId::UINT64:
        return Value(static_cast<uint64_t>(v));
    default:
        return Value(v);
    }
}

// Compare two doubles, treating NaN as greater than everything.
std::strong_ordering compare_doubles(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) {
        return std::strong_ordering::equal;
    }
    if (std::isnan(a)) {
        return std::strong_ordering::greater;
    }
    if (std::isnan(b)) {
        return std::strong_ordering::less;
    }
    if (a < b) {
        return std::strong_ordering::less;
    }
    if (a > b) {
        return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

// Compare two same-type non-null values.
Result<std::strong_ordering> compare_same_type(const Value& lhs, const Value& rhs) {
    switch (lhs.type_id()) {
    case TypeId::INT8:
        return ok(lhs.as_int8() <=> rhs.as_int8());
    case TypeId::INT16:
        return ok(lhs.as_int16() <=> rhs.as_int16());
    case TypeId::INT32:
        return ok(lhs.as_int32() <=> rhs.as_int32());
    case TypeId::INT64:
        return ok(lhs.as_int64() <=> rhs.as_int64());
    case TypeId::UINT8:
        return ok(lhs.as_uint8() <=> rhs.as_uint8());
    case TypeId::UINT16:
        return ok(lhs.as_uint16() <=> rhs.as_uint16());
    case TypeId::UINT32:
        return ok(lhs.as_uint32() <=> rhs.as_uint32());
    case TypeId::UINT64:
        return ok(lhs.as_uint64() <=> rhs.as_uint64());
    case TypeId::FLOAT32:
        return ok(compare_doubles(lhs.as_float32(), rhs.as_float32()));
    case TypeId::FLOAT64:
        return ok(compare_doubles(lhs.as_float64(), rhs.as_float64()));
    case TypeId::DECIMAL: {
        auto la = lhs.as_decimal();
        auto ra = rhs.as_decimal();
        if (la.hi != ra.hi) {
            return ok(la.hi <=> ra.hi);
        }
        return ok(la.lo <=> ra.lo);
    }
    case TypeId::BOOL:
        return ok(static_cast<int>(lhs.as_bool()) <=> static_cast<int>(rhs.as_bool()));
    case TypeId::STRING:
        return ok(lhs.as_string() <=> rhs.as_string());
    case TypeId::DATE:
        return ok(lhs.as_date().days_since_epoch <=> rhs.as_date().days_since_epoch);
    case TypeId::TIME:
        return ok(lhs.as_time().microseconds <=> rhs.as_time().microseconds);
    case TypeId::TIMESTAMP:
        return ok(lhs.as_timestamp().microseconds <=> rhs.as_timestamp().microseconds);
    case TypeId::INTERVAL: {
        // Lexicographic comparison by (months, microseconds). Note: this does NOT
        // attempt calendar-aware normalization — Interval{1, 0} (1 month) is not
        // equivalent to Interval{0, 2678400000000} (31 days in microseconds).
        // This matches PostgreSQL's interval comparison semantics.
        auto li = lhs.as_interval();
        auto ri = rhs.as_interval();
        if (li.months != ri.months) {
            return ok(li.months <=> ri.months);
        }
        return ok(li.microseconds <=> ri.microseconds);
    }
    case TypeId::POINT: {
        auto lp = lhs.as_point();
        auto rp = rhs.as_point();
        auto x_cmp = compare_doubles(lp.x, rp.x);
        if (x_cmp != std::strong_ordering::equal) {
            return ok(x_cmp);
        }
        return ok(compare_doubles(lp.y, rp.y));
    }
    case TypeId::JSON:
        return ok(lhs.as_json().data <=> rhs.as_json().data);
    case TypeId::UUID: {
        auto& lu = lhs.as_uuid();
        auto& ru = rhs.as_uuid();
        for (size_t i = 0; i < 16; ++i) {
            if (lu[i] != ru[i]) {
                return ok(lu[i] <=> ru[i]);
            }
        }
        return ok(std::strong_ordering::equal);
    }
    case TypeId::BLOB:
    case TypeId::EMBEDDING:
        return make_error(StatusCode::TYPE_ERROR,
                          "cannot compare values of type " + std::string(type_name(lhs.type_id())));
    }
    return make_error(StatusCode::INTERNAL_ERROR, "unknown type in comparison");
}

} // namespace

// -- can_coerce ---------------------------------------------------------------

bool can_coerce(TypeId from, TypeId to) {
    if (from == to) {
        return true;
    }

    int from_rank = numeric_rank(from);
    int to_rank = numeric_rank(to);

    // Both are numeric: allow widening promotion.
    if (from_rank > 0 && to_rank > 0) {
        // Reject signed→unsigned coercions: negative values have no valid
        // representation in unsigned types.
        bool from_signed = (from == TypeId::INT8 || from == TypeId::INT16 ||
                            from == TypeId::INT32 || from == TypeId::INT64);
        bool to_unsigned = (to == TypeId::UINT8 || to == TypeId::UINT16 || to == TypeId::UINT32 ||
                            to == TypeId::UINT64);
        if (from_signed && to_unsigned) {
            return false;
        }
        return to_rank >= from_rank;
    }

    // STRING → UUID: allow implicit coercion from string literals.
    if (from == TypeId::STRING && to == TypeId::UUID) {
        return true;
    }

    return false;
}

// -- coerce -------------------------------------------------------------------

Result<Value> coerce(const Value& value, TypeId target) {
    if (value.is_null()) {
        return ok(Value::make_null());
    }

    TypeId from = value.type_id();
    if (from == target) {
        return ok(value);
    }

    if (!can_coerce(from, target)) {
        return make_error(StatusCode::TYPE_ERROR,
                          "cannot coerce " + std::string(type_name(from)) + " to " +
                              std::string(type_name(target)));
    }

    // Numeric coercions
    if (target == TypeId::FLOAT32) {
        return ok(Value(static_cast<float>(to_double(value))));
    }
    if (target == TypeId::FLOAT64) {
        return ok(Value(to_double(value)));
    }
    if (target == TypeId::DECIMAL) {
        // For now, store integer value in lo, 0 in hi.
        if (is_integer(from)) {
            int64_t v = to_int64(value);
            if (v >= 0) {
                return ok(Value(Decimal128{0, static_cast<uint64_t>(v)}));
            }
            return ok(Value(Decimal128{-1, static_cast<uint64_t>(v)}));
        }
        // Float to decimal: truncate to integer representation.
        double d = to_double(value);
        if (d >= 0) {
            return ok(Value(Decimal128{0, static_cast<uint64_t>(d)}));
        }
        return ok(Value(Decimal128{-1, static_cast<uint64_t>(static_cast<int64_t>(d))}));
    }

    // Integer-to-integer widening
    if (is_integer(from) && is_integer(target)) {
        int64_t v = to_int64(value);
        return ok(int64_to_value(v, target));
    }

    // FLOAT32 to FLOAT64
    if (from == TypeId::FLOAT32 && target == TypeId::FLOAT64) {
        return ok(Value(static_cast<double>(value.as_float32())));
    }

    // STRING → UUID
    if (from == TypeId::STRING && target == TypeId::UUID) {
        auto parsed = parse_uuid(value.as_string());
        if (!parsed) {
            return tl::unexpected(parsed.error());
        }
        return ok(Value(*parsed));
    }

    return make_error(StatusCode::TYPE_ERROR,
                      "cannot coerce " + std::string(type_name(from)) + " to " +
                          std::string(type_name(target)));
}

// -- fit_to_storage -----------------------------------------------------------

Result<Value> fit_to_storage(const Value& val, TypeId target) {
    if (val.is_null() || val.type_id() == target) {
        return ok(val);
    }
    // Try standard widening coercion first.
    if (can_coerce(val.type_id(), target)) {
        return coerce(val, target);
    }
    // Allow numeric narrowing (e.g. INT64 → INT32).
    if (is_numeric(val.type_id()) && is_integer(target)) {
        int64_t v = to_int64(val);
        return ok(int64_to_value(v, target));
    }
    if (is_numeric(val.type_id()) && is_floating(target)) {
        double d = to_double(val);
        if (target == TypeId::FLOAT32) {
            return ok(Value(static_cast<float>(d)));
        }
        if (target == TypeId::FLOAT64) {
            return ok(Value(d));
        }
    }
    return make_error(StatusCode::TYPE_ERROR,
                      "cannot fit " + std::string(type_name(val.type_id())) + " to " +
                          std::string(type_name(target)));
}

// -- compare ------------------------------------------------------------------

Result<std::strong_ordering> compare(const Value& lhs, const Value& rhs) {
    // NULL handling: NULL sorts before all non-NULL values.
    if (lhs.is_null() && rhs.is_null()) {
        return ok(std::strong_ordering::equal);
    }
    if (lhs.is_null()) {
        return ok(std::strong_ordering::less);
    }
    if (rhs.is_null()) {
        return ok(std::strong_ordering::greater);
    }

    TypeId lt = lhs.type_id();
    TypeId rt = rhs.type_id();

    // Same type: direct comparison.
    if (lt == rt) {
        return compare_same_type(lhs, rhs);
    }

    // Cross-type numeric comparison: promote to common type.
    int lr = numeric_rank(lt);
    int rr = numeric_rank(rt);
    if (lr > 0 && rr > 0) {
        // Promote to the wider type.
        TypeId common = (lr >= rr) ? lt : rt;

        // For mixed integer/float, compare as doubles.
        if (is_floating(lt) || is_floating(rt)) {
            return ok(compare_doubles(to_double(lhs), to_double(rhs)));
        }

        // For DECIMAL, compare as doubles for now.
        if (common == TypeId::DECIMAL) {
            return ok(compare_doubles(to_double(lhs), to_double(rhs)));
        }

        // Mixed signed/unsigned integer comparison: if one side is signed
        // and negative, it is always less than any unsigned value. This avoids
        // incorrect results from unsigned wraparound during coercion.
        bool l_signed = (lt == TypeId::INT8 || lt == TypeId::INT16 || lt == TypeId::INT32 ||
                         lt == TypeId::INT64);
        bool r_signed = (rt == TypeId::INT8 || rt == TypeId::INT16 || rt == TypeId::INT32 ||
                         rt == TypeId::INT64);
        if (l_signed != r_signed) {
            if (l_signed && to_int64(lhs) < 0) {
                return ok(std::strong_ordering::less);
            }
            if (r_signed && to_int64(rhs) < 0) {
                return ok(std::strong_ordering::greater);
            }
            // Both non-negative: safe to compare as uint64_t.
            uint64_t l_val =
                (lt == TypeId::UINT64) ? lhs.as_uint64() : static_cast<uint64_t>(to_int64(lhs));
            uint64_t r_val =
                (rt == TypeId::UINT64) ? rhs.as_uint64() : static_cast<uint64_t>(to_int64(rhs));
            return ok(l_val <=> r_val);
        }

        // Same signedness: compare via coerced values.
        auto lc = coerce(lhs, common);
        auto rc = coerce(rhs, common);
        if (!lc) {
            return tl::unexpected(lc.error());
        }
        if (!rc) {
            return tl::unexpected(rc.error());
        }
        return compare_same_type(*lc, *rc);
    }

    return make_error(StatusCode::TYPE_ERROR,
                      "cannot compare " + std::string(type_name(lt)) + " with " +
                          std::string(type_name(rt)));
}

} // namespace sixseven
