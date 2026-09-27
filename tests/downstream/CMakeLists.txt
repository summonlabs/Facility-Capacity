# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# An independent consumer of the installed Facility Capacity package.
#
# This project is deliberately separate from the Facility Capacity source tree.
# It resolves the library through `find_package(facility_capacity)` against an
# installed prefix, so it proves that the exported package, the installed
# headers and the installed library are usable on their own.

cmake_minimum_required(VERSION 3.20)

project(FacilityCapacityDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(facility_capacity 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE dccp::facility_capacity)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
endif()
