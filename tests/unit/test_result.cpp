#include "sixseven/common/result.h"

#include <gtest/gtest.h>

using namespace sixseven;

TEST(Result, SuccessConstruction) {
    Result<int> r = ok(42);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 42);
}

TEST(Result, ErrorConstruction) {
    Result<int> r = make_error(StatusCode::NOT_FOUND, "item missing");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
    EXPECT_EQ(r.error().message, "item missing");
}

TEST(Result, ErrorHasSourceLocation) {
    Result<int> r = make_error(StatusCode::INTERNAL_ERROR, "oops");
    ASSERT_FALSE(r.has_value());
    // source_location should point to this file
    std::string file = r.error().location.file_name();
    EXPECT_TRUE(file.find("test_result.cpp") != std::string::npos);
}

TEST(Result, ValueAccess) {
    Result<std::string> r = ok(std::string("hello"));
    EXPECT_EQ(*r, "hello");
}

TEST(Result, MapTransformsValue) {
    Result<int> r = ok(10);
    auto doubled = r.map([](int v) { return v * 2; });
    ASSERT_TRUE(doubled.has_value());
    EXPECT_EQ(doubled.value(), 20);
}

TEST(Result, MapPreservesError) {
    Result<int> r = make_error(StatusCode::IO_ERROR, "disk failure");
    auto mapped = r.map([](int v) { return v * 2; });
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error().code, StatusCode::IO_ERROR);
}

TEST(Result, AndThenChainsSuccess) {
    auto parse_int = [](const std::string& s) -> Result<int> {
        if (s.empty()) {
            return make_error(StatusCode::PARSE_ERROR, "empty string");
        }
        return ok(static_cast<int>(s.size()));
    };

    Result<std::string> r = ok(std::string("hello"));
    auto chained = r.and_then(parse_int);
    ASSERT_TRUE(chained.has_value());
    EXPECT_EQ(chained.value(), 5);
}

TEST(Result, AndThenPropagatesError) {
    auto parse_int = [](const std::string& /*s*/) -> Result<int> { return ok(0); };

    Result<std::string> r = make_error(StatusCode::NOT_FOUND, "missing");
    auto chained = r.and_then(parse_int);
    ASSERT_FALSE(chained.has_value());
    EXPECT_EQ(chained.error().code, StatusCode::NOT_FOUND);
}

TEST(StatusCode, NameReturnsCorrectStrings) {
    EXPECT_STREQ(status_code_name(StatusCode::OK), "OK");
    EXPECT_STREQ(status_code_name(StatusCode::NOT_FOUND), "NOT_FOUND");
    EXPECT_STREQ(status_code_name(StatusCode::ALREADY_EXISTS), "ALREADY_EXISTS");
    EXPECT_STREQ(status_code_name(StatusCode::INVALID_ARGUMENT), "INVALID_ARGUMENT");
    EXPECT_STREQ(status_code_name(StatusCode::INTERNAL_ERROR), "INTERNAL_ERROR");
    EXPECT_STREQ(status_code_name(StatusCode::NOT_IMPLEMENTED), "NOT_IMPLEMENTED");
    EXPECT_STREQ(status_code_name(StatusCode::IO_ERROR), "IO_ERROR");
    EXPECT_STREQ(status_code_name(StatusCode::PARSE_ERROR), "PARSE_ERROR");
    EXPECT_STREQ(status_code_name(StatusCode::TYPE_ERROR), "TYPE_ERROR");
    EXPECT_STREQ(status_code_name(StatusCode::CONSTRAINT_VIOLATION), "CONSTRAINT_VIOLATION");
    EXPECT_STREQ(status_code_name(StatusCode::TXN_CONFLICT), "TXN_CONFLICT");
    EXPECT_STREQ(status_code_name(StatusCode::TXN_ABORTED), "TXN_ABORTED");
}
