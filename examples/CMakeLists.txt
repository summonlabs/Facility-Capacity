# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Runnable examples.
#
# Each example is a standalone program that resolves the library exactly as a
# consumer does: it links the `facility_capacity` target and inherits the
# first-party warning interface when that target exists in this build. Nothing
# here is part of the installed package, and no example is enabled by another
# option: `FCAP_BUILD_EXAMPLES` is the only switch.
#
# The downstream consumer is deliberately NOT built here. It is a separate
# out-of-tree project that resolves the installed package through
# `find_package(facility_capacity)`; building it from the source tree would
# prove nothing about the installed package.

add_executable(fcap_example_quick_start quick_start.cpp)
target_link_libraries(fcap_example_quick_start PRIVATE facility_capacity)
if(TARGET fcap_warnings)
  target_link_libraries(fcap_example_quick_start PRIVATE fcap_warnings)
endif()

add_executable(fcap_example_durable_lifecycle durable_lifecycle.cpp)
target_link_libraries(fcap_example_durable_lifecycle PRIVATE facility_capacity)
if(TARGET fcap_warnings)
  target_link_libraries(fcap_example_durable_lifecycle PRIVATE fcap_warnings)
endif()
