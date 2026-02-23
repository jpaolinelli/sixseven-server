cmake_minimum_required(VERSION 3.25)
project(giodb VERSION 0.1.0 LANGUAGES CXX)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings for Clang and GCC
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Testing
enable_testing()

# Subdirectories
add_subdirectory(src)
add_subdirectory(tests)

# ---------- Code quality targets (format-check, format-fix, tidy) ----------

set(GIODB_LLVM_VERSION 20)

find_program(CLANG_FORMAT_BIN NAMES clang-format-${GIODB_LLVM_VERSION} clang-format)
find_program(CLANG_TIDY_BIN NAMES clang-tidy-${GIODB_LLVM_VERSION} clang-tidy)

if(CLANG_FORMAT_BIN)
    add_custom_target(format-check
        COMMAND find ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests
                -name "*.cpp" -o -name "*.h"
                | xargs ${CLANG_FORMAT_BIN} --dry-run --Werror
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking formatting with ${CLANG_FORMAT_BIN}"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(format-fix
        COMMAND find ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests
                -name "*.cpp" -o -name "*.h"
                | xargs ${CLANG_FORMAT_BIN} -i
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Fixing formatting with ${CLANG_FORMAT_BIN}"
        VERBATIM
        USES_TERMINAL
    )
else()
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format-${GIODB_LLVM_VERSION} not found. Install LLVM ${GIODB_LLVM_VERSION}."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
    add_custom_target(format-fix
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format-${GIODB_LLVM_VERSION} not found. Install LLVM ${GIODB_LLVM_VERSION}."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()

if(CLANG_TIDY_BIN)
    add_custom_target(tidy
        COMMAND find ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests
                -name "*.cpp"
                | xargs ${CLANG_TIDY_BIN} -p ${CMAKE_BINARY_DIR}
                "--warnings-as-errors=bugprone-use-after-move,bugprone-dangling-handle,clang-analyzer-core.*"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy with ${CLANG_TIDY_BIN}"
        VERBATIM
        USES_TERMINAL
    )
else()
    add_custom_target(tidy
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-tidy-${GIODB_LLVM_VERSION} not found. Install LLVM ${GIODB_LLVM_VERSION}."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()
