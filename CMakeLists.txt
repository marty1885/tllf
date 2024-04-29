cmake_minimum_required(VERSION 3.20)
project(tllf)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)

add_library(tllf)
target_include_directories(tllf PUBLIC .)
target_precompile_headers(tllf PRIVATE tllf/pch.hpp)
target_sources(tllf PRIVATE
    tllf/tllf.cpp
)
target_link_libraries(tllf PRIVATE Drogon::Drogon)

add_subdirectory(example)