# SixSevenDB dev unit tests (files NOT matching test_qa_*.cpp)

file(GLOB DEV_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(sixseven_unit_tests ${DEV_TEST_SOURCES})
target_link_libraries(sixseven_unit_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        sixseven
        sixseven_cli_lib
)

include(GoogleTest)
gtest_discover_tests(sixseven_unit_tests PROPERTIES LABELS "unit")
