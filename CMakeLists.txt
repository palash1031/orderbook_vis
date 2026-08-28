cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0167 NEW)

project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_BENCHMARKS "Build benchmarks" OFF)

find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)

add_library(orderbook_core
    src/order_book.cpp
    src/coinbase_parser.cpp
    src/sequence_tracker.cpp
    src/book_reconstructor.cpp
)

target_include_directories(orderbook_core PUBLIC include)

target_link_libraries(orderbook_core PUBLIC Boost::json)

add_executable(orderbook
    src/main.cpp
)

target_include_directories(orderbook PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(orderbook PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(replay
    src/replay.cpp
)

target_link_libraries(replay PRIVATE
    orderbook_core
)

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(googletest)

    add_executable(orderbook_tests
        tests/order_book_tests.cpp
        tests/coinbase_parser_tests.cpp
        tests/sequence_tracker_tests.cpp
    )

    target_link_libraries(orderbook_tests PRIVATE
        orderbook_core
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(orderbook_tests)
endif()

if(BUILD_BENCHMARKS)
    include(FetchContent)

    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(orderbook_benchmarks
        benchmarks/orderbook_benchmarks.cpp
        benchmarks/parser_benchmarks.cpp
    )

    target_link_libraries(orderbook_benchmarks PRIVATE
        orderbook_core
        benchmark::benchmark_main
    )
endif()
