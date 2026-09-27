// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "child_process.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "test_framework.hpp"

namespace ftest {
namespace {

struct ChildEntry {
  std::string name;
  ChildFunction function;
};

std::vector<ChildEntry>& child_registry() {
  static std::vector<ChildEntry> entries;
  return entries;
}

template <class T>
std::string to_native(const T& value) {
#if defined(_WIN32)
  return std::filesystem::path(value).string();
#else
  return std::string(value);
#endif
}

}  // namespace

void register_child(const char* name, ChildFunction function) {
  child_registry().push_back(ChildEntry{name, function});
}

int run_child(const std::string& name, const std::vector<std::string>& arguments) {
  for (const ChildEntry& entry : child_registry()) {
    if (entry.name == name) {
      return entry.function(arguments);
    }
  }
  return -1;
}

std::filesystem::path test_executable_path() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer);
#elif defined(__APPLE__)
  return std::filesystem::path("/proc/self/exe");
#else
  std::vector<char> buffer(4096, '\0');
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    throw std::runtime_error("could not resolve the test executable path");
  }
  return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
}

ChildProcess::~ChildProcess() { release(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
#if defined(_WIN32)
  process_ = other.process_;
  other.process_ = nullptr;
#else
  pid_ = other.pid_;
  other.pid_ = -1;
#endif
  running_ = other.running_;
  exit_code_ = other.exit_code_;
  output_file_ = std::move(other.output_file_);
  other.running_ = false;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    release();
#if defined(_WIN32)
    process_ = other.process_;
    other.process_ = nullptr;
#else
    pid_ = other.pid_;
    other.pid_ = -1;
#endif
    running_ = other.running_;
    exit_code_ = other.exit_code_;
    output_file_ = std::move(other.output_file_);
    other.running_ = false;
  }
  return *this;
}

void ChildProcess::release() noexcept {
  if (!running_) {
    return;
  }
  terminate();
}

ChildProcess ChildProcess::start(const std::string& child_name,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& output_file) {
  std::vector<std::string> prelude;
  prelude.push_back("--child");
  prelude.push_back(child_name);
  for (const std::string& argument : arguments) {
    prelude.push_back(argument);
  }
  return start_program(test_executable_path(), prelude, output_file);
}

ChildProcess ChildProcess::start_program(const std::filesystem::path& executable,
                                         const std::vector<std::string>& arguments,
                                         const std::filesystem::path& output_file) {
  ChildProcess child;
  child.output_file_ = output_file;

#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const HANDLE output = ::CreateFileW(output_file.c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("could not open the child output file");
  }
  if (::SetHandleInformation(output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == 0) {
    ::CloseHandle(output);
    throw std::runtime_error("could not mark the child output handle inheritable");
  }

  std::string command = "\"" + executable.string() + "\"";
  for (const std::string& argument : arguments) {
    command.append(" \"");
    command.append(argument);
    command.push_back('"');
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output;
  startup.hStdError = output;
  PROCESS_INFORMATION information{};

  std::wstring wide(command.begin(), command.end());
  std::vector<wchar_t> mutable_command(wide.begin(), wide.end());
  mutable_command.push_back(L'\0');

  const BOOL started = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        nullptr, &startup, &information);
  ::CloseHandle(output);
  if (started == 0) {
    throw std::runtime_error("could not start a child process");
  }
  ::CloseHandle(information.hThread);
  child.process_ = information.hProcess;
  child.running_ = true;
  return child;
#else
  const int descriptor = ::open(output_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (descriptor < 0) {
    throw std::runtime_error("could not open the child output file");
  }
  std::vector<std::string> storage;
  storage.push_back(executable.string());
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(descriptor);
    throw std::runtime_error("could not fork a child process");
  }
  if (pid == 0) {
    ::dup2(descriptor, STDOUT_FILENO);
    ::dup2(descriptor, STDERR_FILENO);
    ::close(descriptor);
    ::execve(argv[0], argv.data(), environ);
    ::_exit(127);
  }
  ::close(descriptor);
  child.pid_ = static_cast<int>(pid);
  child.running_ = true;
  return child;
#endif
}

int ChildProcess::wait() {
  if (!running_) {
    return exit_code_;
  }
#if defined(_WIN32)
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
    throw std::runtime_error("could not read the child exit code");
  }
  ::CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
  exit_code_ = static_cast<int>(code);
#else
  int status = 0;
  while (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
    // retry on interruption; there is no timeout and no alternative exit
  }
  pid_ = -1;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  running_ = false;
  return exit_code_;
}

void ChildProcess::terminate() {
  if (!running_) {
    return;
  }
#if defined(_WIN32)
  ::TerminateProcess(static_cast<HANDLE>(process_), 1);
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  ::CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
#else
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  int status = 0;
  while (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
  }
  pid_ = -1;
#endif
  running_ = false;
  exit_code_ = -1;
}

std::string ChildProcess::output() const {
  std::ifstream stream(output_file_, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool ChildProcess::wait_for_marker(const std::filesystem::path& output_file, const std::string& marker,
                                   int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    {
      std::ifstream stream(output_file, std::ios::binary);
      if (stream) {
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        if (buffer.str().find(marker) != std::string::npos) {
          return true;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace ftest
