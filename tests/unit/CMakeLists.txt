# GioDB unit tests

add_executable(giodb_unit_tests
    test_sanity.cpp
    test_logging.cpp
    test_result.cpp
    test_config.cpp
    test_types.cpp
)

target_link_libraries(giodb_unit_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

include(GoogleTest)
gtest_discover_tests(giodb_unit_tests)
