cmake_minimum_required(VERSION 3.20)
project(tllf)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)

option(TLLF_BUILD_EXAMPLE "Build example" ON)
option(TLLF_BUILD_TESTS "Build tests" OFF)

add_library(tllf)
target_include_directories(tllf PUBLIC .)
target_precompile_headers(tllf PRIVATE tllf/pch.hpp)
target_sources(tllf PRIVATE
    tllf/tllf.cpp
    tllf/url_parser.cpp
    tllf/tool.cpp
    tllf/parsers.cpp
    tllf/utils.cpp
)
target_link_libraries(tllf PRIVATE Drogon::Drogon)

if(TLLF_BUILD_EXAMPLE)
    add_subdirectory(example)
endif()

if(TLLF_BUILD_TESTS)
    add_subdirectory(tests)
endif()