# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Facility Capacity test suites.
#
# One executable carries every suite so that the multiprocess tests can re-enter
# the same binary as a child process. No test carries a timeout: a hang is a
# defect to diagnose, never something to terminate.

set(FCAP_TEST_SOURCES
  test_main.cpp
  test_support.cpp
  child_process.cpp
  test_units.cpp
  test_identity.cpp
  test_digest.cpp
  test_evidence.cpp
  test_reserve_constraint.cpp
  test_derivation.cpp
  test_model.cpp
  test_revalidate.cpp
  test_canonical.cpp
  test_diff_explain.cpp
  test_store.cpp
  test_recovery.cpp
  test_property.cpp
  test_adversarial.cpp
  test_concurrency.cpp
  test_multiprocess.cpp
  test_cli.cpp)

add_executable(fcap_tests ${FCAP_TEST_SOURCES})
target_include_directories(fcap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fcap_tests PRIVATE facility_capacity fcap_warnings)

add_test(NAME fcap_tests COMMAND fcap_tests)

# The CLI suite runs the installed-or-built tool as a real process, so it needs
# the tool's path.
if(TARGET fcap_cli)
  target_compile_definitions(fcap_tests PRIVATE FCAP_TEST_CLI_PATH="$<TARGET_FILE:fcap_cli>")
else()
  target_compile_definitions(fcap_tests PRIVATE FCAP_TEST_CLI_PATH="")
endif()

# ---------------------------------------------------------------------------
# Independent consumer of the installed package.
#
# `downstream/` is a standalone CMake project that resolves the package through
# `find_package(facility_capacity)` against an installed prefix. It is
# deliberately NOT part of this build: resolving it from the source tree would
# prove nothing. The validation procedure configures, builds and runs it
# against the installed prefix.
# ---------------------------------------------------------------------------
