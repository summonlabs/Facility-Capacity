// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test framework.
//
// No external dependency, no timing logic, no timeout of any kind: every test
// runs to completion. A hang is a defect to diagnose, never something to
// terminate. Property tests take an explicit seed so a failing case can be
// reproduced exactly.
//
// Assertions are evaluated inside function templates rather than inside an `if`
// written at the macro call site. A test that compares two compile-time
// constants is perfectly reasonable, and an `if` whose condition the compiler
// can fold would be reported as C4127 and, with warnings as errors, would fail
// the build for the wrong reason.

#ifndef FACILITY_CAPACITY_TESTS_TEST_FRAMEWORK_HPP
#define FACILITY_CAPACITY_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/result.hpp"

namespace ftest {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

std::vector<TestCase>& registry();
int register_test(const char* suite, const char* name, void (*function)());

/// Thrown by FT_REQUIRE to abandon the remainder of a test body.
struct TestAborted {};

void fail(const char* file, int line, const std::string& message);
void note(const std::string& message);

/// Runs the whole suite. Returns the process exit status.
int run_all(int argc, char** argv);

/// Seed used by property tests in the current run.
std::uint64_t current_seed();

/// Records context so a property-test failure names the exact case.
void set_case_context(const std::string& context);

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) { register_test(suite, name, function); }
};

// ---------------------------------------------------------------------------
// Value rendering
// ---------------------------------------------------------------------------

std::string render(const std::string& value);
std::string render(std::string_view value);
std::string render(const char* value);
std::string render(bool value);
std::string render(const std::error_code& value);

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// Renders any value for a failure message: streamable values directly,
/// iterable values element by element, anything else as a placeholder.
template <class T>
std::string render(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (requires { std::begin(value); std::end(value); }) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : value) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append(render(item));
    }
    out.push_back(']');
    return out;
  } else {
    return "<value>";
  }
}

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------

void check(bool ok, const std::string& message, const char* file, int line);

void require(bool ok, const std::string& message, const char* file, int line);

template <class Actual, class Expected>
void check_equal(const Actual& actual, const Expected& expected, const char* expression, const char* file,
                 int line) {
  if (actual == expected) {
    return;
  }
  std::ostringstream stream;
  stream << "CHECK_EQ failed: " << expression << " (actual=" << render(actual)
         << ", expected=" << render(expected) << ")";
  fail(file, line, stream.str());
}

template <class Actual, class Expected>
void check_not_equal(const Actual& actual, const Expected& expected, const char* expression,
                     const char* file, int line) {
  if (!(actual == expected)) {
    return;
  }
  std::ostringstream stream;
  stream << "CHECK_NE failed: " << expression << " (both=" << render(actual) << ")";
  fail(file, line, stream.str());
}

template <class ResultLike>
void check_ok(const ResultLike& result, const char* expression, const char* file, int line) {
  if (result.has_value()) {
    return;
  }
  fail(file, line, std::string("expected success but got ") + result.error().to_string() + " from " +
                       expression);
}

template <class ResultLike>
void require_ok(const ResultLike& result, const char* expression, const char* file, int line) {
  if (result.has_value()) {
    return;
  }
  const std::string message =
      std::string("REQUIRE_OK failed: ") + expression + " -> " + result.error().to_string();
  fail(file, line, message);
  throw TestAborted{};
}

template <class ResultLike>
void check_error(const ResultLike& result, dccp::facility_capacity::ErrorCode expected,
                 const char* expression, const char* file, int line) {
  if (!result.has_value()) {
    if (result.error().code() == expected) {
      return;
    }
    fail(file, line, std::string("expected ") +
                        std::string(dccp::facility_capacity::error_code_name(expected)) + " from " +
                        expression + " but got " + result.error().to_string());
    return;
  }
  fail(file, line, std::string("expected failure ") +
                      std::string(dccp::facility_capacity::error_code_name(expected)) +
                      " but the call succeeded: " + expression);
}

}  // namespace ftest

#define FT_TEST(suite_name, case_name)                                                              \
  static void suite_name##_##case_name##_body();                                                    \
  static const ::ftest::Registrar suite_name##_##case_name##_registrar(#suite_name, #case_name,      \
                                                                      &suite_name##_##case_name##_body); \
  static void suite_name##_##case_name##_body()

#define FT_FAIL(message) ::ftest::fail(__FILE__, __LINE__, (message))

#define FT_CHECK(condition) \
  ::ftest::check(static_cast<bool>(condition), std::string("CHECK failed: ") + #condition, __FILE__, __LINE__)

#define FT_REQUIRE(condition) \
  ::ftest::require(static_cast<bool>(condition), std::string("REQUIRE failed: ") + #condition, __FILE__, __LINE__)

#define FT_CHECK_EQ(actual, expected) \
  ::ftest::check_equal((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define FT_CHECK_NE(actual, expected) \
  ::ftest::check_not_equal((actual), (expected), #actual " != " #expected, __FILE__, __LINE__)

#define FT_CHECK_OK(expression) ::ftest::check_ok((expression), #expression, __FILE__, __LINE__)

#define FT_REQUIRE_OK(expression) ::ftest::require_ok((expression), #expression, __FILE__, __LINE__)

#define FT_CHECK_ERROR(expression, expected_code) \
  ::ftest::check_error((expression), (expected_code), #expression, __FILE__, __LINE__)

#endif  // FACILITY_CAPACITY_TESTS_TEST_FRAMEWORK_HPP
