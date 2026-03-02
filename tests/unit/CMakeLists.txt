# GioDB unit tests — split into dev and QA targets

file(GLOB ALL_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")
file(GLOB QA_TEST_SOURCES CONFIGURE_DEPENDS "test_qa_*.cpp")

set(DEV_TEST_SOURCES ${ALL_TEST_SOURCES})
list(REMOVE_ITEM DEV_TEST_SOURCES ${QA_TEST_SOURCES})

# Dev unit tests (~90 files, excludes test_qa_*.cpp)
add_executable(giodb_unit_tests ${DEV_TEST_SOURCES})
target_link_libraries(giodb_unit_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

# QA regression tests (~75 files, only test_qa_*.cpp)
add_executable(giodb_qa_tests ${QA_TEST_SOURCES})
target_link_libraries(giodb_qa_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

include(GoogleTest)
gtest_discover_tests(giodb_unit_tests PROPERTIES LABELS "unit")
gtest_discover_tests(giodb_qa_tests PROPERTIES LABELS "qa")
