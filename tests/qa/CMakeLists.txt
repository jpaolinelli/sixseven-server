# SixSevenDB QA regression tests (test_qa_*.cpp)

file(GLOB QA_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(sixseven_qa_tests ${QA_TEST_SOURCES})
target_include_directories(sixseven_qa_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../unit
)
target_link_libraries(sixseven_qa_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        sixseven
)

include(GoogleTest)
gtest_discover_tests(sixseven_qa_tests PROPERTIES LABELS "qa")

# GDB-710: CI completion check — verifies the migrated QA suites run to
# completion without SIGABRT (exit 0). A crash or unchecked Result dereference
# inside any in-scope test will fail this gate even if gtest_discover_tests
# does not catch it.
#
# FILTER: Intentionally scoped to the 9 suites migrated to bootstrap_qa_catalog
# in GDB-710. The remaining ~59 QA suites still crash the full binary because
# they do not call bootstrap_qa_catalog() in SetUp(). Remove this filter once
# GDB-1224 is resolved (all suites migrated).
#
# NOTE: QA_GDB602.DropDefaultDB_IfExists* are excluded from the filter because
# they expose a pre-existing engine bug (tracked in GDB-1225) where
# DROP DATABASE IF EXISTS demo succeeds instead of returning
# CONSTRAINT_VIOLATION. Those are EXPECT-based failures (no abort) but would
# cause this CTest entry to return nonzero. They will be re-included once
# GDB-1225 is fixed.
add_test(
    NAME QA_BinaryRunsToCompletion
    COMMAND sixseven_qa_tests
        --gtest_filter=QA_AggregateE2E.*:QA_GDB258.*:GDB258ProviderRegistryTest.*:QA_GDB596.*:QA_GDB597.*:QA_GDB598.*:GDB599WindowFunctionQA.*:QA_GDB601.*:QA_GDB602.IfExistsAfterName_NonexistentDB_Succeeds:QA_GDB602.IfExistsAfterName_NonexistentDB_Cascade_Succeeds:QA_GDB602.IfExistsBeforeName_NonexistentDB_StillWorks:QA_GDB602.IfExistsBeforeName_NonexistentDB_Cascade_StillWorks:QA_GDB602.NoIfExists_NonexistentDB_Errors:QA_GDB602.IfExistsAfterName_NonexistentDB_Restrict_Succeeds:QA_GDB602.IfExistsAfterName_ExistingDB_StillDrops:QA_GDB602.IfExistsAfterName_ExistingDB_Cascade_StillDrops:QA_GDB602_Parser.*:QA_GDB602.MultipleDropIfExists_NoStateLeak:QA_GDB603.*
)
set_tests_properties(QA_BinaryRunsToCompletion PROPERTIES LABELS "qa")
