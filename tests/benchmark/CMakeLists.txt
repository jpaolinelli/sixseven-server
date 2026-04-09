# SixSevenDB benchmark tests (Google Benchmark)

find_package(benchmark CONFIG REQUIRED)

file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(sixseven_benchmarks ${BENCH_SOURCES})
target_link_libraries(sixseven_benchmarks
    PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        sixseven
)
