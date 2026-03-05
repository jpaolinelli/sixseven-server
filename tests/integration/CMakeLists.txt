# SixSevenDB integration tests

add_executable(sixseven_integration_tests
    test_multi_database.cpp
    test_replication.cpp
    test_system_database.cpp
)

target_link_libraries(sixseven_integration_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        sixseven
)

include(GoogleTest)
gtest_discover_tests(sixseven_integration_tests PROPERTIES LABELS "integration")
