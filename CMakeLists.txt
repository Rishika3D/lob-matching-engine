cmake_minimum_required(VERSION 3.20)
project(lob_backtester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Performance numbers from an unoptimized build are meaningless, so default to
# Release when the caller did not choose a build type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build the GoogleTest suite" ON)
option(LOB_BUILD_BENCH "Build the benchmark" ON)
option(LOB_BUILD_DEMO  "Build the demo executable" ON)
option(LOB_SANITIZE    "Build with AddressSanitizer + UBSan" OFF)

add_library(lob_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
elseif(MSVC)
    target_compile_options(lob_warnings INTERFACE /W4)
endif()

if(LOB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Core library: order book + matching engine
add_library(lob
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PRIVATE lob_warnings)

if(LOB_BUILD_DEMO)
    add_executable(lob_demo src/main.cpp)
    target_link_libraries(lob_demo PRIVATE lob lob_warnings)
endif()

if(LOB_BUILD_BENCH)
    add_executable(lob_bench bench/benchmark.cpp)
    target_link_libraries(lob_bench PRIVATE lob lob_warnings)
    target_compile_options(lob_bench PRIVATE -O2)
endif()

if(LOB_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)

    add_executable(lob_tests
        tests/test_matching.cpp
        tests/test_validation.cpp
        tests/test_integrity.cpp
        tests/test_differential.cpp
    )
    target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(lob_tests)
endif()
