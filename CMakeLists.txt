cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0167 NEW)

project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)

add_library(orderbook_core
    src/order_book.cpp
)

target_include_directories(orderbook_core PUBLIC include)

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

target_include_directories(replay PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(replay PRIVATE
    Boost::json
    orderbook_core
)
