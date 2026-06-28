#include "sixseven/common/parse_utils.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace sixseven {

Result<int> safe_stoi(std::string_view s) {
    try {
        return ok(std::stoi(std::string(s)));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid integer literal");
    }
}

Result<int64_t> safe_stoll(std::string_view s) {
    try {
        return ok(static_cast<int64_t>(std::stoll(std::string(s))));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid integer literal");
    }
}

Result<uint32_t> safe_stou32(std::string_view s) {
    try {
        uint64_t v = std::stoull(std::string(s));
        if (v > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
        }
        return ok(static_cast<uint32_t>(v));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid integer literal");
    }
}

Result<unsigned long> safe_stoul(std::string_view s) {
    try {
        return ok(std::stoul(std::string(s)));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid integer literal");
    }
}

Result<uint64_t> safe_stoull(std::string_view s) {
    try {
        return ok(static_cast<uint64_t>(std::stoull(std::string(s))));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "integer literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid integer literal");
    }
}

Result<double> safe_stod(std::string_view s) {
    try {
        return ok(std::stod(std::string(s)));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "float literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid float literal");
    }
}

Result<float> safe_stof(std::string_view s) {
    try {
        return ok(std::stof(std::string(s)));
    } catch (const std::out_of_range&) {
        return make_error(StatusCode::PARSE_ERROR, "float literal out of range");
    } catch (const std::invalid_argument&) {
        return make_error(StatusCode::PARSE_ERROR, "invalid float literal");
    }
}

} // namespace sixseven
