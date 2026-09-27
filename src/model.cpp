// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The aggregate facility-capacity model.
//
// Lock order
// ----------
// The model owns exactly one lock, a `std::shared_mutex` protecting its state.
//
//   * Readers take it shared and copy or borrow immutable values.
//   * Writers take it exclusive for the duration of one mutation.
//   * No lock is ever taken while another is held: the model never calls into
//     the store, never calls into the clock and never invokes a callback while
//     holding its own lock. The clock is sampled before the lock is acquired.
//   * The lock is never upgraded. There is no read-modify-write path that
//     releases and re-acquires, and no code path takes the same lock twice.
//   * Published snapshots are immutable `shared_ptr<const>` values that outlive
//     the lock and can be handed to other threads freely.

#include "dccp/facility_capacity/model.hpp"

#include <algorithm>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/version.hpp"
#include "internal/canonical_io.hpp"
#include "internal/checked.hpp"
#include "internal/documents.hpp"

namespace dccp::facility_capacity {
namespace {

Error stale(std::string_view what, std::uint64_t expected, std::uint64_t actual) {
  Error error = Error::make(ErrorCode::stale_generation, what);
  error.with_generations(expected, actual);
  return error;
}

Error stale_authority(std::string_view what, std::uint64_t expected, std::uint64_t actual) {
  Error error = Error::make(ErrorCode::stale_authority, what);
  error.with_generations(expected, actual);
  return error;
}

Status normalize_requirements(std::vector<CapacityRequirement>& requirements) {  if (requirements.size() > limits::max_requirements) {
    return Error::make(ErrorCode::limit_exceeded, "too many coverage requirements");
  }
  for (CapacityRequirement& requirement : requirements) {
    const Status status = requirement.normalize();
    if (!status.has_value()) {
      return status.error();
    }
  }
  std::sort(requirements.begin(), requirements.end(),
            [](const CapacityRequirement& lhs, const CapacityRequirement& rhs) {
              return dimension_ordinal(lhs.dimension) < dimension_ordinal(rhs.dimension);
            });
  for (std::size_t index = 1; index < requirements.size(); ++index) {
    if (requirements[index].dimension == requirements[index - 1].dimension) {
      return Error::make(ErrorCode::duplicate_identity, "a dimension has more than one coverage requirement");
    }
  }
  return Status::success();
}

/// Advances the model revision. A revision that would wrap is reported rather
/// than silently returning to zero.
Status bump_revision(internal::ModelState& state) {
  const Result<Revision> next = state.revision.next();
  if (!next.has_value()) {
    return next.error();
  }
  state.revision = next.value();
  return Status::success();
}

}  // namespace

std::string CapacityPrecondition::to_string() const {
  std::string out = "generation=";
  out.append(expected_capacity_generation.to_string());
  out.append(" revision=");
  out.append(expected_revision.to_string());
  out.append(" epoch=");
  out.append(expected_epoch.to_string());
  out.append(" incarnation=");
  out.append(expected_incarnation.to_string());
  out.append(" attempt=");
  out.append(attempt.to_string());
  return out;
}

class FacilityCapacityModel::Impl {
 public:
  ModelConfig config;
  Clock* clock = nullptr;
  mutable std::shared_mutex mutex;
  internal::ModelState state;
  std::shared_ptr<const CapacitySnapshot> published;

  /// True when `attempt` is a recorded idempotency token. Caller holds the lock.
  bool is_replay(const AttemptId& attempt) const {
    if (attempt.is_zero()) {
      return false;
    }
    return std::find(state.attempts.begin(), state.attempts.end(), attempt) != state.attempts.end();
  }

  /// Records an idempotency token, keeping the window bounded. Caller holds the
  /// lock.
  void record_attempt(const AttemptId& attempt) {
    if (attempt.is_zero()) {
      return;
    }
    const auto existing = std::find(state.attempts.begin(), state.attempts.end(), attempt);
    if (existing != state.attempts.end()) {
      return;
    }
    state.attempts.push_back(attempt);
    if (state.attempts.size() > limits::max_recorded_attempts) {
      const std::size_t excess = state.attempts.size() - limits::max_recorded_attempts;
      state.attempts.erase(state.attempts.begin(),
                           state.attempts.begin() + static_cast<std::ptrdiff_t>(excess));
    }
  }

  /// Validation precedence: authority before generation before revision.
  /// Caller holds the lock.
  Status check_precondition(const CapacityPrecondition& precondition) const {
    if (precondition.expected_epoch != state.epoch) {
      return stale_authority("the control-plane epoch has moved on", precondition.expected_epoch.value(),
                             state.epoch.value());
    }
    if (precondition.expected_incarnation != state.incarnation) {
      return stale_authority("the controller incarnation has been superseded",
                             precondition.expected_incarnation.value(), state.incarnation.value());
    }
    if (precondition.expected_capacity_generation != state.capacity_generation) {
      return stale("the capacity generation has moved on",
                   precondition.expected_capacity_generation.value(),
                   state.capacity_generation.value());
    }
    if (precondition.expected_revision != state.revision) {
      return stale("the model revision has moved on", precondition.expected_revision.value(),
                   state.revision.value());
    }
    return Status::success();
  }

  /// Caller holds the lock. Returns the index of the evidence record, or
  /// `state.evidence.size()`.
  std::size_t find_evidence(const CapacitySourceId& source, CapacityDimension dimension) const {
    for (std::size_t index = 0; index < state.evidence.size(); ++index) {
      if (state.evidence[index].source() == source && state.evidence[index].dimension() == dimension) {
        return index;
      }
    }
    return state.evidence.size();
  }

  std::size_t find_reserve(const ReserveId& id) const {
    for (std::size_t index = 0; index < state.reserves.size(); ++index) {
      if (state.reserves[index].id() == id) {
        return index;
      }
    }
    return state.reserves.size();
  }

  std::size_t find_constraint(const ConstraintId& id) const {
    for (std::size_t index = 0; index < state.constraints.size(); ++index) {
      if (state.constraints[index].id() == id) {
        return index;
      }
    }
    return state.constraints.size();
  }

  std::size_t find_requirement(CapacityDimension dimension) const {
    for (std::size_t index = 0; index < state.requirements.size(); ++index) {
      if (state.requirements[index].dimension == dimension) {
        return index;
      }
    }
    return state.requirements.size();
  }

  /// Caller holds the lock. Sorts the mutable collections into canonical order.
  void sort_state() {
    std::sort(state.evidence.begin(), state.evidence.end(),
              [](const SourceEvidence& lhs, const SourceEvidence& rhs) {
                if (lhs.dimension() != rhs.dimension()) {
                  return dimension_ordinal(lhs.dimension()) < dimension_ordinal(rhs.dimension());
                }
                return lhs.source() < rhs.source();
              });
    std::sort(state.reserves.begin(), state.reserves.end(),
              [](const CapacityReserve& lhs, const CapacityReserve& rhs) {
                if (lhs.dimension() != rhs.dimension()) {
                  return dimension_ordinal(lhs.dimension()) < dimension_ordinal(rhs.dimension());
                }
                if (lhs.source() != rhs.source()) {
                  return lhs.source() < rhs.source();
                }
                return lhs.id() < rhs.id();
              });
    std::sort(state.constraints.begin(), state.constraints.end(),
              [](const ServiceConstraint& lhs, const ServiceConstraint& rhs) {
                if (lhs.dimension() != rhs.dimension()) {
                  return dimension_ordinal(lhs.dimension()) < dimension_ordinal(rhs.dimension());
                }
                return lhs.id() < rhs.id();
              });
  }

  /// Caller holds the lock. Builds the snapshot input for `generation`.
  Result<CapacitySnapshotInput> make_input(CapacityGeneration generation, Revision revision, Tick built_at,
                                           SnapshotFreshness freshness) const {
    CapacitySnapshotInput input;
    input.facility = state.facility;
    input.site = state.site;
    input.generation = generation;
    input.epoch = state.epoch;
    input.incarnation = state.incarnation;
    input.revision = revision;
    input.built_at = built_at;
    if (state.snapshot_validity_ticks.is_zero()) {
      input.valid_until = max_tick;
    } else {
      const Result<std::uint64_t> end =
          internal::add_u64(built_at.value(), state.snapshot_validity_ticks.value(),
                            "built_at + snapshot_validity_ticks within uint64");
      if (!end.has_value()) {
        return end.error();
      }
      input.valid_until = Tick::from_value(end.value());
    }
    input.freshness = freshness;
    input.requirements = state.requirements;
    input.evidence = state.evidence;
    input.reserves = state.reserves;
    input.constraints = state.constraints;
    return input;
  }
};

FacilityCapacityModel::FacilityCapacityModel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

FacilityCapacityModel::~FacilityCapacityModel() = default;

FacilityCapacityModel::FacilityCapacityModel(FacilityCapacityModel&&) noexcept = default;

FacilityCapacityModel& FacilityCapacityModel::operator=(FacilityCapacityModel&&) noexcept = default;

Result<FacilityCapacityModel> FacilityCapacityModel::create(ModelConfig config, Clock& clock) {
  if (config.facility.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a model must name a facility");
  }
  if (config.site.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a model must name a site");
  }
  if (config.epoch.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a model must be created inside a non-zero epoch");
  }
  if (config.incarnation.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a model must be created with a non-zero incarnation");
  }
  if (config.max_sources == 0 || config.max_sources > limits::max_evidence_sources) {
    return Error::make(ErrorCode::invalid_argument, "the source bound is outside the accepted range");
  }
  if (config.max_reserves == 0 || config.max_reserves > limits::max_reserves) {
    return Error::make(ErrorCode::invalid_argument, "the reserve bound is outside the accepted range");
  }
  if (config.max_constraints == 0 || config.max_constraints > limits::max_service_constraints) {
    return Error::make(ErrorCode::invalid_argument, "the constraint bound is outside the accepted range");
  }
  const Status normalized = normalize_requirements(config.requirements);
  if (!normalized.has_value()) {
    return normalized.error();
  }

  auto impl = std::make_unique<Impl>();
  impl->clock = &clock;
  impl->state.facility = config.facility;
  impl->state.site = config.site;
  impl->state.epoch = config.epoch;
  impl->state.incarnation = config.incarnation;
  impl->state.snapshot_validity_ticks = config.snapshot_validity_ticks;
  impl->state.max_sources = config.max_sources;
  impl->state.max_reserves = config.max_reserves;
  impl->state.max_constraints = config.max_constraints;
  impl->state.requirements = std::move(config.requirements);
  impl->config = std::move(config);

  FacilityCapacityModel model(std::move(impl));
  return model;
}

const FacilityId& FacilityCapacityModel::facility() const { return impl_->state.facility; }

const SiteId& FacilityCapacityModel::site() const { return impl_->state.site; }

CapacityGeneration FacilityCapacityModel::capacity_generation() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.capacity_generation;
}

Revision FacilityCapacityModel::revision() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.revision;
}

EpochId FacilityCapacityModel::epoch() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.epoch;
}

IncarnationId FacilityCapacityModel::incarnation() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.incarnation;
}

Tick FacilityCapacityModel::snapshot_validity_ticks() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.snapshot_validity_ticks;
}

std::vector<CapacityRequirement> FacilityCapacityModel::requirements() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.requirements;
}

CapacityPrecondition FacilityCapacityModel::current_precondition() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  CapacityPrecondition precondition;
  precondition.expected_capacity_generation = impl_->state.capacity_generation;
  precondition.expected_revision = impl_->state.revision;
  precondition.expected_epoch = impl_->state.epoch;
  precondition.expected_incarnation = impl_->state.incarnation;
  return precondition;
}

Status FacilityCapacityModel::validate() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->state.facility.empty() || impl_->state.site.empty()) {
    return Error::make(ErrorCode::invariant_violation, "the model has no facility or site identity");
  }
  if (impl_->state.evidence.size() > impl_->state.max_sources * capacity_dimension_count) {
    return Error::make(ErrorCode::invariant_violation, "the model holds more evidence than its bound");
  }
  if (impl_->state.reserves.size() > impl_->state.max_reserves) {
    return Error::make(ErrorCode::invariant_violation, "the model holds more reserves than its bound");
  }
  if (impl_->state.constraints.size() > impl_->state.max_constraints) {
    return Error::make(ErrorCode::invariant_violation, "the model holds more constraints than its bound");
  }
  for (std::size_t index = 0; index < impl_->state.evidence.size(); ++index) {
    for (std::size_t other = index + 1; other < impl_->state.evidence.size(); ++other) {
      if (impl_->state.evidence[index].source() == impl_->state.evidence[other].source() &&
          impl_->state.evidence[index].dimension() == impl_->state.evidence[other].dimension()) {
        return Error::make(ErrorCode::invariant_violation,
                           "the model holds two evidence records for one source and dimension");
      }
    }
  }
  for (std::size_t index = 1; index < impl_->state.requirements.size(); ++index) {
    if (impl_->state.requirements[index].dimension == impl_->state.requirements[index - 1].dimension) {
      return Error::make(ErrorCode::invariant_violation, "the model holds two requirements for one dimension");
    }
  }
  return Status::success();
}

Result<SourceEvidence> FacilityCapacityModel::evidence(const CapacitySourceId& source,
                                                       CapacityDimension dimension) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const std::size_t index = impl_->find_evidence(source, dimension);
  if (index == impl_->state.evidence.size()) {
    Error error = Error::make(ErrorCode::not_found, "no evidence is held for that source and dimension");
    error.with_constraint(source.value() + "/" + std::string(capacity_dimension_name(dimension)));
    return error;
  }
  return impl_->state.evidence[index];
}

std::vector<SourceEvidence> FacilityCapacityModel::all_evidence() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.evidence;
}

std::vector<CapacityReserve> FacilityCapacityModel::all_reserves() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.reserves;
}

std::vector<ServiceConstraint> FacilityCapacityModel::all_constraints() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.constraints;
}

std::shared_ptr<const CapacitySnapshot> FacilityCapacityModel::current_snapshot() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->published;
}

CapacityGeneration FacilityCapacityModel::published_generation() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.capacity_generation;
}

Status FacilityCapacityModel::declare_evidence(SourceEvidence source_evidence,
                                               const CapacityPrecondition& precondition) {
  // The clock is sampled before the lock is taken: no call-out happens while
  // the model lock is held.
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  if (source_evidence.provenance().epoch != impl_->state.epoch) {
    return stale_authority("the evidence was produced under a different control-plane epoch",
                           impl_->state.epoch.value(), source_evidence.provenance().epoch.value());
  }

  const std::size_t index = impl_->find_evidence(source_evidence.source(), source_evidence.dimension());
  if (index != impl_->state.evidence.size()) {
    const EvidenceGeneration current = impl_->state.evidence[index].generation();
    if (!(current < source_evidence.generation())) {
      Error error = Error::make(ErrorCode::stale_generation,
                                "the evidence generation is not newer than the one already held");
      error.with_generations(current.value() + 1u, source_evidence.generation().value());
      return error;
    }
    impl_->state.evidence[index] = std::move(source_evidence);
  } else {
    if (impl_->state.evidence.size() >= impl_->config.max_sources * capacity_dimension_count) {
      return Error::make(ErrorCode::limit_exceeded, "the model holds as many evidence records as it accepts");
    }
    impl_->state.evidence.push_back(std::move(source_evidence));
  }
  impl_->sort_state();
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::retire_evidence(const CapacitySourceId& source, CapacityDimension dimension,
                                              const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  const std::size_t index = impl_->find_evidence(source, dimension);
  if (index == impl_->state.evidence.size()) {
    Error error = Error::make(ErrorCode::not_found, "no evidence is held for that source and dimension");
    error.with_constraint(source.value() + "/" + std::string(capacity_dimension_name(dimension)));
    return error;
  }
  impl_->state.evidence.erase(impl_->state.evidence.begin() + static_cast<std::ptrdiff_t>(index));
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::declare_reserve(CapacityReserve reserve,
                                              const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  if (reserve.provenance().epoch != impl_->state.epoch) {
    return stale_authority("the reserve was declared under a different control-plane epoch",
                           impl_->state.epoch.value(), reserve.provenance().epoch.value());
  }
  const std::size_t index = impl_->find_reserve(reserve.id());
  if (index != impl_->state.reserves.size()) {
    const Tick current = impl_->state.reserves[index].provenance().produced_at;
    if (reserve.provenance().produced_at < current) {
      Error error = Error::make(ErrorCode::stale_generation,
                                "the reserve declaration is older than the one already held");
      error.with_generations(current.value(), reserve.provenance().produced_at.value());
      return error;
    }
    impl_->state.reserves[index] = std::move(reserve);
  } else {
    if (impl_->state.reserves.size() >= impl_->config.max_reserves) {
      return Error::make(ErrorCode::limit_exceeded, "the model holds as many reserves as it accepts");
    }
    impl_->state.reserves.push_back(std::move(reserve));
  }
  impl_->sort_state();
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::withdraw_reserve(const ReserveId& id,
                                               const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  const std::size_t index = impl_->find_reserve(id);
  if (index == impl_->state.reserves.size()) {
    Error error = Error::make(ErrorCode::not_found, "no reserve with that identity is declared");
    error.with_constraint(id.value());
    return error;
  }
  impl_->state.reserves.erase(impl_->state.reserves.begin() + static_cast<std::ptrdiff_t>(index));
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::declare_constraint(ServiceConstraint constraint,
                                                 const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  if (constraint.provenance().epoch != impl_->state.epoch) {
    return stale_authority("the constraint was declared under a different control-plane epoch",
                           impl_->state.epoch.value(), constraint.provenance().epoch.value());
  }
  const std::size_t index = impl_->find_constraint(constraint.id());
  if (index != impl_->state.constraints.size()) {
    const Tick current = impl_->state.constraints[index].provenance().produced_at;
    if (constraint.provenance().produced_at < current) {
      Error error = Error::make(ErrorCode::stale_generation,
                                "the constraint declaration is older than the one already held");
      error.with_generations(current.value(), constraint.provenance().produced_at.value());
      return error;
    }
    impl_->state.constraints[index] = std::move(constraint);
  } else {
    if (impl_->state.constraints.size() >= impl_->config.max_constraints) {
      return Error::make(ErrorCode::limit_exceeded, "the model holds as many constraints as it accepts");
    }
    impl_->state.constraints.push_back(std::move(constraint));
  }
  impl_->sort_state();
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::withdraw_constraint(const ConstraintId& id,
                                                  const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  const std::size_t index = impl_->find_constraint(id);
  if (index == impl_->state.constraints.size()) {
    Error error = Error::make(ErrorCode::not_found, "no service constraint with that identity is declared");
    error.with_constraint(id.value());
    return error;
  }
  impl_->state.constraints.erase(impl_->state.constraints.begin() + static_cast<std::ptrdiff_t>(index));
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::set_requirement(CapacityRequirement requirement,
                                              const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  const Status normalized = requirement.normalize();
  if (!normalized.has_value()) {
    return normalized.error();
  }
  const std::size_t index = impl_->find_requirement(requirement.dimension);
  if (index != impl_->state.requirements.size()) {
    impl_->state.requirements[index] = std::move(requirement);
  } else {
    if (impl_->state.requirements.size() >= limits::max_requirements) {
      return Error::make(ErrorCode::limit_exceeded, "the model holds as many requirements as it accepts");
    }
    impl_->state.requirements.push_back(std::move(requirement));
  }
  std::sort(impl_->state.requirements.begin(), impl_->state.requirements.end(),
            [](const CapacityRequirement& lhs, const CapacityRequirement& rhs) {
              return dimension_ordinal(lhs.dimension) < dimension_ordinal(rhs.dimension);
            });
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Status FacilityCapacityModel::advance_epoch(EpochId new_epoch, IncarnationId new_incarnation,
                                            const CapacityPrecondition& precondition) {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    return Status::success();
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }
  if (new_epoch < impl_->state.epoch) {
    Error error = Error::make(ErrorCode::invalid_argument, "the control-plane epoch cannot move backwards");
    error.with_generations(impl_->state.epoch.value(), new_epoch.value());
    return error;
  }
  if (new_epoch == impl_->state.epoch && new_incarnation == impl_->state.incarnation) {
    return Error::make(ErrorCode::invalid_argument,
                       "advancing authority requires a greater epoch or a different incarnation");
  }
  if (new_incarnation.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a controller incarnation is never zero");
  }
  impl_->state.epoch = new_epoch;
  impl_->state.incarnation = new_incarnation;
  const Status bumped = bump_revision(impl_->state);
  if (!bumped.has_value()) {
    return bumped.error();
  }
  impl_->record_attempt(precondition.attempt);
  return Status::success();
}

Result<std::shared_ptr<const CapacitySnapshot>> FacilityCapacityModel::publish(
    const CapacityPrecondition& precondition) {
  // Sampled before the lock: the clock is never called with the model lock held.
  const Result<Tick> now = impl_->clock->now();
  if (!now.has_value()) {
    return now.error();
  }

  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->is_replay(precondition.attempt)) {
    if (impl_->published != nullptr) {
      return impl_->published;
    }
    return Error::make(ErrorCode::already_exists, "the publish attempt was already applied");
  }
  const Status precondition_status = impl_->check_precondition(precondition);
  if (!precondition_status.has_value()) {
    return precondition_status.error();
  }

  const Result<CapacityGeneration> next_generation = impl_->state.capacity_generation.next();
  if (!next_generation.has_value()) {
    return next_generation.error();
  }
  const Result<Revision> next_revision = impl_->state.revision.next();
  if (!next_revision.has_value()) {
    return next_revision.error();
  }

  Result<CapacitySnapshotInput> input = impl_->make_input(next_generation.value(), next_revision.value(),
                                                          now.value(), SnapshotFreshness::issued);
  if (!input.has_value()) {
    return input.error();
  }
  Result<std::shared_ptr<const CapacitySnapshot>> snapshot =
      CapacitySnapshot::build(std::move(input.value()));
  if (!snapshot.has_value()) {
    return snapshot.error();
  }

  impl_->state.capacity_generation = next_generation.value();
  impl_->state.revision = next_revision.value();
  impl_->state.published_snapshot_id = snapshot.value()->id().value();
  impl_->state.published_snapshot_digest = snapshot.value()->digest();
  impl_->published = snapshot.value();
  impl_->record_attempt(precondition.attempt);
  return snapshot;
}

Result<CapacityAnswer> FacilityCapacityModel::query(CapacityDimension dimension) const {
  const Result<Tick> now = impl_->clock->now();
  if (!now.has_value()) {
    return now.error();
  }
  std::shared_ptr<const CapacitySnapshot> snapshot;
  {
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    Result<CapacitySnapshotInput> input =
        impl_->make_input(impl_->state.capacity_generation, impl_->state.revision, now.value(),
                          SnapshotFreshness::issued);
    if (!input.has_value()) {
      return input.error();
    }
    Result<std::shared_ptr<const CapacitySnapshot>> built =
        CapacitySnapshot::build(std::move(input.value()));
    if (!built.has_value()) {
      return built.error();
    }
    snapshot = built.value();
  }

  CapacityAnswer answer;
  answer.dimension = dimension;
  answer.unit = canonical_unit(dimension);
  const DimensionTotals* totals = snapshot->find_totals(dimension);
  if (totals != nullptr) {
    answer.totals = *totals;
  } else {
    answer.totals.dimension = dimension;
    answer.totals.unit = canonical_unit(dimension);
  }
  answer.outcome = snapshot->outcome(dimension);
  answer.snapshot_digest = snapshot->digest();
  answer.generation = snapshot->generation();
  for (const CapacityNote& note : snapshot->notes().notes()) {
    if (!note.has_dimension || note.dimension == dimension) {
      (void)answer.reasons.add(note);
    }
  }
  return answer;
}

Result<RevalidationReport> FacilityCapacityModel::revalidate(const CapacitySnapshot& snapshot) const {
  // Sampled before the lock: the clock is never called with the model lock held.
  const Result<Tick> now = impl_->clock->now();

  RevalidationReport report;
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);

  if (snapshot.facility() != impl_->state.facility || snapshot.site() != impl_->state.site) {
    Error error = Error::make(ErrorCode::conflict, "the snapshot describes a different facility or site");
    error.with_constraint("snapshot.facility == model.facility");
    return error;
  }

  report.snapshot_generation = snapshot.generation();
  report.current_generation = impl_->state.capacity_generation;
  report.current_revision = impl_->state.revision;

  std::uint8_t severity = revalidation_status_severity(RevalidationStatus::valid);
  const auto raise = [&severity, &report](RevalidationStatus status) {
    const std::uint8_t candidate = revalidation_status_severity(status);
    if (candidate > severity) {
      severity = candidate;
      report.status = status;
    }
  };

  if (report.current_generation > report.snapshot_generation) {
    (void)report.findings.add(CapacityNote(
        ReasonCode::capacity_generation_advanced, CapacityDimension::space, CapacitySourceId(),
        "a newer capacity generation has been published since the snapshot was issued"));
    raise(RevalidationStatus::superseded);
  }

  if (snapshot.epoch() != impl_->state.epoch) {
    (void)report.findings.add(CapacityNote(ReasonCode::source_epoch_mismatch, CapacityDimension::space,
                                           CapacitySourceId(),
                                           "the control-plane epoch has changed since the snapshot was issued"));
    raise(RevalidationStatus::stale);
  }
  if (snapshot.incarnation() != impl_->state.incarnation) {
    (void)report.findings.add(CapacityNote(
        ReasonCode::source_incarnation_mismatch, CapacityDimension::space, CapacitySourceId(),
        "the controller incarnation has changed since the snapshot was issued"));
    raise(RevalidationStatus::stale);
  }

  if (now.has_value() && now.value() > snapshot.valid_until()) {
    (void)report.findings.add(CapacityNote(ReasonCode::snapshot_expired, CapacityDimension::space,
                                           CapacitySourceId(),
                                           "the snapshot validity window has passed"));
    raise(RevalidationStatus::stale);
  }

  for (const CapacityRequirement& requirement : impl_->state.requirements) {
    const auto required = requirement.required_sources;
    for (const CapacitySourceId& source : required) {
      const std::size_t index = impl_->find_evidence(source, requirement.dimension);
      if (index == impl_->state.evidence.size()) {
        (void)report.findings.add(CapacityNote(ReasonCode::source_not_reported, requirement.dimension,
                                               source, "a required source holds no current evidence"));
        raise(RevalidationStatus::incomplete);
      }
    }
  }

  if (snapshot.requirements() != impl_->state.requirements) {
    (void)report.findings.add(CapacityNote(ReasonCode::coverage_contract_changed, CapacityDimension::space,
                                           CapacitySourceId(),
                                           "the coverage contract has changed since the snapshot was issued"));
    raise(RevalidationStatus::stale);
  }

  for (const SourceGenerationStamp& stamp : snapshot.sources()) {
    const std::size_t index = impl_->find_evidence(stamp.source, stamp.dimension);
    if (index == impl_->state.evidence.size()) {
      (void)report.findings.add(CapacityNote(ReasonCode::source_superseded, stamp.dimension, stamp.source,
                                             "the source no longer holds evidence"));
      raise(RevalidationStatus::stale);
      continue;
    }
    const SourceEvidence& current = impl_->state.evidence[index];
    if (current.generation() != stamp.generation) {
      (void)report.findings.add(CapacityNote(ReasonCode::source_stale, stamp.dimension, stamp.source,
                                             "the source has published a newer evidence generation"));
      raise(RevalidationStatus::stale);
      continue;
    }
    if (current.digest() != stamp.evidence_digest) {
      (void)report.findings.add(CapacityNote(ReasonCode::source_superseded, stamp.dimension, stamp.source,
                                             "the source's evidence content has changed"));
      raise(RevalidationStatus::stale);
    }
  }

  return report;
}

Result<std::string> FacilityCapacityModel::encode() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  internal::CanonicalWriter writer;
  writer.field("document", "model-state");
  writer.field("canonical_format", static_cast<std::uint64_t>(canonical_format_version));
  internal::encode_model_state_fields(writer, impl_->state);
  return writer.finish();
}

Result<std::string> FacilityCapacityModel::state_digest() const {
  const Result<std::string> encoded = encode();
  if (!encoded.has_value()) {
    return encoded.error();
  }
  const std::string& document = encoded.value();
  if (document.size() < 72) {
    return Error::make(ErrorCode::invariant_violation, "the encoded model state has no digest line");
  }
  return document.substr(document.size() - 65u, 64u);
}

Result<FacilityCapacityModel> FacilityCapacityModel::decode(std::string_view canonical_body, Clock& clock) {
  Result<internal::CanonicalReader> reader = internal::CanonicalReader::parse(canonical_body);
  if (!reader.has_value()) {
    return reader.error();
  }
  const Result<void> kind = reader.value().expect("document", "model-state");
  if (!kind.has_value()) {
    return kind.error();
  }
  const Result<void> format =
      reader.value().expect("canonical_format", std::to_string(canonical_format_version));
  if (!format.has_value()) {
    return format.error();
  }
  Result<internal::ModelState> state = internal::decode_model_state_fields(reader.value());
  if (!state.has_value()) {
    return state.error();
  }
  const Result<void> finished = reader.value().finish();
  if (!finished.has_value()) {
    return finished.error();
  }

  auto impl = std::make_unique<Impl>();
  impl->clock = &clock;
  impl->state = std::move(state.value());
  impl->config.facility = impl->state.facility;
  impl->config.site = impl->state.site;
  impl->config.epoch = impl->state.epoch;
  impl->config.incarnation = impl->state.incarnation;
  impl->config.requirements = impl->state.requirements;
  impl->config.snapshot_validity_ticks = impl->state.snapshot_validity_ticks;
  impl->config.max_sources = impl->state.max_sources;
  impl->config.max_reserves = impl->state.max_reserves;
  impl->config.max_constraints = impl->state.max_constraints;

  FacilityCapacityModel model(std::move(impl));
  const Result<std::string> reencoded = model.encode();
  if (!reencoded.has_value()) {
    return reencoded.error();
  }
  if (reencoded.value() != canonical_body) {
    return Error::make(ErrorCode::corruption, "the model state document does not reproduce from its own fields");
  }
  return model;
}

}  // namespace dccp::facility_capacity
