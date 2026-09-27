// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical documents for a whole snapshot and a whole model state.
//
// A snapshot document carries:
//   - the complete input description (identity, coverage contract, evidence,
//     reserves, service constraints and the findings raised before assembly);
//   - `derived_digest`, the SHA-256 of the canonical encoding of the derived
//     answer.
//
// A decoder parses the input, re-derives the answer, re-encodes the input and
// compares it byte for byte with the bytes it was given, and recomputes the
// derived digest and compares that too. A document whose derived fields were
// edited is therefore refused even when the outer digest was recomputed to
// match, because the recomputed derived digest would not agree.

#include "internal/documents.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/version.hpp"
#include "internal/codec.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity::internal {
namespace {

std::string key_with(std::string_view prefix, std::string_view suffix) {
  if (prefix.empty()) {
    return std::string(suffix);
  }
  std::string key(prefix);
  key.push_back('.');
  key.append(suffix);
  return key;
}

Error corruption(std::string_view message, std::string_view field) {
  Error error = Error::make(ErrorCode::corruption, message);
  error.with_constraint(std::string(field));
  return error;
}

Result<CapacitySourceId> take_source(const CanonicalReader& reader, std::string_view key) {
  const Result<std::string_view> raw = reader.take(key);
  if (!raw.has_value()) {
    return raw.error();
  }
  if (raw.value().empty()) {
    return CapacitySourceId();
  }
  return CapacitySourceId::parse(raw.value());
}

template <class Id>
Result<Id> take_optional_id(const CanonicalReader& reader, std::string_view key) {
  const Result<std::string_view> raw = reader.take(key);
  if (!raw.has_value()) {
    return raw.error();
  }
  if (raw.value().empty()) {
    return Id();
  }
  return Id::parse(raw.value());
}

void encode_requirements(CanonicalWriter& writer, const std::vector<CapacityRequirement>& requirements) {
  writer.field("requirement_count", static_cast<std::uint64_t>(requirements.size()));
  for (std::size_t index = 0; index < requirements.size(); ++index) {
    encode_requirement(writer, "requirement." + std::to_string(index), requirements[index]);
  }
}

Result<std::vector<CapacityRequirement>> decode_requirements(const CanonicalReader& reader) {
  const Result<std::uint64_t> count = reader.take_u64("requirement_count");
  if (!count.has_value()) {
    return count.error();
  }
  if (count.value() > limits::max_requirements) {
    return corruption("too many coverage requirements", "requirement_count");
  }
  std::vector<CapacityRequirement> requirements;
  requirements.reserve(static_cast<std::size_t>(count.value()));
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    const Result<CapacityRequirement> requirement =
        decode_requirement(reader, "requirement." + std::to_string(index));
    if (!requirement.has_value()) {
      return requirement.error();
    }
    requirements.push_back(requirement.value());
  }
  return requirements;
}

/// Rebuilds the exact field prefix of one indexed block so a decoder failure
/// can name the field it rejected.
std::string index_prefix(std::string_view block, std::uint64_t index) {
  std::string prefix(block);
  prefix.push_back('.');
  prefix.append(std::to_string(index));
  return prefix;
}

}  // namespace

void encode_snapshot_fields(CanonicalWriter& writer, const CapacitySnapshot& snapshot) {
  writer.field("facility", snapshot.facility().value());
  writer.field("site", snapshot.site().value());
  writer.field("generation", snapshot.generation().value());
  writer.field("epoch", snapshot.epoch().value());
  writer.field("incarnation", snapshot.incarnation().value());
  writer.field("revision", snapshot.revision().value());
  writer.field("built_at", snapshot.built_at().value());
  writer.field("valid_until", snapshot.valid_until().value());

  const CapacitySnapshotInput& input = snapshot.input();
  encode_requirements(writer, input.requirements);

  writer.field("evidence_count", static_cast<std::uint64_t>(input.evidence.size()));
  for (std::size_t index = 0; index < input.evidence.size(); ++index) {
    encode_evidence(writer, "evidence." + std::to_string(index), input.evidence[index], true);
  }

  writer.field("reserve_count", static_cast<std::uint64_t>(input.reserves.size()));
  for (std::size_t index = 0; index < input.reserves.size(); ++index) {
    encode_reserve(writer, "reserve." + std::to_string(index), input.reserves[index], true);
  }

  writer.field("constraint_count", static_cast<std::uint64_t>(input.constraints.size()));
  for (std::size_t index = 0; index < input.constraints.size(); ++index) {
    encode_constraint(writer, "constraint." + std::to_string(index), input.constraints[index], true);
  }

  encode_notes(writer, "coverage_note", input.coverage_findings);

  CanonicalWriter derived;
  derived.field("document", "snapshot-derived");
  derived.field("canonical_format", static_cast<std::uint64_t>(canonical_format_version));
  derived.field("accounting_closed", snapshot.accounting_closed());
  derived.field("completeness_classification", completeness_class_name(snapshot.completeness().classification));
  derived.field("completeness_required", static_cast<std::uint64_t>(snapshot.completeness().required_dimensions));
  derived.field("completeness_complete", static_cast<std::uint64_t>(snapshot.completeness().complete_dimensions));

  derived.field("totals_count", static_cast<std::uint64_t>(snapshot.dimension_totals().size()));
  for (std::size_t index = 0; index < snapshot.dimension_totals().size(); ++index) {
    const DimensionTotals& totals = snapshot.dimension_totals()[index];
    const std::string prefix = "totals." + std::to_string(index);
    derived.field(key_with(prefix, "dimension"), totals.dimension);
    derived.field(key_with(prefix, "unit"), totals.unit);
    derived.field(key_with(prefix, "installed"), totals.installed);
    derived.field(key_with(prefix, "observed"), totals.observed);
    derived.field(key_with(prefix, "usable"), totals.usable);
    derived.field(key_with(prefix, "protected"), totals.protected_capacity);
    derived.field(key_with(prefix, "reserved"), totals.reserved);
    derived.field(key_with(prefix, "unavailable"), totals.unavailable);
    derived.field(key_with(prefix, "residual"), totals.residual);
    derived.field(key_with(prefix, "allocatable"), totals.allocatable);
    derived.field(key_with(prefix, "contributing_sources"),
                  static_cast<std::uint64_t>(totals.contributing_sources));
    derived.field(key_with(prefix, "measured_usable_sources"),
                  static_cast<std::uint64_t>(totals.measured_usable_sources));
    derived.field(key_with(prefix, "allocation_closed"), totals.allocation_closed);
    derived.field(key_with(prefix, "physical_closed"), totals.physical_closed);
    derived.field(key_with(prefix, "outcome"), snapshot.outcome(totals.dimension));
  }

  derived.field("limit_count", static_cast<std::uint64_t>(snapshot.limits().size()));
  for (std::size_t index = 0; index < snapshot.limits().size(); ++index) {
    const DimensionLimit& limit = snapshot.limits()[index];
    const std::string prefix = "limit." + std::to_string(index);
    derived.field(key_with(prefix, "dimension"), limit.dimension);
    derived.field(key_with(prefix, "unit"), limit.unit);
    derived.field(key_with(prefix, "installed"), limit.installed);
    derived.field(key_with(prefix, "usable"), limit.usable);
    derived.field(key_with(prefix, "allocatable"), limit.allocatable);
    derived.field(key_with(prefix, "reason"), limit.reason);
    derived.field(key_with(prefix, "source"), limit.source.value());
    derived.field(key_with(prefix, "amount"), limit.amount);
  }

  derived.field("source_stamp_count", static_cast<std::uint64_t>(snapshot.sources().size()));
  for (std::size_t index = 0; index < snapshot.sources().size(); ++index) {
    const SourceGenerationStamp& stamp = snapshot.sources()[index];
    const std::string prefix = "source_stamp." + std::to_string(index);
    derived.field(key_with(prefix, "source"), stamp.source.value());
    derived.field(key_with(prefix, "dimension"), stamp.dimension);
    derived.field(key_with(prefix, "generation"), stamp.generation.value());
    derived.field(key_with(prefix, "evidence_digest"), stamp.evidence_digest);
    derived.field(key_with(prefix, "source_digest"), stamp.source_evidence_digest);
    derived.field(key_with(prefix, "source_revision"), stamp.source_revision);
    derived.field(key_with(prefix, "epoch"), stamp.epoch.value());
    derived.field(key_with(prefix, "incarnation"), stamp.incarnation.value());
    derived.field(key_with(prefix, "observed_at"), stamp.observed_at.value());
    derived.field(key_with(prefix, "state"), stamp.state);
    derived.field(key_with(prefix, "itemised_reserves"), static_cast<std::uint64_t>(stamp.itemised_reserves));
    derived.field(key_with(prefix, "used_declared_rollup"), stamp.used_declared_rollup);
  }

  derived.field("reserve_stamp_count", static_cast<std::uint64_t>(snapshot.reserves().size()));
  for (std::size_t index = 0; index < snapshot.reserves().size(); ++index) {
    const ReserveStamp& stamp = snapshot.reserves()[index];
    const std::string prefix = "reserve_stamp." + std::to_string(index);
    derived.field(key_with(prefix, "id"), stamp.id.value());
    derived.field(key_with(prefix, "source"), stamp.source.value());
    derived.field(key_with(prefix, "dimension"), stamp.dimension);
    derived.field(key_with(prefix, "kind"), stamp.kind);
    derived.field(key_with(prefix, "unit"), stamp.unit);
    derived.field(key_with(prefix, "amount"), stamp.amount);
    derived.field(key_with(prefix, "window_start"), stamp.window.start().value());
    derived.field(key_with(prefix, "window_end"), stamp.window.end().value());
    derived.field(key_with(prefix, "owner"), stamp.owner.value());
    derived.field(key_with(prefix, "service_class"), stamp.service_class.value());
    derived.field(key_with(prefix, "reason"), stamp.reason);
    derived.field(key_with(prefix, "reserve_digest"), stamp.reserve_digest);
  }

  derived.field("constraint_eval_count", static_cast<std::uint64_t>(snapshot.constraints().size()));
  for (std::size_t index = 0; index < snapshot.constraints().size(); ++index) {
    const ConstraintEvaluation& evaluation = snapshot.constraints()[index];
    const std::string prefix = "constraint_eval." + std::to_string(index);
    derived.field(key_with(prefix, "id"), evaluation.id.value());
    derived.field(key_with(prefix, "dimension"), evaluation.dimension);
    derived.field(key_with(prefix, "service_class"), evaluation.service_class.value());
    derived.field(key_with(prefix, "unit"), evaluation.unit);
    derived.field(key_with(prefix, "status"), evaluation.status);
    derived.field(key_with(prefix, "floor"), evaluation.floor_value);
    derived.field(key_with(prefix, "allocatable"), evaluation.allocatable);
    derived.field(key_with(prefix, "reason"), evaluation.reason);
    derived.field(key_with(prefix, "detail"), evaluation.detail);
  }

  encode_notes(derived, "note", snapshot.notes());

  derived.field("limiting_present", snapshot.limiting().present);
  derived.field("limiting_dimension", snapshot.limiting().dimension);
  derived.field("limiting_unit", snapshot.limiting().unit);
  derived.field("limiting_headroom_permille", snapshot.limiting().headroom_permille);
  derived.field("limiting_allocatable", snapshot.limiting().allocatable);
  derived.field("limiting_usable", snapshot.limiting().usable);
  derived.field("limiting_reason", snapshot.limiting().reason);
  derived.field("limiting_source", snapshot.limiting().source.value());

  writer.field("derived_digest", derived.digest_hex());
}

Result<CapacitySnapshotInput> decode_snapshot_fields(const CanonicalReader& reader) {
  CapacitySnapshotInput input;

  const Result<FacilityId> facility = take_optional_id<FacilityId>(reader, "facility");
  if (!facility.has_value()) {
    return facility.error();
  }
  input.facility = facility.value();
  const Result<SiteId> site = take_optional_id<SiteId>(reader, "site");
  if (!site.has_value()) {
    return site.error();
  }
  input.site = site.value();
  const Result<std::uint64_t> generation = reader.take_u64("generation");
  if (!generation.has_value()) {
    return generation.error();
  }
  input.generation = CapacityGeneration::from_value(generation.value());
  const Result<std::uint64_t> epoch = reader.take_u64("epoch");
  if (!epoch.has_value()) {
    return epoch.error();
  }
  input.epoch = EpochId::from_value(epoch.value());
  const Result<std::uint64_t> incarnation = reader.take_u64("incarnation");
  if (!incarnation.has_value()) {
    return incarnation.error();
  }
  input.incarnation = IncarnationId::from_value(incarnation.value());
  const Result<std::uint64_t> revision = reader.take_u64("revision");
  if (!revision.has_value()) {
    return revision.error();
  }
  input.revision = Revision::from_value(revision.value());
  const Result<std::uint64_t> built_at = reader.take_u64("built_at");
  if (!built_at.has_value()) {
    return built_at.error();
  }
  input.built_at = Tick::from_value(built_at.value());
  const Result<std::uint64_t> valid_until = reader.take_u64("valid_until");
  if (!valid_until.has_value()) {
    return valid_until.error();
  }
  input.valid_until = Tick::from_value(valid_until.value());
  if (!(input.built_at <= input.valid_until)) {
    return corruption("the snapshot validity window ends before it begins", "valid_until");
  }

  Result<std::vector<CapacityRequirement>> requirements = decode_requirements(reader);
  if (!requirements.has_value()) {
    return requirements.error();
  }
  input.requirements = std::move(requirements.value());

  const Result<std::uint64_t> evidence_count = reader.take_u64("evidence_count");
  if (!evidence_count.has_value()) {
    return evidence_count.error();
  }
  if (evidence_count.value() > limits::max_evidence_sources * capacity_dimension_count) {
    return corruption("too many evidence records", "evidence_count");
  }
  input.evidence.reserve(static_cast<std::size_t>(evidence_count.value()));
  for (std::uint64_t index = 0; index < evidence_count.value(); ++index) {
    const Result<SourceEvidence> record = decode_evidence(reader, index_prefix("evidence", index));
    if (!record.has_value()) {
      return record.error();
    }
    input.evidence.push_back(record.value());
  }

  const Result<std::uint64_t> reserve_count = reader.take_u64("reserve_count");
  if (!reserve_count.has_value()) {
    return reserve_count.error();
  }
  if (reserve_count.value() > limits::max_reserves) {
    return corruption("too many declared reserves", "reserve_count");
  }
  input.reserves.reserve(static_cast<std::size_t>(reserve_count.value()));
  for (std::uint64_t index = 0; index < reserve_count.value(); ++index) {
    const Result<CapacityReserve> reserve = decode_reserve(reader, index_prefix("reserve", index));
    if (!reserve.has_value()) {
      return reserve.error();
    }
    input.reserves.push_back(reserve.value());
  }

  const Result<std::uint64_t> constraint_count = reader.take_u64("constraint_count");
  if (!constraint_count.has_value()) {
    return constraint_count.error();
  }
  if (constraint_count.value() > limits::max_service_constraints) {
    return corruption("too many service constraints", "constraint_count");
  }
  input.constraints.reserve(static_cast<std::size_t>(constraint_count.value()));
  for (std::uint64_t index = 0; index < constraint_count.value(); ++index) {
    const Result<ServiceConstraint> constraint =
        decode_constraint(reader, index_prefix("constraint", index));
    if (!constraint.has_value()) {
      return constraint.error();
    }
    input.constraints.push_back(constraint.value());
  }

  const Result<NoteSet> coverage = decode_notes(reader, "coverage_note");
  if (!coverage.has_value()) {
    return coverage.error();
  }
  input.coverage_findings = coverage.value();

  return input;
}

void encode_model_state_fields(CanonicalWriter& writer, const ModelState& state) {
  writer.field("facility", state.facility.value());
  writer.field("site", state.site.value());
  writer.field("epoch", state.epoch.value());
  writer.field("incarnation", state.incarnation.value());
  writer.field("capacity_generation", state.capacity_generation.value());
  writer.field("revision", state.revision.value());
  writer.field("snapshot_validity_ticks", state.snapshot_validity_ticks.value());
  writer.field("max_sources", static_cast<std::uint64_t>(state.max_sources));
  writer.field("max_reserves", static_cast<std::uint64_t>(state.max_reserves));
  writer.field("max_constraints", static_cast<std::uint64_t>(state.max_constraints));

  encode_requirements(writer, state.requirements);

  writer.field("evidence_count", static_cast<std::uint64_t>(state.evidence.size()));
  for (std::size_t index = 0; index < state.evidence.size(); ++index) {
    encode_evidence(writer, "evidence." + std::to_string(index), state.evidence[index], true);
  }

  writer.field("reserve_count", static_cast<std::uint64_t>(state.reserves.size()));
  for (std::size_t index = 0; index < state.reserves.size(); ++index) {
    encode_reserve(writer, "reserve." + std::to_string(index), state.reserves[index], true);
  }

  writer.field("constraint_count", static_cast<std::uint64_t>(state.constraints.size()));
  for (std::size_t index = 0; index < state.constraints.size(); ++index) {
    encode_constraint(writer, "constraint." + std::to_string(index), state.constraints[index], true);
  }

  writer.field("attempt_count", static_cast<std::uint64_t>(state.attempts.size()));
  for (std::size_t index = 0; index < state.attempts.size(); ++index) {
    writer.field("attempt." + std::to_string(index), state.attempts[index].value());
  }

  writer.field("published_snapshot_id", state.published_snapshot_id);
  writer.field("published_snapshot_digest", state.published_snapshot_digest);
}

Result<ModelState> decode_model_state_fields(const CanonicalReader& reader) {
  ModelState state;

  const Result<FacilityId> facility = take_optional_id<FacilityId>(reader, "facility");
  if (!facility.has_value()) {
    return facility.error();
  }
  state.facility = facility.value();
  const Result<SiteId> site = take_optional_id<SiteId>(reader, "site");
  if (!site.has_value()) {
    return site.error();
  }
  state.site = site.value();
  const Result<std::uint64_t> epoch = reader.take_u64("epoch");
  if (!epoch.has_value()) {
    return epoch.error();
  }
  state.epoch = EpochId::from_value(epoch.value());
  const Result<std::uint64_t> incarnation = reader.take_u64("incarnation");
  if (!incarnation.has_value()) {
    return incarnation.error();
  }
  state.incarnation = IncarnationId::from_value(incarnation.value());
  const Result<std::uint64_t> generation = reader.take_u64("capacity_generation");
  if (!generation.has_value()) {
    return generation.error();
  }
  state.capacity_generation = CapacityGeneration::from_value(generation.value());
  const Result<std::uint64_t> revision = reader.take_u64("revision");
  if (!revision.has_value()) {
    return revision.error();
  }
  state.revision = Revision::from_value(revision.value());
  const Result<std::uint64_t> validity = reader.take_u64("snapshot_validity_ticks");
  if (!validity.has_value()) {
    return validity.error();
  }
  state.snapshot_validity_ticks = Tick::from_value(validity.value());

  struct BoundField {
    const char* name;
    std::size_t* target;
    std::size_t maximum;
  };
  const BoundField bound_fields[] = {
      {"max_sources", &state.max_sources, limits::max_evidence_sources},
      {"max_reserves", &state.max_reserves, limits::max_reserves},
      {"max_constraints", &state.max_constraints, limits::max_service_constraints},
  };
  for (const BoundField& field : bound_fields) {
    const Result<std::uint64_t> value = reader.take_u64(field.name);
    if (!value.has_value()) {
      return value.error();
    }
    if (value.value() == 0 || value.value() > field.maximum) {
      return corruption("a configuration bound is outside the accepted range", field.name);
    }
    *field.target = static_cast<std::size_t>(value.value());
  }

  Result<std::vector<CapacityRequirement>> requirements = decode_requirements(reader);
  if (!requirements.has_value()) {
    return requirements.error();
  }
  state.requirements = std::move(requirements.value());

  const Result<std::uint64_t> evidence_count = reader.take_u64("evidence_count");
  if (!evidence_count.has_value()) {
    return evidence_count.error();
  }
  if (evidence_count.value() > limits::max_evidence_sources * capacity_dimension_count) {
    return corruption("too many evidence records", "evidence_count");
  }
  state.evidence.reserve(static_cast<std::size_t>(evidence_count.value()));
  for (std::uint64_t index = 0; index < evidence_count.value(); ++index) {
    const Result<SourceEvidence> record = decode_evidence(reader, index_prefix("evidence", index));
    if (!record.has_value()) {
      return record.error();
    }
    state.evidence.push_back(record.value());
  }

  const Result<std::uint64_t> reserve_count = reader.take_u64("reserve_count");
  if (!reserve_count.has_value()) {
    return reserve_count.error();
  }
  if (reserve_count.value() > limits::max_reserves) {
    return corruption("too many declared reserves", "reserve_count");
  }
  state.reserves.reserve(static_cast<std::size_t>(reserve_count.value()));
  for (std::uint64_t index = 0; index < reserve_count.value(); ++index) {
    const Result<CapacityReserve> reserve = decode_reserve(reader, index_prefix("reserve", index));
    if (!reserve.has_value()) {
      return reserve.error();
    }
    state.reserves.push_back(reserve.value());
  }

  const Result<std::uint64_t> constraint_count = reader.take_u64("constraint_count");
  if (!constraint_count.has_value()) {
    return constraint_count.error();
  }
  if (constraint_count.value() > limits::max_service_constraints) {
    return corruption("too many service constraints", "constraint_count");
  }
  state.constraints.reserve(static_cast<std::size_t>(constraint_count.value()));
  for (std::uint64_t index = 0; index < constraint_count.value(); ++index) {
    const Result<ServiceConstraint> constraint =
        decode_constraint(reader, index_prefix("constraint", index));
    if (!constraint.has_value()) {
      return constraint.error();
    }
    state.constraints.push_back(constraint.value());
  }

  const Result<std::uint64_t> attempt_count = reader.take_u64("attempt_count");
  if (!attempt_count.has_value()) {
    return attempt_count.error();
  }
  if (attempt_count.value() > limits::max_recorded_attempts) {
    return corruption("too many recorded attempts", "attempt_count");
  }
  state.attempts.reserve(static_cast<std::size_t>(attempt_count.value()));
  for (std::uint64_t index = 0; index < attempt_count.value(); ++index) {
    const Result<std::uint64_t> attempt = reader.take_u64("attempt." + std::to_string(index));
    if (!attempt.has_value()) {
      return attempt.error();
    }
    state.attempts.push_back(AttemptId::from_value(attempt.value()));
  }

  const Result<std::string> published_id = reader.take_string("published_snapshot_id");
  if (!published_id.has_value()) {
    return published_id.error();
  }
  state.published_snapshot_id = published_id.value();
  const Result<std::string> published_digest = reader.take_string("published_snapshot_digest");
  if (!published_digest.has_value()) {
    return published_digest.error();
  }
  state.published_snapshot_digest = published_digest.value();

  if (state.facility.empty() || state.site.empty()) {
    return corruption("the model state does not name a facility and a site", "facility");
  }
  if (!state.published_snapshot_id.empty() || !state.published_snapshot_digest.empty()) {
    if (state.published_snapshot_id.empty() || state.published_snapshot_digest.empty()) {
      return corruption("the published snapshot handle is incomplete", "published_snapshot_id");
    }
    const Result<SnapshotId> id = SnapshotId::parse(state.published_snapshot_id);
    if (!id.has_value()) {
      return id.error();
    }
    if (!is_hex_digest(state.published_snapshot_digest)) {
      return corruption("the published snapshot digest is not a SHA-256 hex value",
                        "published_snapshot_digest");
    }
  }
  return state;
}

std::string ModelState::to_string() const {
  std::string out = facility.value();
  out.push_back('/');
  out.append(site.value());
  out.append(" generation=");
  out.append(capacity_generation.to_string());
  out.append(" revision=");
  out.append(revision.to_string());
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(" evidence=");
  out.append(std::to_string(evidence.size()));
  out.append(" reserves=");
  out.append(std::to_string(reserves.size()));
  out.append(" constraints=");
  out.append(std::to_string(constraints.size()));
  out.append(" published=");
  out.append(published_snapshot_id.empty() ? std::string("-") : published_snapshot_id);
  return out;
}

}  // namespace dccp::facility_capacity::internal
