cmake_minimum_required(VERSION 4.2)
project(kvstorage)

set(CMAKE_CXX_STANDARD 20)

add_library(kvstorage STATIC
        src/DB.cpp
)

target_include_directories(kvstorage
        PUBLIC include
)

include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.18.0
)

FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(kvstorage_tests
        tests/sstable_test.cpp
)

target_link_libraries(
        kvstorage_tests
        PRIVATE
        kvstorage
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(kvstorage_tests)

include(FetchContent)

FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.5
)

set(BENCHMARK_ENABLE_TESTING OFF)

FetchContent_MakeAvailable(benchmark)

add_executable(kvstorage_benchmarks
        benchmarks/integrated_benchmark.cpp
)

target_link_libraries(
        kvstorage_benchmarks
        PRIVATE
        kvstorage
        benchmark::benchmark_main
)

