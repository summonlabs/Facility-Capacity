// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/error.hpp"

#include <array>
#include <ostream>
#include <utility>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

struct CodeEntry {
  ErrorCode code;
  std::string_view name;
  std::string_view description;
};

constexpr std::array<CodeEntry, 24> kCodes{{
    {ErrorCode::invalid_argument, "invalid_argument", "a supplied argument violates a documented precondition"},
    {ErrorCode::stale_generation, "stale_generation", "a generation-bearing token was older than the state"},
    {ErrorCode::stale_authority, "stale_authority", "epoch or controller incarnation authority was superseded"},
    {ErrorCode::conflict, "conflict", "the request contradicts committed state"},
    {ErrorCode::not_found, "not_found", "a referenced object does not exist"},
    {ErrorCode::already_exists, "already_exists", "an object that must be unique already exists"},
    {ErrorCode::incompatible_version, "incompatible_version", "the version or byte order is not supported"},
    {ErrorCode::corruption, "corruption", "bytes are structurally invalid or internally inconsistent"},
    {ErrorCode::limit_exceeded, "limit_exceeded", "a configured or hard bound would be exceeded"},
    {ErrorCode::unsupported, "unsupported", "the operation is well formed but not implemented"},
    {ErrorCode::unavailable, "unavailable", "the resource cannot serve the request right now"},
    {ErrorCode::indeterminate, "indeterminate", "the answer is not known and is not zero"},
    {ErrorCode::permission_denied, "permission_denied", "the operating system refused access"},
    {ErrorCode::io_failure, "io_failure", "an operating-system input/output call failed"},
    {ErrorCode::lock_conflict, "lock_conflict", "another process or thread holds the required lock"},
    {ErrorCode::invariant_violation, "invariant_violation", "an internal consistency rule would be violated"},
    {ErrorCode::unit_mismatch, "unit_mismatch", "quantities with different units were combined"},
    {ErrorCode::duplicate_identity, "duplicate_identity", "the same identity was declared twice"},
    {ErrorCode::precondition_failed, "precondition_failed", "a documented precondition was not satisfied"},
    {ErrorCode::checksum_mismatch, "checksum_mismatch", "stored bytes do not hash to the recorded digest"},
    {ErrorCode::truncated_input, "truncated_input", "the input ended before the required length"},
    {ErrorCode::path_rejected, "path_rejected", "a path or path component was rejected as unsafe"},
    {ErrorCode::account_mismatch, "account_mismatch", "capacity accounting did not close exactly"},
    {ErrorCode::not_measured, "not_measured", "the source never measured the requested value"},
}};

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
  for (const CodeEntry& entry : kCodes) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown_error_code";
}

std::string_view error_code_description(ErrorCode code) noexcept {
  for (const CodeEntry& entry : kCodes) {
    if (entry.code == code) {
      return entry.description;
    }
  }
  return "unrecognised";
}

bool parse_error_code(std::string_view name, ErrorCode& out) noexcept {
  for (const CodeEntry& entry : kCodes) {
    if (entry.name == name) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, ErrorCode code) { return out << error_code_name(code); }

Error::Error() : code_(ErrorCode::indeterminate), message_("unspecified failure") {}

Error::Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {
  if (message_.size() > limits::max_message_length) {
    message_.resize(limits::max_message_length);
  }
}

Error Error::make(ErrorCode code, std::string_view message) { return Error(code, std::string(message)); }

Error& Error::with_constraint(std::string constraint) {
  constraint_ = internal::truncate(constraint, limits::max_detail_length);
  return *this;
}

Error& Error::with_generations(std::uint64_t expected, std::uint64_t actual) noexcept {
  has_generations_ = true;
  expected_generation_ = expected;
  actual_generation_ = actual;
  return *this;
}

std::string Error::to_string() const {
  std::string out(error_code_name(code_));
  out.append(": ");
  out.append(message_);
  if (has_generations_) {
    out.append(" [expected_generation=");
    out.append(std::to_string(expected_generation_));
    out.append(" actual_generation=");
    out.append(std::to_string(actual_generation_));
    out.push_back(']');
  }
  if (!constraint_.empty()) {
    out.append(" [constraint=");
    out.append(constraint_);
    out.push_back(']');
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const Error& error) { return out << error.to_string(); }

}  // namespace dccp::facility_capacity
