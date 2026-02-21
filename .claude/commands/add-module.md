Add a new source file to a GioDB module. Follow these conventions:

1. Create the header in `include/giodb/<module>/<name>.h` with `#pragma once` and `namespace giodb {}`
2. Create the source in `src/<module>/<name>.cpp` including the header
3. Create the test in `tests/unit/test_<name>.cpp` with at least one test
4. Add the source to `src/<module>/CMakeLists.txt`
5. Add the test to `tests/unit/CMakeLists.txt`
6. Use `Result<T>` for error handling, never throw exceptions
7. Use `GIODB_LOG_*` macros for logging

Ask which module and file name to use if not specified.
