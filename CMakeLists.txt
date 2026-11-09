cmake_minimum_required(VERSION 3.28)
project(spsc-queue)

set(CMAKE_CXX_STANDARD 26)

add_executable(${PROJECT_NAME} tests.cpp)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP true
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

target_link_libraries(${PROJECT_NAME} gtest gtest_main)