// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The aggregate facility-capacity model.
//
// This is the product's centre of gravity. It holds the current evidence,
// declared reserves and declared service constraints for one facility, derives
// one generation-bound answer from them, and refuses every mutation whose
// precondition does not hold against the state it actually has.
//
// Ownership: the model is serialised behind one lock. Readers take a shared
// lock and observe immutable snapshots; writers take an exclusive lock for the
// duration of one mutation. No callback, no observer and no file operation ever
// runs while the lock is held, so the lock cannot be re-entered and cannot be
// held across a blocking call.

#ifndef DCCP_FACILITY_CAPACITY_MODEL_HPP
#define DCCP_FACILITY_CAPACITY_MODEL_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/clock.hpp"
#include "dccp/facility_capacity/constraint.hpp"
#include "dccp/facility_capacity/evidence.hpp"
#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/reserve.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/revalidate.hpp"
#include "dccp/facility_capacity/snapshot.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// Explicit precondition carried by every state-dependent mutation.
///
/// All four tokens are checked. There is no wildcard and no "force" flag: a
/// caller that does not know the current generation, revision, epoch and
/// incarnation has no authority to mutate the model and is told so.
struct CapacityPrecondition {
  CapacityGeneration expected_capacity_generation{};
  Revision expected_revision{};
  EpochId expected_epoch{};
  IncarnationId expected_incarnation{};
  /// Bounded idempotency token. Re-submitting an attempt that is still inside
  /// the model's recorded attempt window succeeds without applying anything a
  /// second time.
  AttemptId attempt{};

  std::string to_string() const;
};

/// Configuration of one aggregate model.
struct ModelConfig {
  FacilityId facility;
  SiteId site;
  EpochId epoch;
  IncarnationId incarnation;
  /// The coverage contract: which dimensions must be answered, and by which
  /// sources.
  std::vector<CapacityRequirement> requirements;
  /// Lifetime of a published snapshot, in ticks. Zero means no expiry.
  Tick snapshot_validity_ticks{};
  std::size_t max_sources = limits::max_evidence_sources;
  std::size_t max_reserves = limits::max_reserves;
  std::size_t max_constraints = limits::max_service_constraints;
};

/// The aggregate facility-capacity model for one facility.
class FacilityCapacityModel {
 public:
  FacilityCapacityModel() = delete;
  ~FacilityCapacityModel();

  FacilityCapacityModel(FacilityCapacityModel&&) noexcept;
  FacilityCapacityModel& operator=(FacilityCapacityModel&&) noexcept;
  FacilityCapacityModel(const FacilityCapacityModel&) = delete;
  FacilityCapacityModel& operator=(const FacilityCapacityModel&) = delete;

  /// Creates an empty model with no evidence, no reserves, no constraints and
  /// capacity generation zero.
  static Result<FacilityCapacityModel> create(ModelConfig config, Clock& clock);

  // --- identity and authority --------------------------------------------

  const FacilityId& facility() const;
  const SiteId& site() const;
  CapacityGeneration capacity_generation() const;
  Revision revision() const;
  EpochId epoch() const;
  IncarnationId incarnation() const;
  Tick snapshot_validity_ticks() const;

  /// A copy of the coverage contract.
  std::vector<CapacityRequirement> requirements() const;

  /// The precondition that matches the model's current state, with `attempt`
  /// left at zero for the caller to fill in.
  CapacityPrecondition current_precondition() const;

  /// Re-checks every internal invariant. Returns the first violation found.
  Status validate() const;

  // --- read ---------------------------------------------------------------

  /// The current evidence for one source and dimension.
  Result<SourceEvidence> evidence(const CapacitySourceId& source, CapacityDimension dimension) const;

  /// Every current evidence record, ordered by dimension then source.
  std::vector<SourceEvidence> all_evidence() const;

  /// Every declared reserve, ordered by dimension, source and identity.
  std::vector<CapacityReserve> all_reserves() const;

  /// Every declared service constraint, ordered by dimension and identity.
  std::vector<ServiceConstraint> all_constraints() const;

  /// The outcome this model would publish for one dimension right now, without
  /// publishing a generation.
  Result<CapacityAnswer> query(CapacityDimension dimension) const;

  /// The most recently published snapshot, or null when none has been
  /// published. The returned pointer is immutable and remains valid after the
  /// model is destroyed.
  std::shared_ptr<const CapacitySnapshot> current_snapshot() const;

  /// The generation of the current snapshot, or zero when there is none.
  CapacityGeneration published_generation() const;

  // --- mutate -------------------------------------------------------------

  /// Applies or replaces the evidence for one source and dimension.
  ///
  /// Replaces only when `evidence.generation()` is strictly greater than the
  /// generation already held for that source and dimension; an equal or older
  /// generation is refused with `stale_generation`, and a different facility
  /// epoch is refused with `stale_authority`.
  Status declare_evidence(SourceEvidence evidence, const CapacityPrecondition& precondition);

  /// Removes the evidence for one source and dimension.
  Status retire_evidence(const CapacitySourceId& source, CapacityDimension dimension,
                         const CapacityPrecondition& precondition);

  /// Declares or replaces a reserve. Same generation rules as evidence apply
  /// through the reserve's provenance.
  Status declare_reserve(CapacityReserve reserve, const CapacityPrecondition& precondition);

  /// Withdraws a declared reserve.
  Status withdraw_reserve(const ReserveId& id, const CapacityPrecondition& precondition);

  /// Declares or replaces a service constraint.
  Status declare_constraint(ServiceConstraint constraint, const CapacityPrecondition& precondition);

  /// Withdraws a declared service constraint.
  Status withdraw_constraint(const ConstraintId& id, const CapacityPrecondition& precondition);

  /// Declares or replaces the coverage requirement for one dimension.
  Status set_requirement(CapacityRequirement requirement, const CapacityPrecondition& precondition);

  /// Advances the control-plane epoch and this controller's incarnation.
  ///
  /// The new epoch must be strictly greater than the current one; the new
  /// incarnation must be non-zero and different from the current one. Evidence
  /// already held keeps its own provenance epoch and is therefore reported as
  /// stale by `revalidate` until the sources republish under the new epoch.
  Status advance_epoch(EpochId new_epoch, IncarnationId new_incarnation, const CapacityPrecondition& precondition);

  /// Builds and publishes the next capacity generation.
  ///
  /// The returned snapshot is immutable. Publication advances the capacity
  /// generation by exactly one and records the new generation in the model.
  Result<std::shared_ptr<const CapacitySnapshot>> publish(const CapacityPrecondition& precondition);

  /// Compares a snapshot with the model's current state.
  Result<RevalidationReport> revalidate(const CapacitySnapshot& snapshot) const;

  // --- persistence --------------------------------------------------------

  /// Canonical, self-digested encoding of the whole model state.
  Result<std::string> encode() const;

  /// Decodes a state produced by `encode`.
  static Result<FacilityCapacityModel> decode(std::string_view canonical_body, Clock& clock);

  /// Digest of the canonical encoding. Equal to the digest recorded inside it.
  Result<std::string> state_digest() const;

 private:
  class Impl;
  explicit FacilityCapacityModel(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_MODEL_HPP
