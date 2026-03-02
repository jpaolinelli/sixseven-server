# GioDB dev unit tests (files NOT matching test_qa_*.cpp)

file(GLOB DEV_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(giodb_unit_tests ${DEV_TEST_SOURCES})
target_link_libraries(giodb_unit_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

include(GoogleTest)
gtest_discover_tests(giodb_unit_tests PROPERTIES LABELS "unit")
