# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Facility Capacity build.
#
# DCCP Tranche 2: Facility Capacity and Placement.
#
# The project is standalone: it requires no other Summon Software Labs
# repository to build, and it has no third-party dependencies.

cmake_minimum_required(VERSION 3.20)

project(
  FacilityCapacity
  VERSION 1.0.0
  DESCRIPTION "Aggregate, generation-bound facility-capacity model: composes space, rack, power, cooling, operational-reserve and facility-service evidence into one authoritative answer"
  HOMEPAGE_URL "https://github.com/summonlabs/Facility-Capacity"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FCAP_IS_TOP_LEVEL ON)
else()
  set(FCAP_IS_TOP_LEVEL OFF)
endif()

option(FCAP_BUILD_LIBRARY "Build the facility_capacity library" ON)
option(FCAP_BUILD_TOOLS "Build the fcap inspection and administration tool" ON)
option(FCAP_BUILD_TESTS "Build the Facility Capacity test suites" ON)
option(FCAP_BUILD_EXAMPLES "Build the examples" ON)
option(FCAP_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(FCAP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FCAP_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Declared after GNUInstallDirs: CMAKE_INSTALL_LIBDIR is only defined once that
# module has run, and an empty component would turn the destination into an
# absolute path outside the install prefix.
set(FCAP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/facility_capacity"
    CACHE STRING "Directory for the installed CMake package configuration")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning configuration: strict for first-party code only.
# ---------------------------------------------------------------------------

add_library(fcap_warnings INTERFACE)
if(MSVC)
  target_compile_options(fcap_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(FCAP_WARNINGS_AS_ERRORS)
    target_compile_options(fcap_warnings INTERFACE /WX)
  endif()
  set(FCAP_ASAN_AVAILABLE OFF)
  if(FCAP_ENABLE_ASAN)
    target_compile_options(fcap_warnings INTERFACE /fsanitize=address)
    target_link_options(fcap_warnings INTERFACE /fsanitize=address)
    set(FCAP_ASAN_AVAILABLE ON)
  endif()
  target_compile_definitions(fcap_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
else()
  target_compile_options(fcap_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor)
  if(FCAP_WARNINGS_AS_ERRORS)
    target_compile_options(fcap_warnings INTERFACE -Werror)
  endif()
  set(FCAP_ASAN_AVAILABLE OFF)
  if(FCAP_ENABLE_ASAN)
    target_compile_options(fcap_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fcap_warnings INTERFACE -fsanitize=address)
    set(FCAP_ASAN_AVAILABLE ON)
  endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

set(FCAP_SOURCES
  src/clock.cpp
  src/canonical.cpp
  src/constraint.cpp
  src/derive.cpp
  src/diff.cpp
  src/digest.cpp
  src/dimension.cpp
  src/error.cpp
  src/evidence.cpp
  src/explain.cpp
  src/measured.cpp
  src/model.cpp
  src/provenance.cpp
  src/reason.cpp
  src/reserve.cpp
  src/revalidate.cpp
  src/snapshot.cpp
  src/store.cpp
  src/strong_id.cpp
  src/units.cpp
  src/internal/canonical_io.cpp
  src/internal/checked.cpp
  src/internal/codec.cpp
  src/internal/documents.cpp
  src/internal/file_ops.cpp
  src/internal/store_format.cpp
  src/internal/text.cpp)

add_library(facility_capacity STATIC ${FCAP_SOURCES})
add_library(facility_capacity::facility_capacity ALIAS facility_capacity)
# Short alias used inside this build tree only.
add_library(fcap ALIAS facility_capacity)

target_include_directories(facility_capacity
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# BUILD_INTERFACE keeps the warning interface out of the installed export set:
# consumers of the package never inherit first-party build flags.
target_link_libraries(facility_capacity PRIVATE $<BUILD_INTERFACE:fcap_warnings>)
target_compile_features(facility_capacity PUBLIC cxx_std_20)

set_target_properties(facility_capacity PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  EXPORT_NAME facility_capacity)

if(WIN32)
  target_link_libraries(facility_capacity PRIVATE advapi32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(facility_capacity PRIVATE Threads::Threads)
endif()

target_compile_definitions(facility_capacity PRIVATE FCAP_BUILDING_LIBRARY=1)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(FCAP_BUILD_TOOLS)
  add_executable(fcap_cli tools/fcapctl/main.cpp tools/fcapctl/script.cpp)
  set_target_properties(fcap_cli PROPERTIES OUTPUT_NAME fcap)
  target_include_directories(fcap_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/fcapctl)
  target_link_libraries(fcap_cli PRIVATE facility_capacity fcap_warnings)

  install(TARGETS fcap_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks
# ---------------------------------------------------------------------------

if(FCAP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(FCAP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(FCAP_BUILD_BENCHMARKS)
  add_executable(fcap_benchmarks benchmarks/capacity_benchmarks.cpp)
  target_link_libraries(fcap_benchmarks PRIVATE facility_capacity fcap_warnings)
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------

install(TARGETS facility_capacity
        EXPORT facility_capacityTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT facility_capacityTargets
        FILE facility_capacityTargets.cmake
        NAMESPACE dccp::
        DESTINATION ${FCAP_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/facility_capacityConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/facility_capacityConfig.cmake
  INSTALL_DESTINATION ${FCAP_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/facility_capacityConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/facility_capacityConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/facility_capacityConfigVersion.cmake
        DESTINATION ${FCAP_INSTALL_CMAKEDIR})

install(FILES LICENSE README.md NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
