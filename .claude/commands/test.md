Build and run SixSevenDB developer unit tests.

Run: `export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset default && cmake --build build/debug --target sixseven_unit_tests && ./build/debug/tests/unit/sixseven_unit_tests`

Report the test results summary. If any tests fail, show the failure details and suggest fixes.
