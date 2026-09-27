// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real operating-system child processes for the multiprocess and crash-recovery
// tests.
//
// The child is this same test executable re-entered with `--child <name>`.
// Standard output and standard error are redirected to a *file*, not a pipe:
// a pipe that fills while the parent waits would deadlock, and this suite has
// no timeouts by design, so a hang would be a real hang.

#ifndef FACILITY_CAPACITY_TESTS_CHILD_PROCESS_HPP
#define FACILITY_CAPACITY_TESTS_CHILD_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ftest {

/// Registers a child entry point. `run_all` dispatches `--child <name>` to it.
using ChildFunction = int (*)(const std::vector<std::string>&);

void register_child(const char* name, ChildFunction function);

/// Runs a registered child entry point. Returns -1 when the name is unknown.
int run_child(const std::string& name, const std::vector<std::string>& arguments);

/// A running or finished child process.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  /// Starts this test executable as `--child <name> <arguments...>`, with
  /// standard output and standard error appended to `output_file`.
  static ChildProcess start(const std::string& child_name, const std::vector<std::string>& arguments,
                            const std::filesystem::path& output_file);

  /// Starts an arbitrary program with `arguments`, with standard output and
  /// standard error appended to `output_file`. Used to drive the installed
  /// command-line tool as a real process.
  static ChildProcess start_program(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments,
                                    const std::filesystem::path& output_file);

  bool running() const noexcept { return running_; }

  /// Waits for the child to end and returns its exit status.
  int wait();

  /// Terminates the child immediately, as a crash would. Relinquishes any lock
  /// the child held, because the operating system releases it when the process
  /// ends.
  void terminate();

  /// Waits for `output_file` to contain `marker`, reading it from disk. The
  /// caller supplies a bounded number of attempts so a genuine hang is reported
  /// rather than waited on for ever.
  static bool wait_for_marker(const std::filesystem::path& output_file, const std::string& marker,
                              int attempts);

  /// The contents of the child's output file.
  std::string output() const;

  const std::filesystem::path& output_file() const noexcept { return output_file_; }

 private:
  void release() noexcept;

#if defined(_WIN32)
  void* process_ = nullptr;
#else
  int pid_ = -1;
#endif
  bool running_ = false;
  int exit_code_ = -1;
  std::filesystem::path output_file_;
};

/// The absolute path of the running test executable.
std::filesystem::path test_executable_path();

}  // namespace ftest

#endif  // FACILITY_CAPACITY_TESTS_CHILD_PROCESS_HPP
