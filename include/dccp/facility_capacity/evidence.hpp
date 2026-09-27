// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Typed immutable evidence.
//
// SourceEvidence is the only way capacity enters the aggregate model. It is
// created once, validated at creation, digest-bound, and immutable thereafter:
// there is no setter, no mutable field and no way to re-stamp a generation.
//
// A source that wants to report a new value publishes new evidence with a new
// evidence generation. The old record stays exactly as it was, which is what
// makes stale-authority rejection and provenance preservation possible.

#ifndef DCCP_FACILITY_CAPACITY_EVIDENCE_HPP
#define DCCP_FACILITY_CAPACITY_EVIDENCE_HPP

#include <iosfwd>
#include <string>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/provenance.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// The source's own view of whether it can serve capacity right now.
enum class OperationalState : std::uint8_t {
  /// The source considers itself fully operational.
  nominal = 0,
  /// The source is serving capacity but is not at full capability.
  degraded = 1,
  /// The source is not serving capacity at all. Its usable capacity is zero.
  unavailable = 2,
  /// The source cannot report its own state. Not the same as nominal.
  unknown = 3,
};

std::string_view operational_state_name(OperationalState state) noexcept;
bool parse_operational_state(std::string_view name, OperationalState& out) noexcept;
std::ostream& operator<<(std::ostream& out, OperationalState state);

/// The fields of one evidence record, as supplied by a source.
///
/// The unit is explicit even though a dimension has exactly one canonical unit:
/// the mismatch is refused with `unit_mismatch` rather than silently corrected,
/// both when evidence is declared and when it is decoded from a store.
struct SourceEvidenceFields {
  CapacitySourceId source;
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  EvidenceGeneration generation{};
  Tick observed_at{};
  Provenance provenance;
  OperationalState state = OperationalState::nominal;
  ReasonCode state_reason = ReasonCode::none;
  std::string state_detail;

  /// Physically installed capacity in the dimension. Must be measured.
  Measured installed;
  /// Capacity the source observed to be present. May be unmeasured.
  Measured observed;
  /// Capacity available for allocation after derating. May be unmeasured.
  Measured usable;
  /// Installed capacity that is out of service. May be unmeasured.
  Measured unavailable;
  /// Installed capacity that is neither usable nor out of service: derating,
  /// conversion loss, physical overhead. May be unmeasured.
  Measured residual;
  /// Capacity the source declares as held back for protection. May be
  /// unmeasured.
  Measured protected_capacity;
  /// Capacity the source declares as held back for other reserves. May be
  /// unmeasured.
  Measured reserved;

  /// When true, `residual` is computed as `installed - unavailable - usable`
  /// and must be non-negative. When false, `residual` must be supplied and the
  /// closure must hold exactly.
  bool derive_residual = false;
};

/// One immutable, digest-bound evidence record from one source about one
/// dimension.
class SourceEvidence {
 public:
  SourceEvidence() = default;

  /// Validates and freezes a record.
  ///
  /// Enforced here, at the boundary, so that no later stage can observe an
  /// inconsistent input:
  ///   - the source identity is valid and the generation is at least one;
  ///   - the declared unit is the dimension's canonical unit;
  ///   - `installed` is measured;
  ///   - every measured value is non-negative and in the same unit;
  ///   - `observed <= installed` when both are measured;
  ///   - `usable + unavailable + residual == installed` exactly whenever all
  ///     three parts are measured;
  ///   - `protected + reserved <= usable` whenever all three are measured;
  ///   - a source that reports itself unavailable has measured zero usable
  ///     capacity;
  ///   - provenance is present, valid and names the same source.
  static Result<SourceEvidence> create(SourceEvidenceFields fields);

  const CapacitySourceId& source() const noexcept { return source_; }
  CapacityDimension dimension() const noexcept { return dimension_; }
  Unit unit() const noexcept { return unit_; }
  EvidenceGeneration generation() const noexcept { return generation_; }
  Tick observed_at() const noexcept { return observed_at_; }
  const Provenance& provenance() const noexcept { return provenance_; }
  OperationalState state() const noexcept { return state_; }
  ReasonCode state_reason() const noexcept { return state_reason_; }
  const std::string& state_detail() const noexcept { return state_detail_; }

  const Measured& installed() const noexcept { return installed_; }
  const Measured& observed() const noexcept { return observed_; }
  const Measured& usable() const noexcept { return usable_; }
  const Measured& unavailable() const noexcept { return unavailable_; }
  const Measured& residual() const noexcept { return residual_; }
  const Measured& protected_capacity() const noexcept { return protected_capacity_; }
  const Measured& reserved() const noexcept { return reserved_; }

  /// True when `usable + unavailable + residual == installed` was proven
  /// exactly at creation. False when a part is unmeasured.
  bool closure_proven() const noexcept { return closure_proven_; }

  /// `usable - protected_capacity - reserved`, or unmeasured when any of the
  /// three is unmeasured.
  Measured allocatable() const;

  /// Hex-encoded SHA-256 over the canonical encoding of this record.
  const std::string& digest() const noexcept { return digest_; }

  /// Deterministic single-line rendering.
  std::string to_string() const;

  friend bool operator==(const SourceEvidence& lhs, const SourceEvidence& rhs) noexcept {
    return lhs.digest_ == rhs.digest_;
  }

 private:
  CapacitySourceId source_;
  CapacityDimension dimension_ = CapacityDimension::space;
  Unit unit_ = Unit::rack_unit;
  EvidenceGeneration generation_{};
  Tick observed_at_{};
  Provenance provenance_;
  OperationalState state_ = OperationalState::nominal;
  ReasonCode state_reason_ = ReasonCode::none;
  std::string state_detail_;
  Measured installed_;
  Measured observed_;
  Measured usable_;
  Measured unavailable_;
  Measured residual_;
  Measured protected_capacity_;
  Measured reserved_;
  bool closure_proven_ = false;
  std::string digest_;
};

std::ostream& operator<<(std::ostream& out, const SourceEvidence& evidence);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_EVIDENCE_HPP
