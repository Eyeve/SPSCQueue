cmake_minimum_required(VERSION 3.28)

project(spsc-queue)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Google Test
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP true
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Google Benchmark
FetchContent_Declare(
        googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        mimalloc
        URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.6.1.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(
        googletest
        googlebenchmark
        mimalloc
)

# Tests
add_executable(${PROJECT_NAME}-test
        tests.cpp
)

target_link_libraries(${PROJECT_NAME}-test
        PRIVATE
        GTest::gtest
        GTest::gtest_main
        mimalloc
)

# Benchmarks
add_executable(${PROJECT_NAME}-bench
        # benchmarks.cpp
        # source/SPSCQueueEx.h
        spsc_bench.cpp
)

target_link_libraries(${PROJECT_NAME}-bench
        PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        mimalloc
)

target_compile_options(${PROJECT_NAME}-bench PRIVATE
        -O3
        -DNDEBUG
)
