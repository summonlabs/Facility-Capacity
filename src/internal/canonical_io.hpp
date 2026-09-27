// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical document writer and reader. Private to the library.
//
// A document is a sequence of `key=value` lines terminated by LF, followed by
// exactly one `digest=<hex>` line. Keys are unique. Values are escaped so that
// the document is pure ASCII regardless of the input.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CANONICAL_IO_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CANONICAL_IO_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/evidence.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/reserve.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"
#include "dccp/facility_capacity/units.hpp"

namespace dccp::facility_capacity::internal {

/// Appends canonical `key=value` lines.
class CanonicalWriter {
 public:
  void field(std::string_view key, std::string_view value);
  void field(std::string_view key, const char* value) { field(key, std::string_view(value)); }
  void field(std::string_view key, std::uint64_t value);
  void field(std::string_view key, std::int64_t value);
  void field(std::string_view key, bool value);
  void field(std::string_view key, Unit value);
  void field(std::string_view key, CapacityDimension value);
  void field(std::string_view key, ReasonCode value);
  void field(std::string_view key, OperationalState value);
  void field(std::string_view key, ReserveKind value);
  void field(std::string_view key, CapacityOutcome value);
  void field(std::string_view key, ConstraintStatus value);
  void field(std::string_view key, SnapshotFreshness value);
  void field(std::string_view key, const Measured& value);

  /// The bytes written so far.
  const std::string& str() const noexcept { return buffer_; }

  /// SHA-256 of the bytes written so far, hex encoded.
  std::string digest_hex() const;

  /// Appends the terminating `digest=` line and returns the complete document.
  std::string finish();

 private:
  std::string buffer_;
};

/// Indexed field name builder: `prefix` + `.` + index + `.` + suffix.
std::string indexed_key(std::string_view prefix, std::size_t index, std::string_view suffix);

/// A parsed, digest-verified canonical document.
///
/// Every field must be consumed exactly once and nothing may be left over, so a
/// decoder that forgets a field or that would accept an extra one fails instead
/// of silently ignoring it.
class CanonicalReader {
 public:
  /// Parses and verifies. Enforces the line, length and count bounds before it
  /// indexes anything.
  static Result<CanonicalReader> parse(std::string_view body);

  Result<std::string_view> take(std::string_view key) const;
  Result<std::string> take_string(std::string_view key) const;
  Result<std::uint64_t> take_u64(std::string_view key) const;
  Result<std::int64_t> take_i64(std::string_view key) const;
  Result<bool> take_bool(std::string_view key) const;
  Result<Unit> take_unit(std::string_view key) const;
  Result<CapacityDimension> take_dimension(std::string_view key) const;
  Result<ReasonCode> take_reason(std::string_view key) const;
  Result<OperationalState> take_state(std::string_view key) const;
  Result<ReserveKind> take_reserve_kind(std::string_view key) const;
  Result<CapacityOutcome> take_outcome(std::string_view key) const;
  Result<ConstraintStatus> take_constraint_status(std::string_view key) const;
  Result<SnapshotFreshness> take_freshness(std::string_view key) const;
  /// Parses `unknown` or a decimal magnitude.
  Result<Measured> take_measured(std::string_view key, Unit unit) const;
  /// Parses `unknown` or a decimal magnitude.
  Result<Quantity> take_quantity(std::string_view key, Unit unit) const;

  /// Requires that a field exists and equals `expected`.
  Result<void> expect(std::string_view key, std::string_view expected) const;

  /// True when the field exists. Does not consume it.
  bool has(std::string_view key) const;

  /// Requires that every field has been consumed.
  Result<void> finish() const;

  /// Number of fields in the document.
  std::size_t size() const noexcept { return fields_.size(); }

 private:
  CanonicalReader() = default;

  /// Looks up a field, marking it consumed.
  Result<std::string_view> locate(std::string_view key) const;

  std::vector<std::pair<std::string, std::string>> fields_;
  std::unordered_map<std::string, std::size_t> index_;
  mutable std::vector<bool> consumed_;
};

/// Bounded decimal rendering used by the writer for unsigned magnitudes.
std::string render_u64(std::uint64_t value);

}  // namespace dccp::facility_capacity::internal

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CANONICAL_IO_HPP
