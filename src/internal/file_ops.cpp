// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "internal/file_ops.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "dccp/facility_capacity/limits.hpp"

namespace dccp::facility_capacity::internal::file_ops {
namespace {

Error io_error(std::string_view operation, const std::filesystem::path& path) {
  Error error = Error::make(ErrorCode::io_failure, operation);
  error.with_constraint(path.filename().string());
  return error;
}

Error rejected(std::string_view reason, const std::filesystem::path& path) {
  Error error = Error::make(ErrorCode::path_rejected, reason);
  error.with_constraint(path.filename().string());
  return error;
}

#if defined(_WIN32)

Error from_win32(std::string_view operation, const std::filesystem::path& path, DWORD code) {
  switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
      return Error::make(ErrorCode::not_found, operation);
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
      return Error::make(ErrorCode::permission_denied, operation);
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return Error::make(ErrorCode::already_exists, operation);
    case ERROR_LOCK_VIOLATION:
      return Error::make(ErrorCode::lock_conflict, operation);
    case ERROR_DISK_FULL:
      return Error::make(ErrorCode::limit_exceeded, "the volume is full");
    default:
      break;
  }
  Error error = Error::make(ErrorCode::io_failure, operation);
  error.with_constraint(std::string(path.filename().string()) + " win32=" + std::to_string(code));
  return error;
}

struct Win32Handle {
  HANDLE value = INVALID_HANDLE_VALUE;

  ~Win32Handle() {
    if (value != INVALID_HANDLE_VALUE) {
      ::CloseHandle(value);
    }
  }

  Win32Handle(const Win32Handle&) = delete;
  Win32Handle& operator=(const Win32Handle&) = delete;
  Win32Handle() = default;
};

bool attributes_of(const std::filesystem::path& path, DWORD& attributes) noexcept {
  const DWORD value = ::GetFileAttributesW(path.c_str());
  if (value == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  attributes = value;
  return true;
}

#else

Error from_errno(std::string_view operation, const std::filesystem::path& path, int code) {
  switch (code) {
    case ENOENT:
    case ENOTDIR:
      return Error::make(ErrorCode::not_found, operation);
    case EACCES:
    case EPERM:
    case ELOOP:
      return Error::make(ErrorCode::permission_denied, operation);
    case EEXIST:
      return Error::make(ErrorCode::already_exists, operation);
    case EAGAIN:
      return Error::make(ErrorCode::lock_conflict, operation);
    case ENOSPC:
      return Error::make(ErrorCode::limit_exceeded, "the volume is full");
    default:
      break;
  }
  Error error = Error::make(ErrorCode::io_failure, operation);
  error.with_constraint(std::string(path.filename().string()) + " errno=" + std::to_string(code));
  return error;
}

#endif

std::atomic<std::uint64_t> g_sequence{0};

}  // namespace

struct FileLock::Handle {
#if defined(_WIN32)
  HANDLE file = INVALID_HANDLE_VALUE;
#else
  int descriptor = -1;
#endif
};

FileLock::FileLock() noexcept = default;

FileLock::FileLock(std::unique_ptr<Handle> handle) noexcept : handle_(std::move(handle)) {}

FileLock::FileLock(FileLock&& other) noexcept : handle_(std::move(other.handle_)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = std::move(other.handle_);
  }
  return *this;
}

FileLock::~FileLock() { release(); }

bool FileLock::held() const noexcept { return handle_ != nullptr; }

void FileLock::release() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  if (handle_->file != INVALID_HANDLE_VALUE) {
    OVERLAPPED overlapped{};
    ::UnlockFileEx(handle_->file, 0, 1, 0, &overlapped);
    ::CloseHandle(handle_->file);
    handle_->file = INVALID_HANDLE_VALUE;
  }
#else
  if (handle_->descriptor >= 0) {
    struct flock lock {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    ::fcntl(handle_->descriptor, F_SETLK, &lock);
    ::close(handle_->descriptor);
    handle_->descriptor = -1;
  }
#endif
  handle_.reset();
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& file, LockMode mode) {
  auto handle = std::make_unique<Handle>();
#if defined(_WIN32)
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  handle->file = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle->file == INVALID_HANDLE_VALUE) {
    return from_win32("could not open the lock file", file, ::GetLastError());
  }
  OVERLAPPED overlapped{};
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (mode == LockMode::Exclusive) {
    flags |= LOCKFILE_EXCLUSIVE_LOCK;
  }
  if (::LockFileEx(handle->file, flags, 0, 1, 0, &overlapped) == 0) {
    const DWORD code = ::GetLastError();
    const Error error = from_win32("could not acquire the store lock", file, code);
    return error;
  }
#else
  handle->descriptor = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  if (handle->descriptor < 0) {
    return from_errno("could not open the lock file", file, errno);
  }
  struct flock lock {};
  lock.l_type = (mode == LockMode::Exclusive) ? F_WRLCK : F_RDLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 1;
  if (::fcntl(handle->descriptor, F_SETLK, &lock) == -1) {
    const int code = errno;
    ::close(handle->descriptor);
    handle->descriptor = -1;
    return from_errno("could not acquire the store lock", file, code);
  }
#endif
  return FileLock(std::move(handle));
}

Result<std::string> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
#if defined(_WIN32)
  Win32Handle handle;
  handle.value = ::CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle.value == INVALID_HANDLE_VALUE) {
    return from_win32("could not open a store file", path, ::GetLastError());
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle.value, &information) == 0) {
    return from_win32("could not inspect a store file", path, ::GetLastError());
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
    return rejected("a symbolic link or reparse point is not a store file", path);
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
    return rejected("a directory is not a store file", path);
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle.value, &size) == 0) {
    return from_win32("could not size a store file", path, ::GetLastError());
  }
  if (size.QuadPart < 0) {
    return Error::make(ErrorCode::corruption, "a store file reports a negative size");
  }
  const std::uint64_t length = static_cast<std::uint64_t>(size.QuadPart);
  if (length > max_bytes) {
    Error error = Error::make(ErrorCode::limit_exceeded, "a store file exceeds the accepted size bound");
    error.with_constraint(path.filename().string());
    return error;
  }
  std::string buffer(static_cast<std::size_t>(length), '\0');
  std::uint64_t offset = 0;
  while (offset < length) {
    const DWORD request = static_cast<DWORD>(std::min<std::uint64_t>(length - offset, 1u << 20));
    DWORD read = 0;
    if (::ReadFile(handle.value, buffer.data() + offset, request, &read, nullptr) == 0) {
      return from_win32("could not read a store file", path, ::GetLastError());
    }
    if (read == 0) {
      Error error = Error::make(ErrorCode::truncated_input, "a store file ended before its declared size");
      error.with_constraint(path.filename().string());
      return error;
    }
    offset += read;
  }
  char extra = 0;
  DWORD extra_read = 0;
  if (::ReadFile(handle.value, &extra, 1, &extra_read, nullptr) == 0) {
    return from_win32("could not read a store file", path, ::GetLastError());
  }
  if (extra_read != 0) {
    Error error = Error::make(ErrorCode::conflict, "a store file grew while it was being read");
    error.with_constraint(path.filename().string());
    return error;
  }
  return buffer;
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    return from_errno("could not inspect a store file", path, errno);
  }
  if (S_ISLNK(status.st_mode)) {
    return rejected("a symbolic link is not a store file", path);
  }
  if (!S_ISREG(status.st_mode)) {
    return rejected("a non-regular file is not a store file", path);
  }
  const std::uint64_t length = static_cast<std::uint64_t>(status.st_size);
  if (length > max_bytes) {
    Error error = Error::make(ErrorCode::limit_exceeded, "a store file exceeds the accepted size bound");
    error.with_constraint(path.filename().string());
    return error;
  }
  int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return from_errno("could not open a store file", path, errno);
  }
  std::string buffer(static_cast<std::size_t>(length), '\0');
  std::uint64_t offset = 0;
  while (offset < length) {
    const ssize_t read = ::read(descriptor, buffer.data() + offset, static_cast<std::size_t>(length - offset));
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int code = errno;
      ::close(descriptor);
      return from_errno("could not read a store file", path, code);
    }
    if (read == 0) {
      ::close(descriptor);
      Error error = Error::make(ErrorCode::truncated_input, "a store file ended before its declared size");
      error.with_constraint(path.filename().string());
      return error;
    }
    offset += static_cast<std::uint64_t>(read);
  }
  char extra = 0;
  const ssize_t extra_read = ::read(descriptor, &extra, 1);
  ::close(descriptor);
  if (extra_read < 0) {
    return from_errno("could not read a store file", path, errno);
  }
  if (extra_read != 0) {
    Error error = Error::make(ErrorCode::conflict, "a store file grew while it was being read");
    error.with_constraint(path.filename().string());
    return error;
  }
  return buffer;
#endif
}

Result<void> write_file_flushed(const std::filesystem::path& path, std::string_view content) {
#if defined(_WIN32)
  Win32Handle handle;
  handle.value = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
  if (handle.value == INVALID_HANDLE_VALUE) {
    return from_win32("could not create a store file", path, ::GetLastError());
  }
  std::size_t offset = 0;
  while (offset < content.size()) {
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(content.size() - offset, 1u << 20));
    DWORD written = 0;
    if (::WriteFile(handle.value, content.data() + offset, request, &written, nullptr) == 0) {
      return from_win32("could not write a store file", path, ::GetLastError());
    }
    if (written == 0) {
      return io_error("could not write a store file", path);
    }
    offset += written;
  }
  if (::FlushFileBuffers(handle.value) == 0) {
    return from_win32("could not flush a store file", path, ::GetLastError());
  }
  return Status::success();
#else
  int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return from_errno("could not create a store file", path, errno);
  }
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written = ::write(descriptor, content.data() + offset, content.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int code = errno;
      ::close(descriptor);
      return from_errno("could not write a store file", path, code);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const int code = errno;
    ::close(descriptor);
    return from_errno("could not flush a store file", path, code);
  }
  if (::close(descriptor) != 0) {
    return from_errno("could not close a store file", path, errno);
  }
  return Status::success();
#endif
}

Result<void> rename_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
  if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return from_win32("could not replace a store file", to, ::GetLastError());
  }
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return from_errno("could not replace a store file", to, errno);
  }
#endif
  return Status::success();
}

Result<void> atomic_write_file(const std::filesystem::path& path, std::string_view content) {
  const std::filesystem::path directory = path.parent_path();
  for (int attempt = 0; attempt < 16; ++attempt) {
    const std::filesystem::path temporary =
        directory / ("tmp-" + process_id_token() + "-" + next_sequence_token() + ".tmp");
    const Result<void> written = write_file_flushed(temporary, content);
    if (!written.has_value()) {
      if (written.error().code() == ErrorCode::already_exists) {
        continue;
      }
      return written.error();
    }
    Result<void> renamed = rename_replace(temporary, path);
    if (!renamed.has_value()) {
      const Result<bool> removed = remove_file(temporary);
      (void)removed;
      return renamed.error();
    }
    return flush_directory(directory);
  }
  return Error::make(ErrorCode::already_exists, "could not obtain a unique temporary file name");
}

Result<void> flush_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  // Win32 exposes no directory-flush operation. The rename that precedes this
  // call is issued with MOVEFILE_WRITE_THROUGH, which does not return until the
  // metadata change has reached stable storage.
  (void)directory;
  return Status::success();
#else
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return from_errno("could not open a store directory for flushing", directory, errno);
  }
  if (::fsync(descriptor) != 0) {
    const int code = errno;
    ::close(descriptor);
    return from_errno("could not flush a store directory", directory, code);
  }
  ::close(descriptor);
  return Status::success();
#endif
}

Result<void> create_directories(const std::filesystem::path& directory) {
  std::error_code code;
  std::filesystem::create_directories(directory, code);
  if (code) {
    Error error = Error::make(ErrorCode::io_failure, "could not create a store directory");
    error.with_constraint(code.message());
    return error;
  }
  return Status::success();
}

bool is_regular_file(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  DWORD attributes = 0;
  if (!attributes_of(path, attributes)) {
    return false;
  }
  if ((attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0u) {
    return false;
  }
  return true;
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    return false;
  }
  return S_ISREG(status.st_mode);
#endif
}

bool is_link_like(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  DWORD attributes = 0;
  if (!attributes_of(path, attributes)) {
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    return false;
  }
  return S_ISLNK(status.st_mode);
#endif
}

bool is_directory(const std::filesystem::path& path) noexcept {
  std::error_code code;
  const std::filesystem::file_status status = std::filesystem::status(path, code);
  if (code) {
    return false;
  }
  return std::filesystem::is_directory(status);
}

bool exists(const std::filesystem::path& path) noexcept {
  std::error_code code;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, code);
  if (code) {
    return false;
  }
  return std::filesystem::exists(status);
}

Result<bool> remove_file(const std::filesystem::path& path) {
  std::error_code code;
  const bool removed = std::filesystem::remove(path, code);
  if (code) {
    if (code.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
      return false;
    }
    Error error = Error::make(ErrorCode::io_failure, "could not remove a store file");
    error.with_constraint(path.filename().string());
    return error;
  }
  return removed;
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  std::error_code code;
  std::filesystem::directory_iterator iterator(directory, code);
  if (code) {
    Error error = Error::make(ErrorCode::io_failure, "could not list a store directory");
    error.with_constraint(code.message());
    return error;
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const std::string name = iterator->path().filename().string();
    if (name != "." && name != "..") {
      names.push_back(name);
    }
    iterator.increment(code);
    if (code) {
      Error error = Error::make(ErrorCode::io_failure, "could not list a store directory");
      error.with_constraint(code.message());
      return error;
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string process_id_token() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
  return std::to_string(static_cast<unsigned long>(::getpid()));
#endif
}

std::string next_sequence_token() {
  const std::uint64_t value = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1u;
  return std::to_string(value);
}

}  // namespace dccp::facility_capacity::internal::file_ops
