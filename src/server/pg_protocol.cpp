#include "sixseven/server/pg_protocol.h"

#include "sixseven/common/logging.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/connection.h"
#include "sixseven/server/session.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace sixseven {

// -- PostgreSQL protocol constants --------------------------------------------

namespace {

// Protocol version 3.0 as a 32-bit integer: major(16) << 16 | minor(16).
constexpr int32_t PG_PROTOCOL_VERSION_3_0 = 196608; // (3 << 16) | 0

// SSL request magic number.
constexpr int32_t SSL_REQUEST_CODE = 80877103;

// Cancel request magic number.
constexpr int32_t CANCEL_REQUEST_CODE = 80877102;

// Frontend message type bytes.
constexpr uint8_t MSG_QUERY = 'Q';
constexpr uint8_t MSG_TERMINATE = 'X';
constexpr uint8_t MSG_PARSE = 'P';
constexpr uint8_t MSG_BIND = 'B';
constexpr uint8_t MSG_DESCRIBE = 'D';
constexpr uint8_t MSG_EXECUTE = 'E';
constexpr uint8_t MSG_SYNC = 'S';
constexpr uint8_t MSG_CLOSE = 'C';
constexpr uint8_t MSG_FLUSH = 'H';
constexpr uint8_t MSG_PASSWORD = 'p'; // PasswordMessage (also used for SASL responses).

// Minimum startup message size: 4-byte length + 4-byte protocol version.
constexpr size_t MIN_STARTUP_SIZE = 8;

// PostgreSQL type OIDs.
constexpr uint32_t PG_OID_BOOL = 16;
constexpr uint32_t PG_OID_INT2 = 21;
constexpr uint32_t PG_OID_INT4 = 23;
constexpr uint32_t PG_OID_INT8 = 20;
constexpr uint32_t PG_OID_FLOAT4 = 700;
constexpr uint32_t PG_OID_FLOAT8 = 701;
constexpr uint32_t PG_OID_NUMERIC = 1700;
constexpr uint32_t PG_OID_TEXT = 25;
constexpr uint32_t PG_OID_VARCHAR = 1043;
constexpr uint32_t PG_OID_BYTEA = 17;
constexpr uint32_t PG_OID_DATE = 1082;
constexpr uint32_t PG_OID_TIME = 1083;
constexpr uint32_t PG_OID_TIMESTAMP = 1114;
constexpr uint32_t PG_OID_INTERVAL = 1186;
constexpr uint32_t PG_OID_POINT = 600;
constexpr uint32_t PG_OID_JSON = 114;
constexpr uint32_t PG_OID_UUID = 2950;

// Custom OIDs for SixSevenDB-specific types (above the PostgreSQL reserved range).
constexpr uint32_t PG_OID_EMBEDDING = 100000;

/// Read a network-order (big-endian) int32 from a byte pointer.
int32_t read_be_int32(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) | (static_cast<uint32_t>(p[3])));
}

/// Generate a random int32 for the cancel secret key.
int32_t generate_secret_key() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> dist;
    return dist(rng);
}

/// Resolve the format code for a given column index per PostgreSQL protocol rules.
/// - Empty vector: all columns text (0).
/// - Single element: applies to all columns.
/// - N elements: per-column format code.
int16_t resolve_format_code(const std::vector<int16_t>& format_codes, size_t col_index) {
    if (format_codes.empty()) {
        return 0;
    }
    if (format_codes.size() == 1) {
        return format_codes[0];
    }
    if (col_index < format_codes.size()) {
        return format_codes[col_index];
    }
    return 0;
}

/// Case-insensitive string prefix check.
bool starts_with_ci(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

/// Build a PostgreSQL CommandComplete tag from a QueryResult.
std::string build_command_complete_tag(const QueryResult& qr) {
    if (!qr.column_names.empty()) {
        return "SELECT " + std::to_string(qr.rows.size());
    }
    if (qr.affected_rows >= 0) {
        std::string tag = qr.message.empty() ? "UNKNOWN" : qr.message;
        if (tag.find("INSERT") == 0) {
            return "INSERT 0 " + std::to_string(qr.affected_rows);
        }
        return tag + " " + std::to_string(qr.affected_rows);
    }
    return qr.message.empty() ? "OK" : qr.message;
}

} // namespace

// -- Type OID mapping ---------------------------------------------------------

uint32_t type_to_pg_oid(TypeId type) {
    switch (type) {
    case TypeId::BOOL:
        return PG_OID_BOOL;
    case TypeId::INT8:
        return PG_OID_INT2; // Closest PG type for int8.
    case TypeId::INT16:
        return PG_OID_INT2;
    case TypeId::INT32:
        return PG_OID_INT4;
    case TypeId::INT64:
        return PG_OID_INT8;
    case TypeId::UINT8:
        return PG_OID_INT2;
    case TypeId::UINT16:
        return PG_OID_INT4;
    case TypeId::UINT32:
        return PG_OID_INT8;
    case TypeId::UINT64:
        return PG_OID_NUMERIC;
    case TypeId::FLOAT32:
        return PG_OID_FLOAT4;
    case TypeId::FLOAT64:
        return PG_OID_FLOAT8;
    case TypeId::DECIMAL:
        return PG_OID_NUMERIC;
    case TypeId::STRING:
        return PG_OID_TEXT;
    case TypeId::BLOB:
        return PG_OID_BYTEA;
    case TypeId::DATE:
        return PG_OID_DATE;
    case TypeId::TIME:
        return PG_OID_TIME;
    case TypeId::TIMESTAMP:
        return PG_OID_TIMESTAMP;
    case TypeId::INTERVAL:
        return PG_OID_INTERVAL;
    case TypeId::POINT:
        return PG_OID_POINT;
    case TypeId::JSON:
        return PG_OID_JSON;
    case TypeId::UUID:
        return PG_OID_UUID;
    case TypeId::EMBEDDING:
        return PG_OID_EMBEDDING;
    case TypeId::PATH:
        return PG_OID_TEXT; // Paths are serialized as text.
    }
    return PG_OID_TEXT; // Fallback.
}

Result<TypeId> pg_oid_to_type(uint32_t oid) {
    switch (oid) {
    case PG_OID_BOOL:
        return ok(TypeId::BOOL);
    case PG_OID_INT2:
        return ok(TypeId::INT16);
    case PG_OID_INT4:
        return ok(TypeId::INT32);
    case PG_OID_INT8:
        return ok(TypeId::INT64);
    case PG_OID_FLOAT4:
        return ok(TypeId::FLOAT32);
    case PG_OID_FLOAT8:
        return ok(TypeId::FLOAT64);
    case PG_OID_NUMERIC:
        return ok(TypeId::DECIMAL);
    case PG_OID_TEXT:
        return ok(TypeId::STRING);
    case PG_OID_BYTEA:
        return ok(TypeId::BLOB);
    case PG_OID_DATE:
        return ok(TypeId::DATE);
    case PG_OID_TIME:
        return ok(TypeId::TIME);
    case PG_OID_TIMESTAMP:
        return ok(TypeId::TIMESTAMP);
    case PG_OID_INTERVAL:
        return ok(TypeId::INTERVAL);
    case PG_OID_POINT:
        return ok(TypeId::POINT);
    case PG_OID_JSON:
        return ok(TypeId::JSON);
    case PG_OID_UUID:
        return ok(TypeId::UUID);
    case PG_OID_EMBEDDING:
        return ok(TypeId::EMBEDDING);
    default:
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "unknown PostgreSQL OID: " + std::to_string(oid));
    }
}

// -- SQLSTATE mapping ---------------------------------------------------------

std::string_view status_to_sqlstate(StatusCode code) {
    switch (code) {
    case StatusCode::OK:
        return "00000"; // Successful completion.
    case StatusCode::NOT_FOUND:
        return "42P01"; // Undefined table.
    case StatusCode::ALREADY_EXISTS:
        return "42P07"; // Duplicate table.
    case StatusCode::INVALID_ARGUMENT:
        return "22023"; // Invalid parameter value.
    case StatusCode::INTERNAL_ERROR:
        return "XX000"; // Internal error.
    case StatusCode::NOT_IMPLEMENTED:
        return "0A000"; // Feature not supported.
    case StatusCode::IO_ERROR:
        return "58030"; // IO error.
    case StatusCode::PARSE_ERROR:
        return "42601"; // Syntax error.
    case StatusCode::TYPE_ERROR:
        return "42804"; // Datatype mismatch.
    case StatusCode::CONSTRAINT_VIOLATION:
        return "23000"; // Integrity constraint.
    case StatusCode::TXN_CONFLICT:
        return "40001"; // Serialization failure.
    case StatusCode::TXN_ABORTED:
        return "25P02"; // In failed SQL transaction.
    case StatusCode::NETWORK_ERROR:
        return "08006"; // Connection failure.
    case StatusCode::AUTH_ERROR:
        return "28000"; // Invalid authorization.
    case StatusCode::REPLICATION_ERROR:
        return "XX001"; // Data corrupted.
    case StatusCode::READ_ONLY:
        return "25006"; // Read-only SQL transaction.
    case StatusCode::LOCK_TIMEOUT:
        return "55P03"; // Lock not available.
    case StatusCode::DEADLOCK:
        return "40P01"; // Deadlock detected.
    }
    return "XX000"; // Fallback.
}

// -- Value text formatting ----------------------------------------------------

std::string value_to_pg_text(const Value& value) {
    if (value.is_null()) {
        return ""; // NULL values are indicated by -1 length, not this string.
    }

    switch (value.type_id()) {
    case TypeId::BOOL:
        return value.as_bool() ? "t" : "f";
    case TypeId::INT8:
        return std::to_string(value.as_int8());
    case TypeId::INT16:
        return std::to_string(value.as_int16());
    case TypeId::INT32:
        return std::to_string(value.as_int32());
    case TypeId::INT64:
        return std::to_string(value.as_int64());
    case TypeId::UINT8:
        return std::to_string(value.as_uint8());
    case TypeId::UINT16:
        return std::to_string(value.as_uint16());
    case TypeId::UINT32:
        return std::to_string(value.as_uint32());
    case TypeId::UINT64:
        return std::to_string(value.as_uint64());
    case TypeId::FLOAT32: {
        std::ostringstream oss;
        oss << value.as_float32();
        return oss.str();
    }
    case TypeId::FLOAT64: {
        std::ostringstream oss;
        oss << value.as_float64();
        return oss.str();
    }
    case TypeId::DECIMAL: {
        // Simplified: format as hi.lo pair.
        const auto& d = value.as_decimal();
        return std::to_string(d.hi) + "." + std::to_string(d.lo);
    }
    case TypeId::STRING:
        return value.as_string();
    case TypeId::BLOB: {
        // PG bytea hex format: \x followed by hex digits.
        const auto& blob = value.as_blob();
        std::string result = "\\x";
        result.reserve(2 + blob.size() * 2);
        for (uint8_t byte : blob) {
            static constexpr const char* hex = "0123456789abcdef";
            result += hex[byte >> 4];
            result += hex[byte & 0x0F];
        }
        return result;
    }
    case TypeId::DATE: {
        // Format as ISO 8601: YYYY-MM-DD.
        int32_t days = value.as_date().days_since_epoch;
        // Simple calculation from days since epoch.
        time_t secs = static_cast<time_t>(days) * 86400;
        struct tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &secs);
#else
        gmtime_r(&secs, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << (tm_buf.tm_year + 1900) << '-' << std::setw(2)
            << (tm_buf.tm_mon + 1) << '-' << std::setw(2) << tm_buf.tm_mday;
        return oss.str();
    }
    case TypeId::TIME: {
        // Format as HH:MM:SS.uuuuuu.
        int64_t us = value.as_time().microseconds;
        int64_t hours = us / 3600000000LL;
        us %= 3600000000LL;
        int64_t minutes = us / 60000000LL;
        us %= 60000000LL;
        int64_t seconds = us / 1000000LL;
        int64_t frac = us % 1000000LL;
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':'
            << std::setw(2) << seconds;
        if (frac > 0) {
            oss << '.' << std::setw(6) << frac;
        }
        return oss.str();
    }
    case TypeId::TIMESTAMP: {
        // Format as ISO 8601: YYYY-MM-DD HH:MM:SS.uuuuuu.
        int64_t us = value.as_timestamp().microseconds;
        time_t secs = static_cast<time_t>(us / 1000000LL);
        int64_t frac = us % 1000000LL;
        if (frac < 0) {
            secs -= 1;
            frac += 1000000LL;
        }
        struct tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &secs);
#else
        gmtime_r(&secs, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << (tm_buf.tm_year + 1900) << '-' << std::setw(2)
            << (tm_buf.tm_mon + 1) << '-' << std::setw(2) << tm_buf.tm_mday << ' ' << std::setw(2)
            << tm_buf.tm_hour << ':' << std::setw(2) << tm_buf.tm_min << ':' << std::setw(2)
            << tm_buf.tm_sec;
        if (frac > 0) {
            oss << '.' << std::setw(6) << frac;
        }
        return oss.str();
    }
    case TypeId::INTERVAL: {
        const auto& iv = value.as_interval();
        std::ostringstream oss;
        if (iv.months != 0) {
            oss << iv.months << " mon";
            if (iv.microseconds != 0) {
                oss << " ";
            }
        }
        if (iv.microseconds != 0 || iv.months == 0) {
            int64_t us = iv.microseconds;
            if (us < 0) {
                oss << "-";
                us = -us;
            }
            int64_t hours = us / 3600000000LL;
            us %= 3600000000LL;
            int64_t minutes = us / 60000000LL;
            us %= 60000000LL;
            int64_t seconds = us / 1000000LL;
            oss << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes
                << ':' << std::setw(2) << seconds;
        }
        return oss.str();
    }
    case TypeId::POINT: {
        const auto& p = value.as_point();
        std::ostringstream oss;
        oss << '(' << p.x << ',' << p.y << ')';
        return oss.str();
    }
    case TypeId::JSON:
        return value.as_json().data;
    case TypeId::UUID: {
        const auto& uuid = value.as_uuid();
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                oss << '-';
            }
            oss << std::setw(2) << static_cast<unsigned>(uuid[i]);
        }
        return oss.str();
    }
    case TypeId::EMBEDDING: {
        const auto& emb = value.as_embedding();
        std::ostringstream oss;
        oss << '[';
        for (size_t i = 0; i < emb.size(); ++i) {
            if (i > 0) {
                oss << ',';
            }
            oss << emb[i];
        }
        oss << ']';
        return oss.str();
    }
    case TypeId::PATH: {
        const auto& path = value.as_path();
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < path.steps.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            oss << path.steps[i].node_pk;
            if (path.steps[i].edge_id >= 0) {
                oss << "-(" << path.steps[i].edge_id << ")->";
            }
        }
        oss << "]";
        return oss.str();
    }
    }
    return ""; // Unreachable.
}

// -- Value binary formatting --------------------------------------------------

namespace {

/// Write a big-endian int16 into a byte buffer.
void push_be16(std::vector<uint8_t>& buf, int16_t val) {
    auto uval = static_cast<uint16_t>(val);
    buf.push_back(static_cast<uint8_t>((uval >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(uval & 0xFF));
}

/// Write a big-endian int32 into a byte buffer.
void push_be32(std::vector<uint8_t>& buf, int32_t val) {
    auto uval = static_cast<uint32_t>(val);
    buf.push_back(static_cast<uint8_t>((uval >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(uval & 0xFF));
}

/// Write a big-endian int64 into a byte buffer.
void push_be64(std::vector<uint8_t>& buf, int64_t val) {
    auto uval = static_cast<uint64_t>(val);
    buf.push_back(static_cast<uint8_t>((uval >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((uval >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(uval & 0xFF));
}

} // namespace

std::vector<uint8_t> value_to_pg_binary(const Value& value) {
    if (value.is_null()) {
        return {}; // NULL is indicated by -1 length; caller handles it.
    }

    std::vector<uint8_t> buf;

    switch (value.type_id()) {
    case TypeId::BOOL:
        buf.push_back(value.as_bool() ? 1 : 0);
        break;
    case TypeId::INT8:
        // Maps to PG int2 (2 bytes big-endian).
        push_be16(buf, static_cast<int16_t>(value.as_int8()));
        break;
    case TypeId::INT16:
        push_be16(buf, value.as_int16());
        break;
    case TypeId::INT32:
        push_be32(buf, value.as_int32());
        break;
    case TypeId::INT64:
        push_be64(buf, value.as_int64());
        break;
    case TypeId::UINT8:
        // Maps to PG int2 (2 bytes big-endian).
        push_be16(buf, static_cast<int16_t>(value.as_uint8()));
        break;
    case TypeId::UINT16:
        // Maps to PG int4 (4 bytes big-endian).
        push_be32(buf, static_cast<int32_t>(value.as_uint16()));
        break;
    case TypeId::UINT32:
        // Maps to PG int8 (8 bytes big-endian).
        push_be64(buf, static_cast<int64_t>(value.as_uint32()));
        break;
    case TypeId::FLOAT32: {
        float f = value.as_float32();
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        push_be32(buf, static_cast<int32_t>(bits));
        break;
    }
    case TypeId::FLOAT64: {
        double d = value.as_float64();
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        push_be64(buf, static_cast<int64_t>(bits));
        break;
    }
    case TypeId::STRING: {
        const auto& s = value.as_string();
        buf.insert(buf.end(), s.begin(), s.end());
        break;
    }
    default: {
        // Unsupported types fall back to text representation.
        std::string text = value_to_pg_text(value);
        buf.insert(buf.end(), text.begin(), text.end());
        break;
    }
    }

    return buf;
}

// -- Parameter substitution ---------------------------------------------------

namespace {

/// Return true if the OID is a numeric type that should be unquoted in SQL.
bool is_numeric_oid(uint32_t oid) {
    return oid == PG_OID_INT2 || oid == PG_OID_INT4 || oid == PG_OID_INT8 || oid == PG_OID_FLOAT4 ||
           oid == PG_OID_FLOAT8 || oid == PG_OID_NUMERIC;
}

/// Validate that a string looks like a valid numeric literal.
/// Accepts optional leading minus, digits, optional decimal point + digits,
/// optional 'e'/'E' exponent with optional sign.
bool is_valid_numeric_literal(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') {
        ++i;
    }
    if (i == s.size()) {
        return false;
    }
    bool has_digit = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        has_digit = true;
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            has_digit = true;
            ++i;
        }
    }
    if (!has_digit) {
        return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        if (i == s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
    }
    return i == s.size();
}

/// Escape a string value for embedding in a SQL single-quoted literal.
/// Doubles any single quotes inside the string.
std::string escape_sql_string(const std::string& s) {
    std::string result;
    result.reserve(s.size() + s.size() / 8);
    for (char c : s) {
        if (c == '\'') {
            result += "''";
        } else {
            result += c;
        }
    }
    return result;
}

/// Format a single parameter value as a SQL literal.
/// Returns the SQL literal string (e.g. "42", "'hello'", "NULL").
Result<std::string> format_param_as_sql(const std::optional<std::string>& value, uint32_t oid) {
    if (!value.has_value()) {
        return ok(std::string("NULL"));
    }
    const auto& val = *value;

    if (oid == PG_OID_BOOL) {
        // Normalize boolean values.
        if (val == "t" || val == "true" || val == "TRUE" || val == "1") {
            return ok(std::string("TRUE"));
        }
        if (val == "f" || val == "false" || val == "FALSE" || val == "0") {
            return ok(std::string("FALSE"));
        }
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid boolean parameter value: '" + val + "'");
    }

    if (is_numeric_oid(oid)) {
        if (!is_valid_numeric_literal(val)) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "invalid numeric parameter value: '" + val + "'");
        }
        return ok(val);
    }

    // OID 0 means unspecified — infer from value content.
    if (oid == 0 && is_valid_numeric_literal(val)) {
        return ok(val);
    }

    // All other types (text, date, timestamp, etc.): single-quote with escaping.
    return ok("'" + escape_sql_string(val) + "'");
}

/// Read a big-endian unsigned integer from up to 8 raw bytes.
uint64_t read_be_uint(const std::string& bytes) {
    uint64_t result = 0;
    for (char c : bytes) {
        result = (result << 8) | static_cast<uint8_t>(c);
    }
    return result;
}

/// Format a floating-point value as locale-independent round-trippable text.
template <typename T>
std::string float_to_text(T value) {
    std::array<char, 64> buf{};
    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec != std::errc{}) {
        return "0"; // Unreachable: 64 chars always fit the shortest representation.
    }
    return std::string(buf.data(), ptr);
}

/// Build the error for a binary parameter whose byte count does not match its type.
tl::unexpected<Error> binary_length_error(const char* type_name, size_t expected, size_t actual) {
    return make_error(StatusCode::INVALID_ARGUMENT,
                      std::string("binary ") + type_name + " parameter must be " +
                          std::to_string(expected) + " bytes, got " + std::to_string(actual));
}

} // namespace

Result<std::string>
substitute_parameters(const std::string& sql,
                      const std::vector<std::optional<std::string>>& param_values,
                      const std::vector<uint32_t>& param_oids) {
    if (param_values.empty()) {
        return ok(sql);
    }

    std::string result;
    result.reserve(sql.size() + param_values.size() * 8);

    bool in_single_quote = false;
    bool in_double_quote = false;

    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];

        // Track quote state so we don't substitute inside string literals.
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            result += c;
            continue;
        }
        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            result += c;
            continue;
        }

        if (in_single_quote || in_double_quote) {
            result += c;
            continue;
        }

        if (c == '$' && i + 1 < sql.size() &&
            std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
            // Parse the parameter number ($1, $2, ..., $N).
            size_t start = i + 1;
            size_t end = start;
            while (end < sql.size() && std::isdigit(static_cast<unsigned char>(sql[end]))) {
                ++end;
            }
            auto param_num_str = sql.substr(start, end - start);
            int param_num = 0;
            auto [ptr, ec] = std::from_chars(
                param_num_str.data(), param_num_str.data() + param_num_str.size(), param_num);
            if (ec != std::errc{} || ptr != param_num_str.data() + param_num_str.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "parameter $" + param_num_str + " is not a valid number");
            }

            if (param_num < 1 || static_cast<size_t>(param_num) > param_values.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "parameter $" + param_num_str + " out of range (have " +
                                      std::to_string(param_values.size()) + " parameters)");
            }

            size_t idx = static_cast<size_t>(param_num - 1);
            uint32_t oid = idx < param_oids.size() ? param_oids[idx] : 0;
            auto formatted = format_param_as_sql(param_values[idx], oid);
            if (!formatted) {
                return make_error(formatted.error().code, formatted.error().message);
            }
            result += *formatted;
            i = end - 1; // -1 because the loop will ++i.
        } else {
            result += c;
        }
    }

    return ok(std::move(result));
}

Result<std::string> decode_binary_parameter(const std::string& bytes, uint32_t oid) {
    switch (oid) {
    case PG_OID_BOOL:
        if (bytes.size() != 1) {
            return binary_length_error("bool", 1, bytes.size());
        }
        return ok(std::string(bytes[0] != 0 ? "t" : "f"));
    case PG_OID_INT2:
        if (bytes.size() != 2) {
            return binary_length_error("int2", 2, bytes.size());
        }
        return ok(std::to_string(static_cast<int16_t>(read_be_uint(bytes))));
    case PG_OID_INT4:
        if (bytes.size() != 4) {
            return binary_length_error("int4", 4, bytes.size());
        }
        return ok(std::to_string(static_cast<int32_t>(read_be_uint(bytes))));
    case PG_OID_INT8:
        if (bytes.size() != 8) {
            return binary_length_error("int8", 8, bytes.size());
        }
        return ok(std::to_string(static_cast<int64_t>(read_be_uint(bytes))));
    case PG_OID_FLOAT4: {
        if (bytes.size() != 4) {
            return binary_length_error("float4", 4, bytes.size());
        }
        auto bits = static_cast<uint32_t>(read_be_uint(bytes));
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return ok(float_to_text(value));
    }
    case PG_OID_FLOAT8: {
        if (bytes.size() != 8) {
            return binary_length_error("float8", 8, bytes.size());
        }
        uint64_t bits = read_be_uint(bytes);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return ok(float_to_text(value));
    }
    case PG_OID_TEXT:
    case PG_OID_VARCHAR:
    case PG_OID_JSON:
        // The binary representation of text-like types is identical to the
        // text representation: pass the raw bytes through unchanged.
        return ok(bytes);
    default:
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "binary format (format code 1) is not supported for parameter type "
                          "OID " +
                              std::to_string(oid));
    }
}

Result<std::vector<std::optional<std::string>>>
decode_bind_parameters(const std::vector<std::optional<std::string>>& param_values,
                       const std::vector<int16_t>& param_format_codes,
                       const std::vector<uint32_t>& param_oids) {
    if (!param_format_codes.empty() && param_format_codes.size() != 1 &&
        param_format_codes.size() != param_values.size()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "bind message has " + std::to_string(param_format_codes.size()) +
                              " parameter format codes but " + std::to_string(param_values.size()) +
                              " parameters");
    }

    std::vector<std::optional<std::string>> decoded;
    decoded.reserve(param_values.size());
    for (size_t i = 0; i < param_values.size(); ++i) {
        int16_t format = resolve_format_code(param_format_codes, i);
        if (format != 0 && format != 1) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "unsupported parameter format code " + std::to_string(format) +
                                  " for parameter $" + std::to_string(i + 1));
        }
        if (format == 0 || !param_values[i].has_value()) {
            // Text format (or SQL NULL): pass through unchanged.
            decoded.push_back(param_values[i]);
            continue;
        }
        uint32_t oid = i < param_oids.size() ? param_oids[i] : 0;
        auto text = decode_binary_parameter(*param_values[i], oid);
        if (!text) {
            return make_error(text.error().code,
                              "parameter $" + std::to_string(i + 1) + ": " + text.error().message);
        }
        decoded.push_back(std::move(*text));
    }
    return ok(std::move(decoded));
}

namespace {

/// Split a comma-separated SQL parameter list into raw SQL expressions.
/// Preserves the exact text of each expression (including quotes for strings).
/// For example, "42, 'hello', NULL" → {"42", "'hello'", "NULL"}.
std::vector<std::string> split_execute_expressions(const std::string& params_str) {
    std::vector<std::string> exprs;
    if (params_str.empty()) {
        return exprs;
    }

    size_t i = 0;
    auto skip_ws = [&]() {
        while (i < params_str.size() && (params_str[i] == ' ' || params_str[i] == '\t')) {
            ++i;
        }
    };

    while (i < params_str.size()) {
        skip_ws();
        if (i >= params_str.size()) {
            break;
        }

        size_t start = i;
        if (params_str[i] == '\'') {
            // Quoted string literal — consume the whole string including quotes.
            ++i; // Skip opening quote.
            while (i < params_str.size()) {
                if (params_str[i] == '\'') {
                    ++i;
                    if (i < params_str.size() && params_str[i] == '\'') {
                        ++i; // Escaped quote — skip both.
                    } else {
                        break; // End of string.
                    }
                } else {
                    ++i;
                }
            }
        } else {
            // Unquoted token: read until comma or end.
            while (i < params_str.size() && params_str[i] != ',') {
                ++i;
            }
        }

        auto token = params_str.substr(start, i - start);
        // Trim trailing whitespace.
        auto end_pos = token.find_last_not_of(" \t");
        if (end_pos != std::string::npos) {
            token = token.substr(0, end_pos + 1);
        }
        exprs.push_back(std::move(token));

        skip_ws();
        if (i < params_str.size() && params_str[i] == ',') {
            ++i; // Skip comma.
        }
    }

    return exprs;
}

/// Replace $1, $2, ... in SQL with raw SQL expression strings.
/// Used for SQL-level EXECUTE where parameters are already SQL expressions.
Result<std::string> substitute_sql_expressions(const std::string& sql,
                                               const std::vector<std::string>& exprs) {
    if (exprs.empty()) {
        return ok(sql);
    }

    std::string result;
    result.reserve(sql.size() + exprs.size() * 8);

    bool in_single_quote = false;
    bool in_double_quote = false;

    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            result += c;
            continue;
        }
        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            result += c;
            continue;
        }

        if (in_single_quote || in_double_quote) {
            result += c;
            continue;
        }

        if (c == '$' && i + 1 < sql.size() &&
            std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
            size_t start = i + 1;
            size_t end = start;
            while (end < sql.size() && std::isdigit(static_cast<unsigned char>(sql[end]))) {
                ++end;
            }
            auto param_num_str = sql.substr(start, end - start);
            int param_num = 0;
            auto [ptr, ec] = std::from_chars(
                param_num_str.data(), param_num_str.data() + param_num_str.size(), param_num);
            if (ec != std::errc{} || ptr != param_num_str.data() + param_num_str.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "parameter $" + param_num_str + " is not a valid number");
            }

            if (param_num < 1 || static_cast<size_t>(param_num) > exprs.size()) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "parameter $" + param_num_str + " out of range (have " +
                                      std::to_string(exprs.size()) + " parameters)");
            }

            result += exprs[static_cast<size_t>(param_num - 1)];
            i = end - 1;
        } else {
            result += c;
        }
    }

    return ok(std::move(result));
}

} // namespace

// -- SQL splitting ------------------------------------------------------------

std::vector<std::string> split_sql_statements(std::string_view sql) {
    std::vector<std::string> stmts;
    std::string current;
    current.reserve(sql.size());

    size_t i = 0;
    while (i < sql.size()) {
        char ch = sql[i];

        // Single-quoted string literal.
        if (ch == '\'') {
            current += ch;
            ++i;
            while (i < sql.size()) {
                current += sql[i];
                if (sql[i] == '\'' && (i + 1 >= sql.size() || sql[i + 1] != '\'')) {
                    ++i;
                    break;
                }
                // Escaped quote '' inside single-quoted string.
                if (sql[i] == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
                    ++i;
                    current += sql[i];
                }
                ++i;
            }
            continue;
        }

        // Double-quoted identifier.
        if (ch == '"') {
            current += ch;
            ++i;
            while (i < sql.size()) {
                current += sql[i];
                if (sql[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        // Dollar-quoted string ($$...$$).
        if (ch == '$' && i + 1 < sql.size() && sql[i + 1] == '$') {
            current += "$$";
            i += 2;
            while (i + 1 < sql.size()) {
                if (sql[i] == '$' && sql[i + 1] == '$') {
                    current += "$$";
                    i += 2;
                    break;
                }
                current += sql[i];
                ++i;
            }
            continue;
        }

        // Semicolon — statement separator.
        if (ch == ';') {
            // Trim whitespace.
            auto start = current.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) {
                auto end = current.find_last_not_of(" \t\n\r");
                stmts.push_back(current.substr(start, end - start + 1));
            }
            current.clear();
            ++i;
            continue;
        }

        current += ch;
        ++i;
    }

    // Final statement (no trailing semicolon).
    auto start = current.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        auto end = current.find_last_not_of(" \t\n\r");
        stmts.push_back(current.substr(start, end - start + 1));
    }

    return stmts;
}

// -- MessageWriter ------------------------------------------------------------

void MessageWriter::begin_message(uint8_t type) {
    buf_.clear();
    has_type_ = true;
    buf_.push_back(type);
    // Reserve 4 bytes for the length field (will be patched in finish()).
    length_offset_ = buf_.size();
    buf_.resize(buf_.size() + 4, 0);
}

void MessageWriter::begin_message_no_type() {
    buf_.clear();
    has_type_ = false;
    // Reserve 4 bytes for the length field.
    length_offset_ = 0;
    buf_.resize(4, 0);
}

void MessageWriter::write_byte(uint8_t val) {
    buf_.push_back(val);
}

void MessageWriter::write_int16(int16_t val) {
    auto uval = static_cast<uint16_t>(val);
    buf_.push_back(static_cast<uint8_t>((uval >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>(uval & 0xFF));
}

void MessageWriter::write_int32(int32_t val) {
    auto uval = static_cast<uint32_t>(val);
    buf_.push_back(static_cast<uint8_t>((uval >> 24) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((uval >> 16) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((uval >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>(uval & 0xFF));
}

void MessageWriter::write_cstring(std::string_view str) {
    buf_.insert(buf_.end(), str.begin(), str.end());
    buf_.push_back(0);
}

void MessageWriter::write_bytes(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

std::vector<uint8_t> MessageWriter::finish() {
    // Patch the length field. Length includes itself but NOT the type byte.
    auto length = static_cast<uint32_t>(buf_.size() - length_offset_);
    buf_[length_offset_] = static_cast<uint8_t>((length >> 24) & 0xFF);
    buf_[length_offset_ + 1] = static_cast<uint8_t>((length >> 16) & 0xFF);
    buf_[length_offset_ + 2] = static_cast<uint8_t>((length >> 8) & 0xFF);
    buf_[length_offset_ + 3] = static_cast<uint8_t>(length & 0xFF);
    return std::move(buf_);
}

// -- MessageReader ------------------------------------------------------------

MessageReader::MessageReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

uint8_t MessageReader::read_byte() {
    if (pos_ >= len_) {
        return 0;
    }
    return data_[pos_++];
}

int16_t MessageReader::read_int16() {
    if (pos_ + 2 > len_) {
        return 0;
    }
    auto val = static_cast<int16_t>((static_cast<uint16_t>(data_[pos_]) << 8) |
                                    static_cast<uint16_t>(data_[pos_ + 1]));
    pos_ += 2;
    return val;
}

int32_t MessageReader::read_int32() {
    if (pos_ + 4 > len_) {
        return 0;
    }
    auto val = read_be_int32(data_ + pos_);
    pos_ += 4;
    return val;
}

std::string_view MessageReader::read_cstring() {
    const auto* start = data_ + pos_;
    while (pos_ < len_ && data_[pos_] != 0) {
        ++pos_;
    }
    auto result = std::string_view(reinterpret_cast<const char*>(start), data_ + pos_ - start);
    if (pos_ < len_) {
        ++pos_; // Skip the null terminator.
    }
    return result;
}

const uint8_t* MessageReader::read_bytes(size_t len) {
    if (pos_ + len > len_) {
        return nullptr;
    }
    const auto* result = data_ + pos_;
    pos_ += len;
    return result;
}

bool MessageReader::has_remaining() const {
    return pos_ < len_;
}

size_t MessageReader::remaining() const {
    return (pos_ < len_) ? (len_ - pos_) : 0;
}

// -- PgProtocolHandler --------------------------------------------------------

PgProtocolHandler::PgProtocolHandler(int32_t backend_pid)
    : backend_pid_(backend_pid), secret_key_(generate_secret_key()),
      session_(std::make_unique<Session>(backend_pid)) {}

PgProtocolHandler::~PgProtocolHandler() = default;
PgProtocolHandler::PgProtocolHandler(PgProtocolHandler&&) noexcept = default;
PgProtocolHandler& PgProtocolHandler::operator=(PgProtocolHandler&&) noexcept = default;

const std::unordered_map<std::string, PreparedStatement>&
PgProtocolHandler::prepared_statements() const {
    return session_->prepared_statements();
}

const std::unordered_map<std::string, Portal>& PgProtocolHandler::portals() const {
    return session_->portals();
}

std::string PgProtocolHandler::startup_database() const {
    auto it = startup_params_.find("database");
    return it != startup_params_.end() ? it->second : std::string{};
}

void PgProtocolHandler::set_query_executor(QueryExecutor executor) {
    query_executor_ = std::move(executor);
}

void PgProtocolHandler::set_query_describer(QueryDescriber describer) {
    query_describer_ = std::move(describer);
}

void PgProtocolHandler::set_auth(AuthMethod method, UserManager* user_mgr) {
    auth_method_ = method;
    user_mgr_ = user_mgr;
}

Result<void> PgProtocolHandler::process(Connection& conn) {
    // Process as many complete messages as are available.
    while (true) {
        const auto& buf = conn.read_buffer();
        if (buf.empty()) {
            return ok();
        }

        auto consumed = process_one_message(conn);
        if (!consumed) {
            return tl::unexpected(consumed.error());
        }
        if (*consumed == 0) {
            // Incomplete message — wait for more data.
            return ok();
        }
        conn.consume_read(*consumed);
    }
}

Result<size_t> PgProtocolHandler::process_one_message(Connection& conn) {
    if (state_ == ProtocolState::WAIT_FOR_STARTUP) {
        return handle_startup_message(conn);
    }
    if (state_ == ProtocolState::WAIT_FOR_AUTH) {
        return handle_auth_message(conn);
    }
    return handle_frontend_message(conn);
}

Result<size_t> PgProtocolHandler::handle_startup_message(Connection& conn) {
    const auto& buf = conn.read_buffer();

    // Need at least 4 bytes for the length field.
    if (buf.size() < 4) {
        return ok(static_cast<size_t>(0));
    }

    auto msg_len = static_cast<uint32_t>(read_be_int32(buf.data()));

    // Sanity check: startup message must be at least 8 bytes and at most 10KB.
    if (msg_len < MIN_STARTUP_SIZE || msg_len > 10240) {
        return make_error(StatusCode::PARSE_ERROR, "invalid startup message length");
    }

    // Wait for the complete message.
    if (buf.size() < msg_len) {
        return ok(static_cast<size_t>(0));
    }

    // Read the protocol version / request code.
    int32_t version = read_be_int32(buf.data() + 4);

    if (version == SSL_REQUEST_CODE) {
        // Respond with 'N' (no SSL support for now).
        uint8_t no_ssl = 'N';
        conn.enqueue_write(&no_ssl, 1);
        return ok(static_cast<size_t>(msg_len));
    }

    if (version == CANCEL_REQUEST_CODE) {
        // Cancel request: 4-byte PID + 4-byte secret key.
        // Not implemented yet — just consume the message.
        SIXSEVEN_LOG_DEBUG("received cancel request (ignored)");
        return ok(static_cast<size_t>(msg_len));
    }

    if (version != PG_PROTOCOL_VERSION_3_0) {
        send_error_response(
            conn, "FATAL", "08004", "unsupported protocol version: " + std::to_string(version));
        state_ = ProtocolState::CLOSED;
        return ok(static_cast<size_t>(msg_len));
    }

    // Parse startup parameters: null-terminated key=value pairs.
    startup_params_.clear();
    MessageReader reader(buf.data() + 8, msg_len - 8);
    while (reader.has_remaining()) {
        auto key = reader.read_cstring();
        if (key.empty()) {
            break; // Terminating null byte.
        }
        auto val = reader.read_cstring();
        startup_params_[std::string(key)] = std::string(val);
    }

    SIXSEVEN_LOG_INFO("startup: user={}, database={}",
                      startup_params_.count("user") > 0 ? startup_params_["user"] : "(none)",
                      startup_params_.count("database") > 0 ? startup_params_["database"]
                                                            : "(none)");

    // Dispatch based on authentication method.
    switch (auth_method_) {
    case AuthMethod::TRUST:
        // No authentication required.
        send_auth_ok(conn);
        complete_startup(conn);
        state_ = ProtocolState::READY;
        break;

    case AuthMethod::MD5: {
        // Check that the user exists.
        std::string username = startup_params_.count("user") > 0 ? startup_params_["user"] : "";
        if (user_mgr_ != nullptr && !user_mgr_->user_exists(username)) {
            send_error_response(conn,
                                "FATAL",
                                "28000",
                                "password authentication failed for user \"" + username + "\"");
            state_ = ProtocolState::CLOSED;
            return ok(static_cast<size_t>(msg_len));
        }
        // Generate random salt and send MD5 auth challenge.
        auto salt_bytes = random_bytes(4);
        std::copy_n(salt_bytes.begin(), 4, md5_salt_.begin());
        send_auth_md5_password(conn, md5_salt_);
        state_ = ProtocolState::WAIT_FOR_AUTH;
        break;
    }

    case AuthMethod::SCRAM_SHA_256: {
        // Check that the user exists.
        std::string username = startup_params_.count("user") > 0 ? startup_params_["user"] : "";
        if (user_mgr_ != nullptr && !user_mgr_->user_exists(username)) {
            send_error_response(conn,
                                "FATAL",
                                "28000",
                                "password authentication failed for user \"" + username + "\"");
            state_ = ProtocolState::CLOSED;
            return ok(static_cast<size_t>(msg_len));
        }
        // Send SASL mechanism list.
        send_auth_sasl(conn);
        scram_state_.emplace();
        scram_first_done_ = false;
        state_ = ProtocolState::WAIT_FOR_AUTH;
        break;
    }
    }

    return ok(static_cast<size_t>(msg_len));
}

void PgProtocolHandler::complete_startup(Connection& conn) {
    send_parameter_status(conn, "server_version", "15.0 (SixSevenDB 0.1.0)");
    send_parameter_status(conn, "server_encoding", "UTF8");
    send_parameter_status(conn, "client_encoding", "UTF8");
    send_parameter_status(conn, "DateStyle", "ISO, MDY");
    send_parameter_status(conn, "integer_datetimes", "on");
    send_parameter_status(conn, "standard_conforming_strings", "on");
    send_backend_key_data(conn);
    send_ready_for_query(conn, 'I');
}

Result<size_t> PgProtocolHandler::handle_auth_message(Connection& conn) {
    const auto& buf = conn.read_buffer();

    // Need at least 5 bytes: 1-byte type + 4-byte length.
    if (buf.size() < 5) {
        return ok(static_cast<size_t>(0));
    }

    uint8_t msg_type = buf[0];
    auto body_len = static_cast<uint32_t>(read_be_int32(buf.data() + 1));
    size_t total_len = 1 + static_cast<size_t>(body_len);

    if (buf.size() < total_len) {
        return ok(static_cast<size_t>(0));
    }

    if (msg_type != MSG_PASSWORD) {
        send_error_response(
            conn, "FATAL", "08P01", "expected password message during authentication");
        state_ = ProtocolState::CLOSED;
        return ok(total_len);
    }

    const uint8_t* payload = buf.data() + 5;
    size_t payload_len = body_len - 4;

    std::string username = startup_params_.count("user") > 0 ? startup_params_["user"] : "";

    if (auth_method_ == AuthMethod::MD5) {
        // PasswordMessage contains null-terminated string.
        std::string client_response(reinterpret_cast<const char*>(payload), payload_len);
        if (!client_response.empty() && client_response.back() == '\0') {
            client_response.pop_back();
        }

        // Look up user.
        std::optional<UserRecord> user;
        if (user_mgr_ != nullptr) {
            user = user_mgr_->get_user(username);
        }

        if (!user ||
            !verify_md5_password(user->password_hash, username, md5_salt_, client_response)) {
            send_error_response(conn,
                                "FATAL",
                                "28P01",
                                "password authentication failed for user \"" + username + "\"");
            state_ = ProtocolState::CLOSED;
            return ok(total_len);
        }

        send_auth_ok(conn);
        complete_startup(conn);
        state_ = ProtocolState::READY;
        return ok(total_len);
    }

    if (auth_method_ == AuthMethod::SCRAM_SHA_256) {
        if (!scram_first_done_) {
            // This is the SASLInitialResponse.
            // Format: mechanism_name\0 [int32 length] [initial_response_data]
            MessageReader reader(payload, payload_len);
            auto mechanism = reader.read_cstring();

            if (mechanism != "SCRAM-SHA-256") {
                send_error_response(conn,
                                    "FATAL",
                                    "28000",
                                    "unsupported SASL mechanism: " + std::string(mechanism));
                state_ = ProtocolState::CLOSED;
                return ok(total_len);
            }

            // Read initial response length and data.
            int32_t resp_len = reader.read_int32();
            std::string client_first;
            if (resp_len > 0 && reader.remaining() >= static_cast<size_t>(resp_len)) {
                const auto* data = reader.read_bytes(static_cast<size_t>(resp_len));
                client_first.assign(reinterpret_cast<const char*>(data),
                                    static_cast<size_t>(resp_len));
            }

            // Look up user.
            std::optional<UserRecord> user;
            if (user_mgr_ != nullptr) {
                user = user_mgr_->get_user(username);
            }

            if (!user) {
                send_error_response(conn,
                                    "FATAL",
                                    "28P01",
                                    "password authentication failed for user \"" + username + "\"");
                state_ = ProtocolState::CLOSED;
                return ok(total_len);
            }

            auto server_first = scram_server_first(client_first, *user, *scram_state_);
            if (!server_first) {
                send_error_response(conn, "FATAL", "28000", server_first.error().message);
                state_ = ProtocolState::CLOSED;
                return ok(total_len);
            }

            send_auth_sasl_continue(conn, *server_first);
            scram_first_done_ = true;
            return ok(total_len);
        }

        // This is the SASLResponse (client-final-message).
        std::string client_final(reinterpret_cast<const char*>(payload), payload_len);

        auto server_final = scram_server_final(client_final, *scram_state_);
        if (!server_final) {
            send_error_response(conn,
                                "FATAL",
                                "28P01",
                                "password authentication failed for user \"" + username + "\"");
            state_ = ProtocolState::CLOSED;
            return ok(total_len);
        }

        send_auth_sasl_final(conn, *server_final);
        send_auth_ok(conn);
        complete_startup(conn);
        state_ = ProtocolState::READY;
        return ok(total_len);
    }

    // Should not reach here for TRUST (it completes in startup).
    send_error_response(conn, "FATAL", "XX000", "unexpected auth state");
    state_ = ProtocolState::CLOSED;
    return ok(total_len);
}

Result<size_t> PgProtocolHandler::handle_frontend_message(Connection& conn) {
    const auto& buf = conn.read_buffer();

    // Need at least 5 bytes: 1-byte type + 4-byte length.
    if (buf.size() < 5) {
        return ok(static_cast<size_t>(0));
    }

    uint8_t msg_type = buf[0];
    auto body_len = static_cast<uint32_t>(read_be_int32(buf.data() + 1));

    // Total message size: 1 (type byte) + body_len.
    size_t total_len = 1 + static_cast<size_t>(body_len);

    // Wait for the complete message.
    if (buf.size() < total_len) {
        return ok(static_cast<size_t>(0));
    }

    // Payload starts after type byte + 4-byte length.
    const uint8_t* payload = buf.data() + 5;
    size_t payload_len = body_len - 4; // body_len includes the 4-byte length field itself.

    switch (msg_type) {
    case MSG_QUERY: {
        // Simple query: null-terminated SQL string.
        std::string_view sql(reinterpret_cast<const char*>(payload), payload_len);
        // Remove trailing null byte.
        if (!sql.empty() && sql.back() == '\0') {
            sql.remove_suffix(1);
        }
        SIXSEVEN_LOG_DEBUG("simple query: {}", sql);
        handle_simple_query(conn, sql);
        break;
    }
    case MSG_TERMINATE: {
        SIXSEVEN_LOG_DEBUG("client sent Terminate");
        session_->cleanup();
        state_ = ProtocolState::CLOSED;
        break;
    }
    case MSG_PARSE: {
        if (!error_in_extended_) {
            handle_parse(conn, payload, payload_len);
        }
        break;
    }
    case MSG_BIND: {
        if (!error_in_extended_) {
            handle_bind(conn, payload, payload_len);
        }
        break;
    }
    case MSG_DESCRIBE: {
        if (!error_in_extended_) {
            handle_describe(conn, payload, payload_len);
        }
        break;
    }
    case MSG_EXECUTE: {
        if (!error_in_extended_) {
            handle_execute(conn, payload, payload_len);
        }
        break;
    }
    case MSG_SYNC: {
        handle_sync(conn);
        break;
    }
    case MSG_CLOSE: {
        if (!error_in_extended_) {
            handle_close(conn, payload, payload_len);
        }
        break;
    }
    case MSG_FLUSH: {
        // Flush requests the server to send any pending output.
        // Since we write immediately, this is a no-op.
        break;
    }
    default: {
        SIXSEVEN_LOG_WARN("unknown frontend message type: 0x{:02x}", msg_type);
        send_error_response(conn, "ERROR", "08P01", "unrecognized message type");
        send_ready_for_query(conn, session_->ready_for_query_status());
        break;
    }
    }

    return ok(total_len);
}

void PgProtocolHandler::send_query_result(Connection& conn, const QueryResult& qr) {
    if (!qr.column_names.empty()) {
        // SELECT: send RowDescription + DataRows + CommandComplete.
        // Simple query always uses text format (empty format_codes).
        send_row_description(conn, qr);
        for (const auto& row : qr.rows) {
            send_data_row(conn, row, qr.column_types, {});
        }
    }
    send_command_complete(conn, build_command_complete_tag(qr));
}

void PgProtocolHandler::handle_simple_query(Connection& conn, std::string_view sql) {
    // Handle empty query.
    if (sql.empty()) {
        send_empty_query_response(conn);
        send_ready_for_query(conn, session_->ready_for_query_status());
        return;
    }

    // Split on semicolons — each statement produces its own result set.
    // If no non-empty statements remain after splitting, treat as empty query.
    auto statements = split_sql_statements(sql);
    if (statements.empty()) {
        send_empty_query_response(conn);
        send_ready_for_query(conn, session_->ready_for_query_status());
        return;
    }

    for (const auto& stmt : statements) {
        // Try session-level commands first (SET, SHOW, RESET, PREPARE,
        // DEALLOCATE).  EXECUTE is handled specially below.
        auto session_result = session_->try_handle_command(stmt);
        if (session_result) {
            if (!*session_result) {
                const auto& err = session_result->error();
                send_error_response(
                    conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
                session_->update_transaction_state(stmt, false);
                send_ready_for_query(conn, session_->ready_for_query_status());
                return;
            }
            send_query_result(conn, **session_result);
            session_->update_transaction_state(stmt, true);
            continue;
        }

        // Handle SQL-level EXECUTE: look up prepared statement and execute its
        // SQL through the query executor.
        auto handled = try_handle_execute(conn, stmt);
        if (handled) {
            if (!*handled) {
                // Error was already sent by try_handle_execute.
                session_->update_transaction_state(stmt, false);
                send_ready_for_query(conn, session_->ready_for_query_status());
                return;
            }
            session_->update_transaction_state(stmt, true);
            continue;
        }

        // Fall through to the query executor.
        if (!query_executor_) {
            send_error_response(conn, "ERROR", "XX000", "no query executor configured");
            session_->update_transaction_state(stmt, false);
            send_ready_for_query(conn, session_->ready_for_query_status());
            return;
        }

        auto result = query_executor_(stmt, startup_database());
        if (!result) {
            // On error, send ErrorResponse and stop processing remaining statements.
            const auto& err = result.error();
            send_error_response(
                conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
            session_->update_transaction_state(stmt, false);
            send_ready_for_query(conn, session_->ready_for_query_status());
            return;
        }

        send_query_result(conn, *result);
        session_->update_transaction_state(stmt, true);
    }

    // A single ReadyForQuery at the end of the entire multi-statement batch.
    send_ready_for_query(conn, session_->ready_for_query_status());
}

// -- SQL-level EXECUTE handler ------------------------------------------------

std::optional<Result<void>> PgProtocolHandler::try_handle_execute(Connection& conn,
                                                                  const std::string& sql) {
    // Check for "EXECUTE " prefix (case-insensitive).
    if (!starts_with_ci(sql, "EXECUTE ")) {
        return std::nullopt;
    }

    // Parse: EXECUTE name [(value, ...)]
    auto rest = sql.substr(8);
    // Trim leading whitespace.
    auto start = rest.find_first_not_of(" \t");
    if (start == std::string::npos) {
        send_error_response(conn, "ERROR", "42601", "syntax error: EXECUTE requires a name");
        return Result<void>(make_error(StatusCode::PARSE_ERROR, "EXECUTE requires a name"));
    }
    rest = rest.substr(start);

    // Extract statement name.
    std::string stmt_name;
    std::string params_str;
    auto paren_pos = rest.find('(');
    if (paren_pos != std::string::npos) {
        stmt_name = rest.substr(0, paren_pos);
        // Trim trailing whitespace from name.
        auto name_end = stmt_name.find_last_not_of(" \t");
        if (name_end != std::string::npos) {
            stmt_name = stmt_name.substr(0, name_end + 1);
        }
        // Extract parameters between parens.
        auto close_paren = rest.rfind(')');
        if (close_paren != std::string::npos && close_paren > paren_pos) {
            params_str = rest.substr(paren_pos + 1, close_paren - paren_pos - 1);
        }
    } else {
        // Trim trailing whitespace.
        auto end = rest.find_last_not_of(" \t\n\r");
        stmt_name = (end != std::string::npos) ? rest.substr(0, end + 1) : rest;
    }

    // Look up the prepared statement in the session.
    const auto* stmt = session_->get_prepared_statement(stmt_name);
    if (stmt == nullptr) {
        send_error_response(
            conn, "ERROR", "26000", "prepared statement \"" + stmt_name + "\" does not exist");
        return Result<void>(make_error(StatusCode::INVALID_ARGUMENT,
                                       "prepared statement \"" + stmt_name + "\" does not exist"));
    }

    if (!query_executor_) {
        send_error_response(conn, "ERROR", "XX000", "no query executor configured");
        return Result<void>(make_error(StatusCode::INTERNAL_ERROR, "no query executor"));
    }

    // Substitute parameter expressions into the SQL.
    // SQL-level EXECUTE parameters are raw SQL expressions (e.g. 42, 'hello', NULL),
    // so they are substituted directly without additional quoting.
    std::string exec_sql = stmt->sql;
    if (!params_str.empty()) {
        auto exprs = split_execute_expressions(params_str);
        auto substituted = substitute_sql_expressions(stmt->sql, exprs);
        if (!substituted) {
            const auto& err = substituted.error();
            send_error_response(
                conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
            return Result<void>(tl::unexpected(err));
        }
        exec_sql = std::move(*substituted);
    }

    SIXSEVEN_LOG_DEBUG("EXECUTE: stmt='{}', sql='{}'", stmt_name, exec_sql);

    auto result = query_executor_(exec_sql, startup_database());
    if (!result) {
        const auto& err = result.error();
        send_error_response(conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
        return Result<void>(tl::unexpected(err));
    }

    send_query_result(conn, *result);
    return ok();
}

// -- Extended query protocol handlers -----------------------------------------

void PgProtocolHandler::handle_parse(Connection& conn, const uint8_t* payload, size_t len) {
    MessageReader reader(payload, len);

    auto stmt_name = std::string(reader.read_cstring());
    auto sql = std::string(reader.read_cstring());
    int16_t num_params = reader.read_int16();

    std::vector<uint32_t> param_oids;
    param_oids.reserve(static_cast<size_t>(num_params));
    for (int16_t i = 0; i < num_params; ++i) {
        param_oids.push_back(static_cast<uint32_t>(reader.read_int32()));
    }

    SIXSEVEN_LOG_DEBUG("Parse: name='{}', sql='{}', params={}", stmt_name, sql, num_params);

    // Store the prepared statement in the session.
    PreparedStatement stmt;
    stmt.name = stmt_name;
    stmt.sql = std::move(sql);
    stmt.param_oids = std::move(param_oids);
    session_->add_prepared_statement(stmt_name, std::move(stmt));

    send_parse_complete(conn);
}

void PgProtocolHandler::handle_bind(Connection& conn, const uint8_t* payload, size_t len) {
    MessageReader reader(payload, len);

    auto portal_name = std::string(reader.read_cstring());
    auto stmt_name = std::string(reader.read_cstring());

    // Parameter format codes (0 = text, 1 = binary).
    int16_t num_format_codes = reader.read_int16();
    std::vector<int16_t> param_formats;
    if (num_format_codes > 0) {
        param_formats.reserve(static_cast<size_t>(num_format_codes));
        for (int16_t i = 0; i < num_format_codes; ++i) {
            param_formats.push_back(reader.read_int16());
        }
    }

    // Parameter values.
    int16_t num_params = reader.read_int16();
    std::vector<std::optional<std::string>> param_values;
    param_values.reserve(static_cast<size_t>(num_params));
    for (int16_t i = 0; i < num_params; ++i) {
        int32_t param_len = reader.read_int32();
        if (param_len == -1) {
            param_values.emplace_back(std::nullopt); // NULL parameter.
        } else {
            const auto* data = reader.read_bytes(static_cast<size_t>(param_len));
            if (data != nullptr) {
                param_values.emplace_back(std::string(reinterpret_cast<const char*>(data),
                                                      static_cast<size_t>(param_len)));
            } else {
                param_values.emplace_back(std::nullopt);
            }
        }
    }

    // Result format codes.
    int16_t num_result_formats = reader.read_int16();
    std::vector<int16_t> result_formats;
    result_formats.reserve(static_cast<size_t>(num_result_formats));
    for (int16_t i = 0; i < num_result_formats; ++i) {
        result_formats.push_back(reader.read_int16());
    }

    // Per PostgreSQL Bind semantics the parameter format-code count must be
    // 0 (all text), 1 (applies to all parameters), or one per parameter.
    if (param_formats.size() > 1 && param_formats.size() != param_values.size()) {
        send_error_response(conn,
                            "ERROR",
                            "08P01",
                            "bind message has " + std::to_string(param_formats.size()) +
                                " parameter format codes but " +
                                std::to_string(param_values.size()) + " parameters");
        error_in_extended_ = true;
        return;
    }

    // Look up the prepared statement in the session.
    const auto* stmt = session_->get_prepared_statement(stmt_name);
    if (stmt == nullptr) {
        send_error_response(
            conn, "ERROR", "26000", "prepared statement \"" + stmt_name + "\" does not exist");
        error_in_extended_ = true;
        return;
    }

    SIXSEVEN_LOG_DEBUG(
        "Bind: portal='{}', stmt='{}', params={}", portal_name, stmt_name, num_params);

    Portal portal;
    portal.name = portal_name;
    portal.sql = stmt->sql;
    portal.param_values = std::move(param_values);
    portal.param_oids = stmt->param_oids;
    portal.param_format_codes = std::move(param_formats);
    portal.result_format_codes = std::move(result_formats);
    session_->add_portal(portal_name, std::move(portal));

    send_bind_complete(conn);
}

void PgProtocolHandler::handle_describe(Connection& conn, const uint8_t* payload, size_t len) {
    MessageReader reader(payload, len);

    uint8_t describe_type = reader.read_byte(); // 'S' = statement, 'P' = portal.
    auto name = std::string(reader.read_cstring());

    if (describe_type == 'S') {
        const auto* stmt = session_->get_prepared_statement(name);
        if (stmt == nullptr) {
            send_error_response(
                conn, "ERROR", "26000", "prepared statement \"" + name + "\" does not exist");
            error_in_extended_ = true;
            return;
        }
        // Send ParameterDescription.
        send_parameter_description(conn, stmt->param_oids);

        // Describe result columns: parse + bind (no execution).
        if (query_describer_) {
            auto columns = query_describer_(stmt->sql, startup_database());
            if (!columns) {
                const auto& err = columns.error();
                send_error_response(
                    conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
                error_in_extended_ = true;
                return;
            }
            if (!columns->empty()) {
                send_row_description(conn, *columns, {});
            } else {
                send_no_data(conn);
            }
        } else {
            send_no_data(conn);
        }
    } else if (describe_type == 'P') {
        const auto* portal = session_->get_portal(name);
        if (portal == nullptr) {
            send_error_response(conn, "ERROR", "34000", "portal \"" + name + "\" does not exist");
            error_in_extended_ = true;
            return;
        }
        // Describe result columns for the portal's SQL.
        if (query_describer_) {
            auto columns = query_describer_(portal->sql, startup_database());
            if (!columns) {
                const auto& err = columns.error();
                send_error_response(
                    conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
                error_in_extended_ = true;
                return;
            }
            if (!columns->empty()) {
                send_row_description(conn, *columns, portal->result_format_codes);
            } else {
                send_no_data(conn);
            }
        } else {
            send_no_data(conn);
        }
    } else {
        send_error_response(conn, "ERROR", "08P01", "invalid Describe target type");
        error_in_extended_ = true;
    }
}

void PgProtocolHandler::handle_execute(Connection& conn, const uint8_t* payload, size_t len) {
    MessageReader reader(payload, len);

    auto portal_name = std::string(reader.read_cstring());
    int32_t max_rows = reader.read_int32(); // 0 = no limit.
    (void)max_rows;                         // We execute all rows for now.

    const auto* portal_ptr = session_->get_portal(portal_name);
    if (portal_ptr == nullptr) {
        send_error_response(
            conn, "ERROR", "34000", "portal \"" + portal_name + "\" does not exist");
        error_in_extended_ = true;
        return;
    }

    const auto& portal = *portal_ptr;

    if (!query_executor_) {
        send_error_response(conn, "ERROR", "XX000", "no query executor configured");
        error_in_extended_ = true;
        return;
    }

    // Decode binary-format parameters (format code 1) into their text
    // representation per their type OID before substitution (GDB-712).
    auto decoded_params =
        decode_bind_parameters(portal.param_values, portal.param_format_codes, portal.param_oids);
    if (!decoded_params) {
        const auto& err = decoded_params.error();
        send_error_response(conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
        error_in_extended_ = true;
        return;
    }

    // Substitute parameter values into the SQL.
    auto substituted = substitute_parameters(portal.sql, *decoded_params, portal.param_oids);
    if (!substituted) {
        const auto& err = substituted.error();
        send_error_response(conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
        error_in_extended_ = true;
        return;
    }

    SIXSEVEN_LOG_DEBUG("Execute: portal='{}', sql='{}'", portal_name, *substituted);

    auto result = query_executor_(*substituted, startup_database());
    if (!result) {
        const auto& err = result.error();
        send_error_response(conn, "ERROR", std::string(status_to_sqlstate(err.code)), err.message);
        error_in_extended_ = true;
        return;
    }

    const auto& qr = *result;

    if (!qr.column_names.empty()) {
        // SELECT: send DataRows + CommandComplete (RowDescription already sent by Describe).
        for (const auto& row : qr.rows) {
            send_data_row(conn, row, qr.column_types, portal.result_format_codes);
        }
    }
    send_command_complete(conn, build_command_complete_tag(qr));
}

void PgProtocolHandler::handle_sync(Connection& conn) {
    // Sync is the end of an extended query batch.
    // Reset error state and send ReadyForQuery with session transaction state.
    error_in_extended_ = false;
    send_ready_for_query(conn, session_->ready_for_query_status());
}

void PgProtocolHandler::handle_close(Connection& conn, const uint8_t* payload, size_t len) {
    MessageReader reader(payload, len);

    uint8_t close_type = reader.read_byte(); // 'S' = statement, 'P' = portal.
    auto name = std::string(reader.read_cstring());

    if (close_type == 'S') {
        session_->remove_prepared_statement(name);
    } else if (close_type == 'P') {
        session_->remove_portal(name);
    }

    send_close_complete(conn);
}

// -- Message senders ----------------------------------------------------------

void PgProtocolHandler::send_auth_ok(Connection& conn) {
    // AuthenticationOk: type 'R', int32 length=8, int32 auth_type=0.
    MessageWriter w;
    w.begin_message('R');
    w.write_int32(0); // AuthenticationOk.
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_auth_md5_password(Connection& conn,
                                               const std::array<uint8_t, 4>& salt) {
    // AuthenticationMD5Password: type 'R', auth_type=5, 4-byte salt.
    MessageWriter w;
    w.begin_message('R');
    w.write_int32(5); // AuthenticationMD5Password.
    w.write_bytes(salt.data(), 4);
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_auth_sasl(Connection& conn) {
    // AuthenticationSASL: type 'R', auth_type=10, mechanism list.
    MessageWriter w;
    w.begin_message('R');
    w.write_int32(10); // AuthenticationSASL.
    w.write_cstring("SCRAM-SHA-256");
    w.write_byte(0); // Terminating null for mechanism list.
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_auth_sasl_continue(Connection& conn, const std::string& data) {
    // AuthenticationSASLContinue: type 'R', auth_type=11, data.
    MessageWriter w;
    w.begin_message('R');
    w.write_int32(11); // AuthenticationSASLContinue.
    w.write_bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_auth_sasl_final(Connection& conn, const std::string& data) {
    // AuthenticationSASLFinal: type 'R', auth_type=12, data.
    MessageWriter w;
    w.begin_message('R');
    w.write_int32(12); // AuthenticationSASLFinal.
    w.write_bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_parameter_status(Connection& conn,
                                              std::string_view name,
                                              std::string_view value) {
    MessageWriter w;
    w.begin_message('S');
    w.write_cstring(name);
    w.write_cstring(value);
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_backend_key_data(Connection& conn) {
    MessageWriter w;
    w.begin_message('K');
    w.write_int32(backend_pid_);
    w.write_int32(secret_key_);
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_ready_for_query(Connection& conn, char status) {
    MessageWriter w;
    w.begin_message('Z');
    w.write_byte(static_cast<uint8_t>(status));
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_row_description(Connection& conn, const QueryResult& result) {
    MessageWriter w;
    w.begin_message('T');
    w.write_int16(static_cast<int16_t>(result.column_names.size()));
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        w.write_cstring(result.column_names[i]); // Column name.
        w.write_int32(0);                        // Table OID (0 = not a table column).
        w.write_int16(0);                        // Column attribute number.
        w.write_int32(static_cast<int32_t>(type_to_pg_oid(result.column_types[i]))); // Type OID.
        w.write_int16(-1); // Type size (-1 = variable length).
        w.write_int32(-1); // Type modifier.
        w.write_int16(0);  // Format code (0 = text).
    }
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_row_description(Connection& conn,
                                             const std::vector<ColumnDescription>& columns,
                                             const std::vector<int16_t>& format_codes) {
    MessageWriter w;
    w.begin_message('T');
    w.write_int16(static_cast<int16_t>(columns.size()));
    for (size_t i = 0; i < columns.size(); ++i) {
        w.write_cstring(columns[i].name); // Column name.
        w.write_int32(0);                 // Table OID.
        w.write_int16(0);                 // Column attribute number.
        w.write_int32(static_cast<int32_t>(type_to_pg_oid(columns[i].type_id))); // Type OID.
        w.write_int16(-1);                                                       // Type size.
        w.write_int32(-1);                                                       // Type modifier.
        w.write_int16(resolve_format_code(format_codes, i));                     // Format code.
    }
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_data_row(Connection& conn,
                                      const std::vector<Value>& row,
                                      const std::vector<TypeId>& /*types*/,
                                      const std::vector<int16_t>& format_codes) {
    MessageWriter w;
    w.begin_message('D');
    w.write_int16(static_cast<int16_t>(row.size()));
    for (size_t i = 0; i < row.size(); ++i) {
        const auto& val = row[i];
        if (val.is_null()) {
            w.write_int32(-1); // NULL indicator.
        } else if (resolve_format_code(format_codes, i) == 1) {
            // Binary format.
            auto bin = value_to_pg_binary(val);
            w.write_int32(static_cast<int32_t>(bin.size()));
            w.write_bytes(bin.data(), bin.size());
        } else {
            // Text format.
            std::string text = value_to_pg_text(val);
            w.write_int32(static_cast<int32_t>(text.size()));
            w.write_bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        }
    }
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_command_complete(Connection& conn, const std::string& tag) {
    MessageWriter w;
    w.begin_message('C');
    w.write_cstring(tag);
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_error_response(Connection& conn,
                                            std::string_view severity,
                                            std::string_view sqlstate,
                                            std::string_view message) {
    MessageWriter w;
    w.begin_message('E');
    w.write_byte('S');
    w.write_cstring(severity); // Severity.
    w.write_byte('V');
    w.write_cstring(severity); // Severity (non-localized).
    w.write_byte('C');
    w.write_cstring(sqlstate); // SQLSTATE.
    w.write_byte('M');
    w.write_cstring(message); // Message.
    w.write_byte(0);          // Terminator.
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_empty_query_response(Connection& conn) {
    MessageWriter w;
    w.begin_message('I');
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_parse_complete(Connection& conn) {
    MessageWriter w;
    w.begin_message('1');
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_bind_complete(Connection& conn) {
    MessageWriter w;
    w.begin_message('2');
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_close_complete(Connection& conn) {
    MessageWriter w;
    w.begin_message('3');
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_no_data(Connection& conn) {
    MessageWriter w;
    w.begin_message('n');
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

void PgProtocolHandler::send_parameter_description(Connection& conn,
                                                   const std::vector<uint32_t>& param_oids) {
    MessageWriter w;
    w.begin_message('t');
    w.write_int16(static_cast<int16_t>(param_oids.size()));
    for (uint32_t oid : param_oids) {
        w.write_int32(static_cast<int32_t>(oid));
    }
    auto msg = w.finish();
    conn.enqueue_write(msg.data(), msg.size());
}

} // namespace sixseven
