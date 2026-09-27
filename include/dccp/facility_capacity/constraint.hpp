// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Service constraints and coverage requirements.
//
// Both are declarations made *to* the aggregate capacity model by the operator
// that owns it. Neither is a lower-level source policy:
//
//   - a `ServiceConstraint` states a floor that must remain allocatable in a
//     dimension while a window is active. The model evaluates it; it does not
//     enforce, admit or schedule anything;
//   - a `CapacityRequirement` states which dimensions the aggregate model is
//     required to answer and which sources must be present for it to claim a
//     complete answer. Without that declaration "incomplete" would have no
//     definition.

#ifndef DCCP_FACILITY_CAPACITY_CONSTRAINT_HPP
#define DCCP_FACILITY_CAPACITY_CONSTRAINT_HPP

#include <iosfwd>
#include <string>
#include <vector>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/provenance.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

struct ServiceConstraintFields {
  ConstraintId id;
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  /// Service class the floor protects. May be empty.
  ServiceClassId service_class;
  /// The minimum allocatable quantity that must remain in the dimension while
  /// the window is active. May be unmeasured, in which case the constraint is
  /// reported as indeterminate rather than satisfied.
  Measured floor;
  ValidityWindow window;
  OwnerId owner;
  ReasonCode reason = ReasonCode::none;
  std::string detail;
  Provenance provenance;
};

/// One immutable, digest-bound service floor.
class ServiceConstraint {
 public:
  ServiceConstraint() = default;

  static Result<ServiceConstraint> create(ServiceConstraintFields fields);

  const ConstraintId& id() const noexcept { return id_; }
  CapacityDimension dimension() const noexcept { return dimension_; }
  Unit unit() const noexcept { return unit_; }
  const ServiceClassId& service_class() const noexcept { return service_class_; }
  const Measured& floor_value() const noexcept { return floor_; }
  const ValidityWindow& window() const noexcept { return window_; }
  const OwnerId& owner() const noexcept { return owner_; }
  ReasonCode reason() const noexcept { return reason_; }
  const std::string& detail() const noexcept { return detail_; }
  const Provenance& provenance() const noexcept { return provenance_; }

  bool active_at(Tick instant) const noexcept { return window_.active_at(instant); }

  const std::string& digest() const noexcept { return digest_; }

  std::string to_string() const;

  friend bool operator==(const ServiceConstraint& lhs, const ServiceConstraint& rhs) noexcept {
    return lhs.digest_ == rhs.digest_;
  }

 private:
  ConstraintId id_;
  CapacityDimension dimension_ = CapacityDimension::space;
  Unit unit_ = Unit::rack_unit;
  ServiceClassId service_class_;
  Measured floor_;
  ValidityWindow window_;
  OwnerId owner_;
  ReasonCode reason_ = ReasonCode::none;
  std::string detail_;
  Provenance provenance_;
  std::string digest_;
};

std::ostream& operator<<(std::ostream& out, const ServiceConstraint& constraint);

/// Which dimensions the aggregate model must answer, and which sources must be
/// present and current for that answer to count as complete.
struct CapacityRequirement {
  CapacityDimension dimension = CapacityDimension::space;
  /// When true, the dimension must be offered and answered completely.
  bool required = true;
  /// Sources that must have current evidence for this dimension. Sorted,
  /// duplicate-free.
  std::vector<CapacitySourceId> required_sources;

  /// Sorts and de-duplicates `required_sources` and enforces the bound.
  Status normalize();

  std::string to_string() const;

  friend bool operator==(const CapacityRequirement& lhs, const CapacityRequirement& rhs) noexcept {
    return lhs.dimension == rhs.dimension && lhs.required == rhs.required &&
           lhs.required_sources == rhs.required_sources;
  }
};

std::ostream& operator<<(std::ostream& out, const CapacityRequirement& requirement);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_CONSTRAINT_HPP
