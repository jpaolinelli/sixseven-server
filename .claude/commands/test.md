Build and run all GioDB unit tests.

Run: `export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset default && cmake --build build/debug --target giodb_unit_tests && ctest --test-dir build/debug --output-on-failure`

Report the test results summary. If any tests fail, show the failure details and suggest fixes.
