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

# ODBC integration tests — requires unixODBC installed.
find_library(ODBC_LIBRARY odbc PATHS /opt/homebrew/lib)
if(ODBC_LIBRARY)
    add_executable(sixseven_odbc_tests
        test_odbc_catalog.cpp
    )

    target_include_directories(sixseven_odbc_tests
        PRIVATE /opt/homebrew/include
    )

    target_link_libraries(sixseven_odbc_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            sixseven
            ${ODBC_LIBRARY}
    )

    gtest_discover_tests(sixseven_odbc_tests PROPERTIES LABELS "odbc")
else()
    message(STATUS "unixODBC not found — skipping ODBC integration tests")
endif()
