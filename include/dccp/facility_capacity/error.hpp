// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Stable error model.
//
// Every failure the library can report is one of the codes below. The codes are
// part of the public contract: callers branch on them, so a code is never
// reused for a different meaning and never renamed within a major version.

#ifndef DCCP_FACILITY_CAPACITY_ERROR_HPP
#define DCCP_FACILITY_CAPACITY_ERROR_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace dccp::facility_capacity {

/// Stable, machine-readable failure classification.
enum class ErrorCode : std::uint16_t {
  /// A caller supplied an argument that violates a documented precondition.
  invalid_argument = 1,
  /// A generation-bearing token was older than the state it was applied to.
  stale_generation = 2,
  /// Epoch or controller incarnation authority had been superseded.
  stale_authority = 3,
  /// The request contradicts committed state (identity or store mismatch).
  conflict = 4,
  /// A referenced object does not exist.
  not_found = 5,
  /// An object that must be unique already exists.
  already_exists = 6,
  /// The encoding, store container or product version is not supported.
  incompatible_version = 7,
  /// Bytes are structurally invalid or internally inconsistent.
  corruption = 8,
  /// A configured or hard bound would have been exceeded.
  limit_exceeded = 9,
  /// The operation is well formed but not implemented by this product.
  unsupported = 10,
  /// The resource exists but cannot serve the request right now.
  unavailable = 11,
  /// The answer is indeterminate: it is not known, and it is not zero.
  indeterminate = 12,
  /// The operating system refused access.
  permission_denied = 13,
  /// An operating-system input/output call failed.
  io_failure = 14,
  /// Another process or thread holds the required lock.
  lock_conflict = 15,
  /// An internal consistency rule would have been violated.
  invariant_violation = 16,
  /// Two quantities with different units were combined.
  unit_mismatch = 17,
  /// The same identity was declared twice with different content.
  duplicate_identity = 18,
  /// A documented precondition of the call was not satisfied.
  precondition_failed = 19,
  /// Stored bytes do not hash to the digest recorded for them.
  checksum_mismatch = 20,
  /// The input ended before the declared or required length.
  truncated_input = 21,
  /// A path or path component was rejected as unsafe.
  path_rejected = 22,
  /// Capacity accounting did not close exactly.
  account_mismatch = 23,
  /// A value was requested that the source never measured.
  not_measured = 24,
};

/// Stable machine-readable name of an error code, e.g. `stale_authority`.
std::string_view error_code_name(ErrorCode code) noexcept;

/// Human-readable one-line description of an error code.
std::string_view error_code_description(ErrorCode code) noexcept;

/// Parses a name produced by `error_code_name`. Returns false when unknown.
bool parse_error_code(std::string_view name, ErrorCode& out) noexcept;

std::ostream& operator<<(std::ostream& out, ErrorCode code);

/// A typed failure.
///
/// `Error` carries the classification, a bounded human-readable message and,
/// where the failure is about a comparison, the exact generations or the exact
/// violated constraint so that a caller can report it without guessing.
class Error {
 public:
  Error();

  Error(ErrorCode code, std::string message);

  /// Convenience for the common `code + message` case.
  static Error make(ErrorCode code, std::string_view message);

  ErrorCode code() const noexcept { return code_; }

  const std::string& message() const noexcept { return message_; }

  /// Exact violated constraint, e.g. `usable + unavailable + residual == installed`.
  /// Empty when the failure is not a constraint comparison.
  const std::string& constraint() const noexcept { return constraint_; }

  /// True when the failure carries an expected/actual generation pair.
  bool has_generations() const noexcept { return has_generations_; }

  /// Generation the caller supplied or that the operation required.
  std::uint64_t expected_generation() const noexcept { return expected_generation_; }

  /// Generation actually held by the state.
  std::uint64_t actual_generation() const noexcept { return actual_generation_; }

  Error& with_constraint(std::string constraint);

  Error& with_generations(std::uint64_t expected, std::uint64_t actual) noexcept;

  /// Deterministic single-line rendering: `code: message [constraint=...]`.
  std::string to_string() const;

 private:
  ErrorCode code_;
  std::string message_;
  std::string constraint_;
  bool has_generations_ = false;
  std::uint64_t expected_generation_ = 0;
  std::uint64_t actual_generation_ = 0;
};

std::ostream& operator<<(std::ostream& out, const Error& error);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_ERROR_HPP
