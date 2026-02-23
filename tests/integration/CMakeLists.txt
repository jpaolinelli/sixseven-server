# GioDB integration tests

add_executable(giodb_integration_tests
    test_multi_database.cpp
)

target_link_libraries(giodb_integration_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

include(GoogleTest)
gtest_discover_tests(giodb_integration_tests)
