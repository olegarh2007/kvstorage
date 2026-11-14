cmake_minimum_required(VERSION 4.2)
project(kvstorage)

set(CMAKE_CXX_STANDARD 20)

add_library(kvstorage STATIC
        src/DB.cpp
        src/wal.cpp
        src/sstable.cpp
        src/BloomFilter.cpp
)

target_include_directories(kvstorage
        PUBLIC include
)

target_compile_options(kvstorage PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
)

target_link_options(kvstorage PUBLIC
        -fsanitize=thread
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
        tests/BloomFilterTest.cpp
        tests/memfs_test.cpp
        tests/wal_test.cpp
        tests/sstable_test.cpp
        tests/db_test.cpp
        tests/db_concurrency_test.cpp
)

target_compile_options(kvstorage_tests PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
)

target_link_options(kvstorage_tests PRIVATE
        -fsanitize=thread
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

target_compile_options(benchmark PRIVATE
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
)

add_executable(kvstorage_benchmarks
        benchmarks/db_benchmark.cpp
)

target_compile_options(kvstorage_benchmarks PRIVATE
        -O2 -g -fno-omit-frame-pointer -fsanitize=thread
)

target_link_libraries(
        kvstorage_benchmarks
        PRIVATE
        kvstorage
        benchmark::benchmark_main
)
