// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed opaque identities.
//
// Every identity family has its own type. There is no implicit conversion
// between families, no conversion from a raw integer and no conversion from a
// bare `std::string`, so a facility identifier can never be passed where a site
// identifier is expected.

#ifndef DCCP_FACILITY_CAPACITY_STRONG_ID_HPP
#define DCCP_FACILITY_CAPACITY_STRONG_ID_HPP

#include <compare>
#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <type_traits>

#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity {

/// Validates an identifier: 1..max_identifier_length bytes, ASCII letters,
/// digits, `.`, `_`, `:`, `-` only; must begin with a letter or digit; must not
/// be `.` or `..`.
///
/// The restricted alphabet is deliberate. Identities appear in canonical
/// encodings, in explanations and in operator-facing output; refusing path
/// separators, whitespace, control characters, non-ASCII bytes and the two
/// relative-path names removes an entire class of path-manipulation and
/// encoding-confusion input before it reaches any other layer.
bool is_valid_identifier(std::string_view text) noexcept;

/// Explains why `is_valid_identifier` rejected a value. Empty when it accepts.
std::string_view identifier_rejection_reason(std::string_view text) noexcept;

/// Tag types. One per identity family.
struct FacilityIdTag {};
struct SiteIdTag {};
struct CapacitySourceIdTag {};
struct ReserveIdTag {};
struct ConstraintIdTag {};
struct SnapshotIdTag {};
struct StoreIdTag {};
struct ControllerIdTag {};
struct ServiceClassIdTag {};
struct OwnerIdTag {};

template <class Tag>
class BasicId {
 public:
  using tag_type = Tag;

  BasicId() = default;

  /// Parses and validates `text`.
  static Result<BasicId> parse(std::string_view text) {
    if (!is_valid_identifier(text)) {
      Error error = Error::make(ErrorCode::invalid_argument, "invalid identifier");
      error.with_constraint(std::string(identifier_rejection_reason(text)));
      return error;
    }
    BasicId id;
    id.value_.assign(text);
    return id;
  }

  /// Returns true when this identity has no value.
  bool empty() const noexcept { return value_.empty(); }

  /// The identity text. Empty for a default-constructed identity.
  const std::string& value() const noexcept { return value_; }

  std::string_view view() const noexcept { return value_; }

  std::string to_string() const { return value_; }

  friend bool operator==(const BasicId& lhs, const BasicId& rhs) noexcept = default;

  friend std::strong_ordering operator<=>(const BasicId& lhs, const BasicId& rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  std::string value_;
};

using FacilityId = BasicId<FacilityIdTag>;
using SiteId = BasicId<SiteIdTag>;
using CapacitySourceId = BasicId<CapacitySourceIdTag>;
using ReserveId = BasicId<ReserveIdTag>;
using ConstraintId = BasicId<ConstraintIdTag>;
using SnapshotId = BasicId<SnapshotIdTag>;
using StoreId = BasicId<StoreIdTag>;
using ControllerId = BasicId<ControllerIdTag>;
using ServiceClassId = BasicId<ServiceClassIdTag>;
using OwnerId = BasicId<OwnerIdTag>;

template <class Tag>
std::ostream& operator<<(std::ostream& out, const BasicId<Tag>& id) {
  return out << id.value();
}

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_STRONG_ID_HPP
