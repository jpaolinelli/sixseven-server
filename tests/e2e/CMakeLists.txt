# SixSevenDB end-to-end tests
#
# These tests launch real sixseven-server child processes and verify observable
# behaviour via the pg-wire protocol.  They are labelled "e2e" so CTest can
# run them in isolation with:
#
#   ctest --test-dir build/debug -L e2e
#
# On Windows the scenarios that require POSIX kill(SIGKILL) or rely on
# POSIX socket semantics are skipped at runtime via GTEST_SKIP().  They
# exercise the real behaviour on POSIX/CI.

add_executable(sixseven_e2e_tests
    test_e2e_durability.cpp
    test_e2e_replication.cpp
)

target_link_libraries(sixseven_e2e_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        sixseven
        sixseven_cli_lib
)

# Inject the path to the server binary so tests can launch it without
# hard-coding a build-directory layout.
target_compile_definitions(sixseven_e2e_tests
    PRIVATE
        SIXSEVEN_SERVER_BINARY="$<TARGET_FILE:sixseven-server>"
)

# The e2e target must be built after the server binary is available.
add_dependencies(sixseven_e2e_tests sixseven-server)

include(GoogleTest)
gtest_discover_tests(sixseven_e2e_tests PROPERTIES LABELS "e2e")
