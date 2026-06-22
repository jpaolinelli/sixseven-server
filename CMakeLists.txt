cmake_minimum_required(VERSION 3.25)
project(sixseven VERSION 0.1.0 LANGUAGES CXX)

# Compiler cache (ccache) — speeds up rebuilds across worktrees/presets.
# No-op if ccache is not installed. Install with: brew install ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

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

# GCC-specific: suppress known false positives in libstdc++ headers
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-stringop-overflow -Wno-missing-field-initializers)
endif()

# Windows: suppress MSVC CRT deprecation warnings so POSIX-compat names
# (open, close, read, write, unlink, strerror, getpid, …) compile without
# -Werror,-Wdeprecated-declarations errors. These must be set globally
# (before any CRT header is included), which a per-header #define can't
# guarantee.
if(WIN32)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATION_WARNINGS
    )
endif()

# Compile-time log level: TRACE for Debug (and Debug-based asan/tsan presets),
# INFO for Release.  Without this, spdlog defaults to SPDLOG_LEVEL_INFO and
# SIXSEVEN_LOG_TRACE/DEBUG compile to no-ops — see GDB-892.
# Note: the asan and tsan presets both use CMAKE_BUILD_TYPE=Debug, so they
# automatically get SPDLOG_LEVEL_TRACE and expose all debug/trace call sites
# to compile-time checking.
add_compile_definitions(
    SPDLOG_ACTIVE_LEVEL=$<IF:$<CONFIG:Debug>,SPDLOG_LEVEL_TRACE,SPDLOG_LEVEL_INFO>
)

# Testing
enable_testing()

# Subdirectories
add_subdirectory(src)
add_subdirectory(tests)

# ---------- Code quality targets (format-check, format-fix, tidy) ----------

set(SIXSEVEN_LLVM_VERSION 20)

find_program(CLANG_FORMAT_BIN NAMES clang-format-${SIXSEVEN_LLVM_VERSION} clang-format)
find_program(CLANG_TIDY_BIN NAMES clang-tidy-${SIXSEVEN_LLVM_VERSION} clang-tidy)

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
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format-${SIXSEVEN_LLVM_VERSION} not found. Install LLVM ${SIXSEVEN_LLVM_VERSION}."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
    add_custom_target(format-fix
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format-${SIXSEVEN_LLVM_VERSION} not found. Install LLVM ${SIXSEVEN_LLVM_VERSION}."
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
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-tidy-${SIXSEVEN_LLVM_VERSION} not found. Install LLVM ${SIXSEVEN_LLVM_VERSION}."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()
