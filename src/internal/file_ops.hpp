// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Platform file primitives used by the durable store. Private to the library.
//
// Two rules govern everything here:
//
//   1. Authoritative bytes only ever change by an atomic replacement, so a
//      reader sees the old content or the new content and never a mixture.
//   2. A lock is never held across a callback and is never upgraded. The
//      exclusive lock is taken once, at the start of a mutating operation, and
//      released before the operation returns to the caller.
//
// Symbolic links and Windows reparse points are never followed when reading
// store content: a link inside a store directory is treated as hostile input
// and refused with `path_rejected`.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_FILE_OPS_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_FILE_OPS_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity::internal::file_ops {

enum class LockMode {
  Shared,
  Exclusive,
};

/// An advisory, inter-process file lock.
///
/// Acquisition never blocks: a lock held elsewhere is reported as
/// `lock_conflict` so the caller decides what to do instead of waiting inside
/// the library. The lock is released on destruction, on `release()`, and by the
/// operating system when the process ends for any reason, including a crash.
class FileLock {
 public:
  FileLock() noexcept;
  ~FileLock();
  FileLock(FileLock&&) noexcept;
  FileLock& operator=(FileLock&&) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  /// Locks byte zero of `file`, creating the file when it is absent.
  static Result<FileLock> acquire(const std::filesystem::path& file, LockMode mode);

  bool held() const noexcept;
  void release() noexcept;

 private:
  struct Handle;
  explicit FileLock(std::unique_ptr<Handle> handle) noexcept;
  std::unique_ptr<Handle> handle_;
};

/// Reads a whole file, refusing anything larger than `max_bytes`.
///
/// The size is checked from the file's own metadata before anything is
/// allocated and the content is length-checked again after reading, so a file
/// that grows or shrinks during the read is reported rather than silently
/// truncated. `actual_bytes` receives the number of bytes read.
Result<std::string> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);

/// Creates `path` exclusively and flushes its bytes to stable storage.
///
/// Fails with `already_exists` when `path` is already present, so a leftover
/// file can never be silently overwritten.
Result<void> write_file_flushed(const std::filesystem::path& path, std::string_view content);

/// Atomically replaces `path` with `content`.
///
/// The bytes go to a uniquely named temporary file in the same directory, are
/// flushed, and are then renamed over the target. On success the target is
/// either the old content or the new content, never a mixture.
Result<void> atomic_write_file(const std::filesystem::path& path, std::string_view content);

/// Flushes a directory entry so that a completed rename survives a crash.
///
/// Implemented with `fsync` on the directory on POSIX. Windows exposes no
/// equivalent operation through Win32; there the rename is issued with
/// `MOVEFILE_WRITE_THROUGH`, which does not return until the metadata change has
/// been flushed, and this call reports success without further work.
Result<void> flush_directory(const std::filesystem::path& directory);

Result<void> create_directories(const std::filesystem::path& directory);

/// True when `path` names an existing regular file. Symbolic links and reparse
/// points are not followed.
bool is_regular_file(const std::filesystem::path& path) noexcept;

/// True when `path` itself is a symbolic link or a reparse point.
bool is_link_like(const std::filesystem::path& path) noexcept;

/// True when `path` names an existing directory.
bool is_directory(const std::filesystem::path& path) noexcept;

bool exists(const std::filesystem::path& path) noexcept;

/// Removes a file, reporting whether it existed. A missing file is not an
/// error, so cleanup paths are idempotent.
Result<bool> remove_file(const std::filesystem::path& path);

/// Renames within one filesystem, replacing an existing target and flushing
/// the metadata change before returning.
Result<void> rename_replace(const std::filesystem::path& from, const std::filesystem::path& to);

/// Directory entry names, sorted byte-wise so every caller observes the same
/// deterministic order.
Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory);

/// Process identifier, used only to make temporary names unique.
std::string process_id_token();

/// Monotonic per-process counter, used only to make temporary names unique.
std::string next_sequence_token();

}  // namespace dccp::facility_capacity::internal::file_ops

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_FILE_OPS_HPP
