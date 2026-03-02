Build and run GioDB developer unit tests.

Run: `export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset default && cmake --build build/debug --target giodb_unit_tests && ./build/debug/tests/unit/giodb_unit_tests`

Report the test results summary. If any tests fail, show the failure details and suggest fixes.
