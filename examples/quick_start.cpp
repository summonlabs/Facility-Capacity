// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Quick start: compose a facility capacity answer, ask one question of it,
// publish it, change a source and diff the two published answers.
//
// EVERY QUANTITY IN THIS FILE IS SYNTHETIC. The numbers are round, invented
// values chosen to make the arithmetic visible; they describe no real facility.
//
// The program is deterministic. It runs against a `ManualClock` that it
// advances itself, so it reads no wall clock, no environment, no locale and no
// network, and two runs over the same inputs produce the same bytes.
//
// Every `Result` is checked. A failure prints one machine-readable line to
// standard error and returns 1; success returns 0.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"

namespace {

namespace fc = dccp::facility_capacity;

/// SYNTHETIC control-plane authority for this example.
constexpr std::uint64_t kEpoch = 7;
constexpr std::uint64_t kIncarnation = 3;

/// SYNTHETIC provenance revision of the sources in this example.
constexpr std::string_view kRevision = "example-quick-start/1.0.0";

/// SYNTHETIC identity of the operator that owns this model.
constexpr std::string_view kOwner = "facility-ops";

/// A deterministic failure line, then the process exit code for that failure.
int fail(const fc::Error& error) {
  std::string line = "error: ";
  line.append(fc::error_code_name(error.code()));
  line.append(": ");
  line.append(error.message());
  if (!error.constraint().empty()) {
    line.append(" [constraint=");
    line.append(error.constraint());
    line.push_back(']');
  }
  if (error.has_generations()) {
    line.append(" [expected_generation=");
    line.append(std::to_string(error.expected_generation()));
    line.append(" actual_generation=");
    line.append(std::to_string(error.actual_generation()));
    line.push_back(']');
  }
  std::fputs(line.c_str(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

/// True when the call failed; the failure has already been reported.
template <class T>
bool failed(const fc::Result<T>& result) {
  if (result.has_value()) {
    return false;
  }
  (void)fail(result.error());
  return true;
}

/// SYNTHETIC inputs for one evidence record.
struct SyntheticEvidence {
  std::string source;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  std::uint64_t generation = 1;
  std::int64_t installed = 0;
  std::int64_t usable = 0;
  std::int64_t unavailable = 0;
  std::int64_t reserved = 0;
  /// When false the protection roll-up is left UNMEASURED, which is what makes
  /// an itemised protection reserve declared against this source the value that
  /// counts. Declaring both a roll-up and itemised reserves that disagree makes
  /// the derivation exclude the source, and the library says so.
  bool protection_rollup_measured = true;
  std::int64_t protected_capacity = 0;
  fc::OperationalState state = fc::OperationalState::nominal;
  std::string detail;
};

/// Builds one immutable evidence record. `residual` is derived by the library
/// as `installed - unavailable - usable`; `observed` is left unmeasured, which
/// is not the same as zero.
fc::Result<fc::SourceEvidence> build_evidence(const SyntheticEvidence& spec, fc::Tick observed_at) {
  const fc::Unit unit = fc::canonical_unit(spec.dimension);
  const fc::Result<fc::CapacitySourceId> source = fc::CapacitySourceId::parse(spec.source);
  if (!source.has_value()) {
    return source.error();
  }
  const fc::Result<fc::Measured> installed = fc::Measured::known(unit, spec.installed);
  if (!installed.has_value()) {
    return installed.error();
  }
  const fc::Result<fc::Measured> usable = fc::Measured::known(unit, spec.usable);
  if (!usable.has_value()) {
    return usable.error();
  }
  const fc::Result<fc::Measured> unavailable = fc::Measured::known(unit, spec.unavailable);
  if (!unavailable.has_value()) {
    return unavailable.error();
  }
  const fc::Result<fc::Measured> reserved = fc::Measured::known(unit, spec.reserved);
  if (!reserved.has_value()) {
    return reserved.error();
  }

  fc::SourceEvidenceFields fields;
  fields.source = source.value();
  fields.dimension = spec.dimension;
  fields.unit = unit;
  fields.generation = fc::EvidenceGeneration::from_value(spec.generation);
  fields.observed_at = observed_at;
  fields.provenance.source = source.value();
  fields.provenance.revision = std::string(kRevision);
  fields.provenance.produced_at = observed_at;
  fields.provenance.epoch = fc::EpochId::from_value(kEpoch);
  fields.provenance.incarnation = fc::IncarnationId::from_value(kIncarnation);
  fields.state = spec.state;
  fields.state_detail = spec.detail;
  fields.installed = installed.value();
  fields.usable = usable.value();
  fields.unavailable = unavailable.value();
  fields.reserved = reserved.value();
  fields.derive_residual = true;
  if (spec.protection_rollup_measured) {
    const fc::Result<fc::Measured> protection = fc::Measured::known(unit, spec.protected_capacity);
    if (!protection.has_value()) {
      return protection.error();
    }
    fields.protected_capacity = protection.value();
  }
  return fc::SourceEvidence::create(std::move(fields));
}

/// One protection reserve: capacity that must stay free so that a failure can
/// be absorbed.
fc::Result<fc::CapacityReserve> build_protection_reserve(std::string_view id, std::string_view source,
                                                         fc::CapacityDimension dimension, std::int64_t amount,
                                                         fc::Tick produced_at) {
  const fc::Result<fc::ReserveId> reserve_id = fc::ReserveId::parse(id);
  if (!reserve_id.has_value()) {
    return reserve_id.error();
  }
  const fc::Result<fc::CapacitySourceId> source_id = fc::CapacitySourceId::parse(source);
  if (!source_id.has_value()) {
    return source_id.error();
  }
  const fc::Result<fc::OwnerId> owner = fc::OwnerId::parse(kOwner);
  if (!owner.has_value()) {
    return owner.error();
  }
  const fc::Result<fc::Measured> measured = fc::Measured::known(fc::canonical_unit(dimension), amount);
  if (!measured.has_value()) {
    return measured.error();
  }

  fc::CapacityReserveFields fields;
  fields.id = reserve_id.value();
  fields.source = source_id.value();
  fields.dimension = dimension;
  fields.unit = fc::canonical_unit(dimension);
  fields.kind = fc::ReserveKind::protection;
  fields.amount = measured.value();
  fields.window = fc::ValidityWindow();
  fields.owner = owner.value();
  fields.provenance.source = source_id.value();
  fields.provenance.revision = std::string(kRevision);
  fields.provenance.produced_at = produced_at;
  fields.provenance.epoch = fc::EpochId::from_value(kEpoch);
  fields.provenance.incarnation = fc::IncarnationId::from_value(kIncarnation);
  return fc::CapacityReserve::create(std::move(fields));
}

/// One service floor: allocatable capacity that must remain available.
fc::Result<fc::ServiceConstraint> build_constraint(std::string_view id, fc::CapacityDimension dimension,
                                                   std::int64_t floor_value, std::string_view service_class,
                                                   fc::Tick produced_at) {
  const fc::Result<fc::ConstraintId> constraint_id = fc::ConstraintId::parse(id);
  if (!constraint_id.has_value()) {
    return constraint_id.error();
  }
  const fc::Result<fc::ServiceClassId> service = fc::ServiceClassId::parse(service_class);
  if (!service.has_value()) {
    return service.error();
  }
  const fc::Result<fc::OwnerId> owner = fc::OwnerId::parse(kOwner);
  if (!owner.has_value()) {
    return owner.error();
  }
  const fc::Result<fc::Measured> measured = fc::Measured::known(fc::canonical_unit(dimension), floor_value);
  if (!measured.has_value()) {
    return measured.error();
  }
  // A service constraint is declared *to* the model by the operator that owns
  // it, so its provenance names the operator rather than a capacity source.
  const fc::Result<fc::CapacitySourceId> operator_source = fc::CapacitySourceId::parse(kOwner);
  if (!operator_source.has_value()) {
    return operator_source.error();
  }

  fc::ServiceConstraintFields fields;
  fields.id = constraint_id.value();
  fields.dimension = dimension;
  fields.unit = fc::canonical_unit(dimension);
  fields.service_class = service.value();
  fields.floor = measured.value();
  fields.window = fc::ValidityWindow();
  fields.owner = owner.value();
  fields.provenance.source = operator_source.value();
  fields.provenance.revision = std::string(kRevision);
  fields.provenance.produced_at = produced_at;
  fields.provenance.epoch = fc::EpochId::from_value(kEpoch);
  fields.provenance.incarnation = fc::IncarnationId::from_value(kIncarnation);
  return fc::ServiceConstraint::create(std::move(fields));
}

/// Applies one mutation under a precondition derived from the model's current
/// state, with a fresh non-zero idempotency token.
fc::Status declare(fc::FacilityCapacityModel& model, const fc::SourceEvidence& evidence, std::uint64_t attempt) {
  fc::CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = fc::AttemptId::from_value(attempt);
  return model.declare_evidence(evidence, precondition);
}

fc::Status declare(fc::FacilityCapacityModel& model, const fc::CapacityReserve& reserve, std::uint64_t attempt) {
  fc::CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = fc::AttemptId::from_value(attempt);
  return model.declare_reserve(reserve, precondition);
}

fc::Status declare(fc::FacilityCapacityModel& model, const fc::ServiceConstraint& constraint,
                   std::uint64_t attempt) {
  fc::CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = fc::AttemptId::from_value(attempt);
  return model.declare_constraint(constraint, precondition);
}

/// Publishes the next capacity generation. Publication always advances the
/// generation by exactly one, so the precondition is re-derived from the state
/// the mutation left behind.
fc::Result<std::shared_ptr<const fc::CapacitySnapshot>> publish(fc::FacilityCapacityModel& model,
                                                                std::uint64_t attempt) {
  fc::CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = fc::AttemptId::from_value(attempt);
  return model.publish(precondition);
}

/// SYNTHETIC sources of one facility. Each record is a different source
/// reporting a different dimension, so the four dimensions are answered by five
/// independent reports.
std::vector<SyntheticEvidence> synthetic_sources() {
  std::vector<SyntheticEvidence> sources;

  SyntheticEvidence space;
  space.source = "space-pod-a";
  space.dimension = fc::CapacityDimension::space;
  space.installed = 480;  // SYNTHETIC: 480 rack units of vertical space
  space.usable = 440;
  space.unavailable = 40;
  sources.push_back(space);

  SyntheticEvidence rack;
  rack.source = "rack-row-a";
  rack.dimension = fc::CapacityDimension::rack;
  rack.installed = 24;  // SYNTHETIC: 24 rack positions
  rack.usable = 22;
  rack.unavailable = 2;
  sources.push_back(rack);

  SyntheticEvidence feed_a;
  feed_a.source = "power-feed-a";
  feed_a.dimension = fc::CapacityDimension::power;
  feed_a.installed = 4'000'000;  // SYNTHETIC: 4.0 kW of IT load
  feed_a.usable = 3'600'000;
  feed_a.unavailable = 400'000;
  // The protection held back on this feed is declared itemised, below, so the
  // roll-up stays unmeasured rather than being restated as a number.
  feed_a.protection_rollup_measured = false;
  sources.push_back(feed_a);

  SyntheticEvidence feed_b;
  feed_b.source = "power-feed-b";
  feed_b.dimension = fc::CapacityDimension::power;
  feed_b.installed = 2'000'000;  // SYNTHETIC: 2.0 kW of IT load
  feed_b.usable = 2'000'000;
  feed_b.unavailable = 0;
  sources.push_back(feed_b);

  SyntheticEvidence cooling;
  cooling.source = "cooling-loop-a";
  cooling.dimension = fc::CapacityDimension::cooling;
  cooling.installed = 5'000'000;  // SYNTHETIC: 5.0 kW of heat removal
  cooling.usable = 4'500'000;
  cooling.unavailable = 500'000;
  sources.push_back(cooling);

  return sources;
}

/// SYNTHETIC follow-up report: the same feed, degraded, with far less usable
/// capacity. A new evidence generation is the only way a source changes its
/// story; the previous record stays exactly as it was.
SyntheticEvidence degraded_feed_a() {
  SyntheticEvidence feed;
  feed.source = "power-feed-a";
  feed.dimension = fc::CapacityDimension::power;
  feed.generation = 2;
  feed.installed = 4'000'000;  // SYNTHETIC: unchanged installed capacity
  feed.usable = 2'400'000;     // SYNTHETIC: a third of the feed is out
  feed.unavailable = 1'600'000;
  feed.protection_rollup_measured = false;
  feed.state = fc::OperationalState::degraded;
  feed.detail = "SYNTHETIC: half of feed A is out for maintenance";
  return feed;
}

}  // namespace

int main() {
  // The model's timeline is injected and advanced by this program alone.
  fc::ManualClock clock(fc::Tick::from_value(1'000));

  const fc::Result<fc::FacilityId> facility = fc::FacilityId::parse("facility-a");
  if (failed(facility)) {
    return 1;
  }
  const fc::Result<fc::SiteId> site = fc::SiteId::parse("site-1");
  if (failed(site)) {
    return 1;
  }

  fc::CapacityRequirement power_requirement;
  power_requirement.dimension = fc::CapacityDimension::power;
  power_requirement.required = true;
  fc::CapacityRequirement cooling_requirement;
  cooling_requirement.dimension = fc::CapacityDimension::cooling;
  cooling_requirement.required = true;

  fc::ModelConfig config;
  config.facility = facility.value();
  config.site = site.value();
  config.epoch = fc::EpochId::from_value(kEpoch);
  config.incarnation = fc::IncarnationId::from_value(kIncarnation);
  config.requirements = {power_requirement, cooling_requirement};
  config.snapshot_validity_ticks = fc::Tick::from_value(3'600'000'000'000ull);

  fc::Result<fc::FacilityCapacityModel> created = fc::FacilityCapacityModel::create(std::move(config), clock);
  if (failed(created)) {
    return 1;
  }
  fc::FacilityCapacityModel& model = created.value();

  std::uint64_t attempt = 0;

  // --- declare the evidence -------------------------------------------------
  for (const SyntheticEvidence& spec : synthetic_sources()) {
    const fc::Result<fc::SourceEvidence> evidence = build_evidence(spec, clock.current());
    if (failed(evidence)) {
      return 1;
    }
    ++attempt;
    const fc::Status declared = declare(model, evidence.value(), attempt);
    if (failed(declared)) {
      return 1;
    }
  }

  // --- declare one protection reserve and one service floor -----------------
  const fc::Result<fc::CapacityReserve> reserve =
      build_protection_reserve("protection-feed-a", "power-feed-a", fc::CapacityDimension::power,
                               600'000,  // SYNTHETIC: 0.6 kW held back for redundancy
                               clock.current());
  if (failed(reserve)) {
    return 1;
  }
  ++attempt;
  const fc::Status reserve_declared = declare(model, reserve.value(), attempt);
  if (failed(reserve_declared)) {
    return 1;
  }

  const fc::Result<fc::ServiceConstraint> constraint =
      build_constraint("floor-tier-1-power", fc::CapacityDimension::power,
                       1'000'000,  // SYNTHETIC: 1.0 kW must stay allocatable
                       "tier-1", clock.current());
  if (failed(constraint)) {
    return 1;
  }
  ++attempt;
  const fc::Status constraint_declared = declare(model, constraint.value(), attempt);
  if (failed(constraint_declared)) {
    return 1;
  }

  // --- publish the first answer --------------------------------------------
  ++attempt;
  fc::Result<std::shared_ptr<const fc::CapacitySnapshot>> first = publish(model, attempt);
  if (failed(first)) {
    return 1;
  }
  const std::shared_ptr<const fc::CapacitySnapshot> before = first.value();
  std::fputs(before->describe().c_str(), stdout);

  // --- ask one question of it ----------------------------------------------
  const fc::Result<fc::CapacityAnswer> answer = model.query(fc::CapacityDimension::power);
  if (failed(answer)) {
    return 1;
  }
  std::fputs(answer.value().describe().c_str(), stdout);

  // --- advance the injected timeline and change a source -------------------
  const fc::Status advanced = clock.advance(fc::Tick::from_value(60'000'000'000ull));
  if (failed(advanced)) {
    return 1;
  }
  const fc::Result<fc::SourceEvidence> degraded = build_evidence(degraded_feed_a(), clock.current());
  if (failed(degraded)) {
    return 1;
  }
  ++attempt;
  const fc::Status degraded_declared = declare(model, degraded.value(), attempt);
  if (failed(degraded_declared)) {
    return 1;
  }

  // --- publish again and diff the two answers ------------------------------
  ++attempt;
  fc::Result<std::shared_ptr<const fc::CapacitySnapshot>> second = publish(model, attempt);
  if (failed(second)) {
    return 1;
  }
  const std::shared_ptr<const fc::CapacitySnapshot> after = second.value();

  const fc::Result<fc::SnapshotDiff> diff = fc::diff_snapshots(*before, *after);
  if (failed(diff)) {
    return 1;
  }
  std::fputs(diff.value().describe().c_str(), stdout);
  return 0;
}
