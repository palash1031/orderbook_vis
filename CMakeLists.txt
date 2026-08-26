cmake_minimum_required(VERSION 3.20)

project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(orderbook
    src/main.cpp
)

target_include_directories(orderbook PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(orderbook PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)                              