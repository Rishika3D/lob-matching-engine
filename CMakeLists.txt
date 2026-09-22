cmake_minimum_required(VERSION 3.20)
project(lob_backtester CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core library: order book + matching engine
add_library(lob
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(lob PUBLIC include)

# Demo executable
add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob)

# Throughput / latency benchmark
add_executable(lob_bench bench/benchmark.cpp)
target_link_libraries(lob_bench PRIVATE lob)
target_compile_options(lob_bench PRIVATE -O2)
