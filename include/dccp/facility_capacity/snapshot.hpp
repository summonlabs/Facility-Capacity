// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The published, generation-bound capacity answer.
//
// A snapshot is immutable and content-addressed. It records the exact evidence
// records, their generations, their provenance, the declared reserves and the
// service constraints that produced it, so that the answer can be explained,
// diffed and revalidated without re-reading the sources.

#ifndef DCCP_FACILITY_CAPACITY_SNAPSHOT_HPP
#define DCCP_FACILITY_CAPACITY_SNAPSHOT_HPP

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "dccp/facility_capacity/constraint.hpp"
#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/evidence.hpp"
#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/reserve.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// The classification of one dimension's answer.
///
/// Precedence is fixed and tested. A dimension is classified by the first rule
/// that applies, in this order:
///   1. `unavailable`  - no evidence covers the dimension and it is not
///                       declared required: the facility does not offer it.
///   2. `incomplete`   - required coverage is missing or was excluded as stale,
///                       superseded or belonging to another epoch, facility or
///                       incarnation.
///   3. `unknown`      - every covering source is present and current, and the
///                       composed allocatable quantity is unmeasured. This is
///                       never reported as zero.
///   4. `exhausted`    - the composed allocatable quantity is measured and
///                       exactly zero.
///   5. `usable`       - the composed allocatable quantity is measured and
///                       strictly positive.
enum class CapacityOutcome : std::uint8_t {
  unavailable = 0,
  incomplete = 1,
  unknown = 2,
  exhausted = 3,
  usable = 4,
};

std::string_view capacity_outcome_name(CapacityOutcome outcome) noexcept;
bool parse_capacity_outcome(std::string_view name, CapacityOutcome& out) noexcept;
std::ostream& operator<<(std::ostream& out, CapacityOutcome outcome);

/// Composed exact totals for one dimension.
///
/// Every field is a `Measured`: the value is either an exact quantity or a
/// statement that it was not measured.
struct DimensionTotals {
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  Measured installed;
  Measured observed;
  Measured usable;
  Measured protected_capacity;
  Measured reserved;
  Measured unavailable;
  Measured residual;
  Measured allocatable;
  std::size_t contributing_sources = 0;
  std::size_t measured_usable_sources = 0;

  /// True when `protected + reserved + allocatable == usable` was proven
  /// exactly while the snapshot was built.
  bool allocation_closed = false;
  /// True when `unavailable + residual + usable == installed` was proven
  /// exactly for every contributing source.
  bool physical_closed = false;

  std::string to_string() const;
};

/// The exact evidence record used for one source and dimension.
struct SourceGenerationStamp {
  CapacitySourceId source;
  CapacityDimension dimension = CapacityDimension::space;
  EvidenceGeneration generation{};
  /// Facility Capacity's digest of the evidence record that was used.
  std::string evidence_digest;
  /// The source's own digest, carried through unchanged. May be empty.
  std::string source_evidence_digest;
  /// Revision of the producing source.
  std::string source_revision;
  EpochId epoch{};
  IncarnationId incarnation{};
  Tick observed_at{};
  OperationalState state = OperationalState::nominal;
  /// Number of declared reserves that were itemised against this source.
  std::size_t itemised_reserves = 0;
  /// True when the source's declared reserve roll-up was used because no
  /// itemised reserves were declared.
  bool used_declared_rollup = false;

  std::string to_string() const;
};

/// One itemised declared reserve as it entered the answer.
struct ReserveStamp {
  ReserveId id;
  CapacitySourceId source;
  CapacityDimension dimension = CapacityDimension::space;
  ReserveKind kind = ReserveKind::protection;
  Unit unit = Unit::rack_unit;
  Measured amount;
  ValidityWindow window;
  OwnerId owner;
  ServiceClassId service_class;
  ReasonCode reason = ReasonCode::none;
  std::string reserve_digest;

  std::string to_string() const;
};

/// Result of evaluating one declared service floor.
enum class ConstraintStatus : std::uint8_t {
  /// The floor was met by a measured allocatable quantity.
  satisfied = 0,
  /// A measured allocatable quantity was below the floor.
  violated = 1,
  /// The floor or the allocatable quantity was unmeasured.
  indeterminate = 2,
  /// The constraint window did not cover the snapshot instant.
  inactive = 3,
};

std::string_view constraint_status_name(ConstraintStatus status) noexcept;
bool parse_constraint_status(std::string_view name, ConstraintStatus& out) noexcept;
std::ostream& operator<<(std::ostream& out, ConstraintStatus status);

struct ConstraintEvaluation {
  ConstraintId id;
  CapacityDimension dimension = CapacityDimension::space;
  ServiceClassId service_class;
  Unit unit = Unit::rack_unit;
  ConstraintStatus status = ConstraintStatus::inactive;
  Measured floor_value;
  Measured allocatable;
  ReasonCode reason = ReasonCode::none;
  std::string detail;

  std::string to_string() const;
};

/// Attribution of the reduction that dominates one dimension.
struct DimensionLimit {
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  Measured installed;
  Measured usable;
  Measured allocatable;
  /// The reason the dimension loses the most capacity, or the reason its
  /// allocatable quantity is unmeasured.
  ReasonCode reason = ReasonCode::none;
  /// The source the reduction is attributed to. Empty when the reduction is
  /// facility-wide rather than source-specific.
  CapacitySourceId source;
  /// Exact size of the dominant reduction, or unmeasured.
  Measured amount;

  std::string to_string() const;
};

/// The dimension that is closest to exhaustion, expressed as an exact permille
/// headroom ratio `allocatable * 1000 / usable`.
///
/// Dimensions have different units, so they cannot be compared by magnitude.
/// They are compared by the exact integer ratio of allocatable to usable
/// capacity, computed with checked 64-bit arithmetic. Ties are broken by
/// dimension ordinal so the attribution is deterministic.
struct LimitingConstraint {
  bool present = false;
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  /// `allocatable * 1000 / usable`, truncated toward zero. Zero when the
  /// dimension has no measured usable capacity.
  std::int64_t headroom_permille = 0;
  Measured allocatable;
  Measured usable;
  ReasonCode reason = ReasonCode::none;
  CapacitySourceId source;

  std::string to_string() const;
};

/// How complete the answer is with respect to the declared coverage contract.
enum class CompletenessClass : std::uint8_t {
  /// Every required dimension is covered by current evidence from every
  /// required source.
  complete = 0,
  /// At least one required dimension or required source is missing or was
  /// excluded.
  incomplete = 1,
};

std::string_view completeness_class_name(CompletenessClass value) noexcept;
std::ostream& operator<<(std::ostream& out, CompletenessClass value);

struct Completeness {
  CompletenessClass classification = CompletenessClass::complete;
  std::size_t required_dimensions = 0;
  std::size_t complete_dimensions = 0;
  NoteSet findings;

  bool is_complete() const noexcept { return classification == CompletenessClass::complete; }
};

/// Whether the snapshot was issued by this process from current evidence, or
/// recovered from durable storage.
///
/// Freshness is the reader's stamp on the answer, not part of the answer: it is
/// deliberately excluded from the canonical encoding and therefore from the
/// content digest, so the digest of a recovered answer is exactly the digest
/// that was published, and a store can verify that what it read is what it
/// wrote.
///
/// A recovered snapshot is never silently treated as current. It reports
/// `recovered` here, `CapacityStore::recover` returns it with a
/// `recovered_not_revalidated` finding, and a caller that wants to rely on it
/// must call `FacilityCapacityModel::revalidate` against current evidence. That
/// revalidation is what turns a recovered answer back into a current one.
enum class SnapshotFreshness : std::uint8_t {
  issued = 0,
  recovered = 1,
};

std::string_view snapshot_freshness_name(SnapshotFreshness value) noexcept;
bool parse_snapshot_freshness(std::string_view name, SnapshotFreshness& out) noexcept;
std::ostream& operator<<(std::ostream& out, SnapshotFreshness value);

/// The inputs from which a snapshot is derived.
///
/// The derived content (totals, limits, completeness, digest) is never
/// supplied by a caller: `CapacitySnapshot::build` computes all of it, and the
/// decoder re-derives it and compares the result byte for byte with what was
/// stored.
struct CapacitySnapshotInput {
  FacilityId facility;
  SiteId site;
  CapacityGeneration generation{};
  EpochId epoch{};
  IncarnationId incarnation{};
  Revision revision{};
  Tick built_at{};
  /// Exclusive end of the snapshot's validity window. `max_tick` when the
  /// model declares no expiry.
  Tick valid_until = max_tick;
  SnapshotFreshness freshness = SnapshotFreshness::issued;
  std::vector<CapacityRequirement> requirements;
  std::vector<SourceEvidence> evidence;
  std::vector<CapacityReserve> reserves;
  std::vector<ServiceConstraint> constraints;
  /// Findings raised before the snapshot was assembled: missing required
  /// sources, excluded stale evidence, and so on.
  NoteSet coverage_findings;
};

class FacilityCapacityModel;

/// An immutable, content-addressed, generation-bound capacity answer.
class CapacitySnapshot {
 public:
  CapacitySnapshot(const CapacitySnapshot&) = delete;
  CapacitySnapshot& operator=(const CapacitySnapshot&) = delete;

  /// Derives and validates a snapshot.
  ///
  /// Fails with `account_mismatch` when the inputs cannot close exactly, with
  /// `limit_exceeded` when the input exceeds a bound, and with
  /// `invalid_argument` when an identity is missing.
  static Result<std::shared_ptr<const CapacitySnapshot>> build(CapacitySnapshotInput input);

  // --- identity and authority --------------------------------------------
  const FacilityId& facility() const noexcept { return facility_; }
  const SiteId& site() const noexcept { return site_; }
  CapacityGeneration generation() const noexcept { return generation_; }
  EpochId epoch() const noexcept { return epoch_; }
  IncarnationId incarnation() const noexcept { return incarnation_; }
  Revision revision() const noexcept { return revision_; }
  Tick built_at() const noexcept { return built_at_; }
  Tick valid_until() const noexcept { return valid_until_; }
  SnapshotFreshness freshness() const noexcept { return freshness_; }

  /// Short operator handle derived from the generation and the digest.
  const SnapshotId& id() const noexcept { return id_; }

  /// Hex SHA-256 over the canonical encoding of this snapshot.
  const std::string& digest() const noexcept { return digest_; }

  // --- content ------------------------------------------------------------
  const std::vector<DimensionTotals>& dimension_totals() const noexcept { return totals_; }
  const DimensionTotals* find_totals(CapacityDimension dimension) const noexcept;
  const std::vector<SourceGenerationStamp>& sources() const noexcept { return sources_; }
  const std::vector<ReserveStamp>& reserves() const noexcept { return reserves_; }
  const std::vector<ConstraintEvaluation>& constraints() const noexcept { return constraints_; }
  const std::vector<DimensionLimit>& limits() const noexcept { return limits_; }
  const std::vector<CapacityRequirement>& requirements() const noexcept { return requirements_; }
  const NoteSet& notes() const noexcept { return notes_; }
  const Completeness& completeness() const noexcept { return completeness_; }
  const LimitingConstraint& limiting() const noexcept { return limiting_; }

  // --- answers ------------------------------------------------------------
  CapacityOutcome outcome(CapacityDimension dimension) const noexcept;
  Measured allocatable(CapacityDimension dimension) const;

  /// Deterministic multi-line rendering of the whole answer.
  std::string describe() const;

  /// Deterministic rendering of one dimension's answer and why it is what it
  /// is.
  std::string explain(CapacityDimension dimension) const;

  /// True when every dimension's accounting closed exactly.
  bool accounting_closed() const noexcept { return accounting_closed_; }

  /// Rebuilds the input description of this snapshot. Used by the decoder to
  /// re-derive and compare, and by `diff`.
  const CapacitySnapshotInput& input() const noexcept { return input_; }

 private:
  CapacitySnapshot() = default;

  static Result<std::shared_ptr<const CapacitySnapshot>> derive(CapacitySnapshotInput input);

  CapacitySnapshotInput input_;
  FacilityId facility_;
  SiteId site_;
  CapacityGeneration generation_{};
  EpochId epoch_{};
  IncarnationId incarnation_{};
  Revision revision_{};
  Tick built_at_{};
  Tick valid_until_ = max_tick;
  SnapshotFreshness freshness_ = SnapshotFreshness::issued;
  SnapshotId id_;
  std::string digest_;
  std::vector<DimensionTotals> totals_;
  std::vector<CapacityOutcome> outcomes_;
  std::vector<SourceGenerationStamp> sources_;
  std::vector<ReserveStamp> reserves_;
  std::vector<ConstraintEvaluation> constraints_;
  std::vector<DimensionLimit> limits_;
  std::vector<CapacityRequirement> requirements_;
  NoteSet notes_;
  Completeness completeness_;
  LimitingConstraint limiting_;
  bool accounting_closed_ = false;
};

/// The answer to one dimension query.
struct CapacityAnswer {
  CapacityOutcome outcome = CapacityOutcome::unavailable;
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  DimensionTotals totals;
  NoteSet reasons;
  /// Digest of the snapshot the answer was taken from.
  std::string snapshot_digest;
  CapacityGeneration generation{};

  std::string describe() const;
};

std::ostream& operator<<(std::ostream& out, const CapacityAnswer& answer);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_SNAPSHOT_HPP
