// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Structural differences between two published answers.
//
// A diff is exact: a change is reported only when the underlying values differ,
// and a delta is reported only when both sides were measured. An unmeasured
// side yields an unmeasured delta, never a zero delta.

#ifndef DCCP_FACILITY_CAPACITY_DIFF_HPP
#define DCCP_FACILITY_CAPACITY_DIFF_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// One quantity before and after.
struct QuantityChange {
  bool before_known = false;
  bool after_known = false;
  Quantity before;
  Quantity after;
  /// True only when both sides were measured, so the difference is exact.
  bool delta_known = false;
  std::int64_t delta = 0;

  /// True when the measured state or the value changed.
  bool changed() const noexcept;

  std::string to_string() const;
};

struct DimensionChange {
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  QuantityChange installed;
  QuantityChange usable;
  QuantityChange protected_capacity;
  QuantityChange reserved;
  QuantityChange unavailable;
  QuantityChange residual;
  QuantityChange allocatable;
  CapacityOutcome outcome_before = CapacityOutcome::unavailable;
  CapacityOutcome outcome_after = CapacityOutcome::unavailable;

  bool changed() const noexcept;
  std::string to_string() const;
};

struct SourceChange {
  CapacitySourceId source;
  CapacityDimension dimension = CapacityDimension::space;
  bool present_before = false;
  bool present_after = false;
  EvidenceGeneration generation_before{};
  EvidenceGeneration generation_after{};
  OperationalState state_before = OperationalState::nominal;
  OperationalState state_after = OperationalState::nominal;

  bool changed() const noexcept;
  std::string to_string() const;
};

struct ConstraintChange {
  ConstraintId id;
  CapacityDimension dimension = CapacityDimension::space;
  ConstraintStatus status_before = ConstraintStatus::inactive;
  ConstraintStatus status_after = ConstraintStatus::inactive;

  bool changed() const noexcept;
  std::string to_string() const;
};

struct SnapshotDiff {
  FacilityId facility;
  SiteId site;
  CapacityGeneration before_generation{};
  CapacityGeneration after_generation{};
  std::string before_digest;
  std::string after_digest;
  std::vector<DimensionChange> dimensions;
  std::vector<SourceChange> sources;
  std::vector<ConstraintChange> constraints;

  /// True when nothing at all changed.
  bool empty() const noexcept;

  /// Deterministic multi-line rendering.
  std::string describe() const;
};

/// Diffs two snapshots.
///
/// Fails with `conflict` when the snapshots describe different facilities or
/// sites, and with `limit_exceeded` when the entry bound would be exceeded.
Result<SnapshotDiff> diff_snapshots(const CapacitySnapshot& before, const CapacitySnapshot& after);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_DIFF_HPP
