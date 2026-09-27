// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_framework.hpp"

#include "child_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ftest {
namespace {

std::string g_current_case;
std::string g_case_context;
int g_failures = 0;
std::uint64_t g_seed = 0x5DEECE66Dull;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::cout << "FAIL " << g_current_case << " (" << file << ":" << line << ")\n";
  std::cout << "  " << message << "\n";
  if (!g_case_context.empty()) {
    std::cout << "  context: " << g_case_context << "\n";
  }
  std::cout << "  seed: " << g_seed << "\n";
  std::cout.flush();
}

void note(const std::string& message) { std::cout << "note " << g_current_case << ": " << message << "\n"; }

void set_case_context(const std::string& context) { g_case_context = context; }

std::uint64_t current_seed() { return g_seed; }

std::string render(const std::string& value) { return value; }

std::string render(std::string_view value) { return std::string(value); }

std::string render(const char* value) { return std::string(value); }

std::string render(bool value) { return value ? "true" : "false"; }

std::string render(const std::error_code& value) { return value.message(); }

void check(bool ok, const std::string& message, const char* file, int line) {
  if (!ok) {
    fail(file, line, message);
  }
}

void require(bool ok, const std::string& message, const char* file, int line) {
  if (!ok) {
    fail(file, line, message);
    throw TestAborted{};
  }
}

int run_all(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--child") {
    std::vector<std::string> arguments;
    for (int index = 3; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    const int status = run_child(argv[2], arguments);
    if (status < 0) {
      std::cout << "unknown child entry point: " << argv[2] << "\n";
      return 2;
    }
    return status;
  }

  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--seed" && index + 1 < argc) {
      g_seed = std::strtoull(argv[++index], nullptr, 10);
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument == "--list") {
      for (const TestCase& test : registry()) {
        std::cout << test.suite << "." << test.name << "\n";
      }
      return 0;
    } else {
      std::cout << "unknown argument: " << argument << "\n";
      return 2;
    }
  }
  if (g_seed == 0x5DEECE66Dull) {
    g_seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    if (g_seed == 0) {
      g_seed = 1;
    }
  }

  std::vector<TestCase> tests = registry();
  std::sort(tests.begin(), tests.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  std::size_t executed = 0;
  for (const TestCase& test : tests) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    g_current_case = full;
    g_case_context.clear();
    ++executed;
    try {
      test.function();
    } catch (const TestAborted&) {
      // Already reported by the macro that threw.
    } catch (const std::exception& error) {
      fail(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what());
    } catch (...) {
      fail(__FILE__, __LINE__, "unexpected non-standard exception");
    }
  }

  std::cout << "seed=" << g_seed << " tests=" << executed << " failures=" << g_failures << "\n";
  std::cout.flush();
  return g_failures == 0 ? 0 : 1;
}

}  // namespace ftest

int main(int argc, char** argv) { return ::ftest::run_all(argc, argv); }
