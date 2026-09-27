// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "internal/codec.hpp"

#include <string>
#include <utility>

#include "dccp/facility_capacity/limits.hpp"
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

template <class Id>
Result<Id> take_id(const CanonicalReader& reader, std::string_view prefix, std::string_view suffix) {
  const std::string key = key_with(prefix, suffix);
  const Result<std::string_view> raw = reader.take(key);
  if (!raw.has_value()) {
    return raw.error();
  }
  return Id::parse(raw.value());
}

template <class Id>
Result<Id> take_optional_id(const CanonicalReader& reader, std::string_view prefix, std::string_view suffix) {
  const std::string key = key_with(prefix, suffix);
  const Result<std::string_view> raw = reader.take(key);
  if (!raw.has_value()) {
    return raw.error();
  }
  if (raw.value().empty()) {
    return Id();
  }
  return Id::parse(raw.value());
}

void encode_provenance(CanonicalWriter& writer, std::string_view prefix, const Provenance& provenance) {
  writer.field(key_with(prefix, "prov_source"), provenance.source.value());
  writer.field(key_with(prefix, "prov_revision"), provenance.revision);
  writer.field(key_with(prefix, "prov_produced_at"), provenance.produced_at.value());
  writer.field(key_with(prefix, "prov_epoch"), provenance.epoch.value());
  writer.field(key_with(prefix, "prov_incarnation"), provenance.incarnation.value());
  writer.field(key_with(prefix, "prov_digest"), provenance.evidence_digest);
}

Result<Provenance> decode_provenance(const CanonicalReader& reader, std::string_view prefix) {
  Provenance provenance;
  const Result<CapacitySourceId> source = take_id<CapacitySourceId>(reader, prefix, "prov_source");
  if (!source.has_value()) {
    return source.error();
  }
  provenance.source = source.value();
  const Result<std::string> revision = reader.take_string(key_with(prefix, "prov_revision"));
  if (!revision.has_value()) {
    return revision.error();
  }
  provenance.revision = revision.value();
  const Result<std::uint64_t> produced_at = reader.take_u64(key_with(prefix, "prov_produced_at"));
  if (!produced_at.has_value()) {
    return produced_at.error();
  }
  provenance.produced_at = Tick::from_value(produced_at.value());
  const Result<std::uint64_t> epoch = reader.take_u64(key_with(prefix, "prov_epoch"));
  if (!epoch.has_value()) {
    return epoch.error();
  }
  provenance.epoch = EpochId::from_value(epoch.value());
  const Result<std::uint64_t> incarnation = reader.take_u64(key_with(prefix, "prov_incarnation"));
  if (!incarnation.has_value()) {
    return incarnation.error();
  }
  provenance.incarnation = IncarnationId::from_value(incarnation.value());
  const Result<std::string> digest = reader.take_string(key_with(prefix, "prov_digest"));
  if (!digest.has_value()) {
    return digest.error();
  }
  provenance.evidence_digest = digest.value();
  return provenance;
}

}  // namespace

void encode_evidence(CanonicalWriter& writer, std::string_view prefix, const SourceEvidence& evidence,
                     bool include_digest) {
  writer.field(key_with(prefix, "source"), evidence.source().value());
  writer.field(key_with(prefix, "dimension"), evidence.dimension());
  writer.field(key_with(prefix, "unit"), evidence.unit());
  writer.field(key_with(prefix, "generation"), evidence.generation().value());
  writer.field(key_with(prefix, "observed_at"), evidence.observed_at().value());
  writer.field(key_with(prefix, "state"), evidence.state());
  writer.field(key_with(prefix, "state_reason"), evidence.state_reason());
  writer.field(key_with(prefix, "state_detail"), evidence.state_detail());
  writer.field(key_with(prefix, "installed"), evidence.installed());
  writer.field(key_with(prefix, "observed"), evidence.observed());
  writer.field(key_with(prefix, "usable"), evidence.usable());
  writer.field(key_with(prefix, "unavailable"), evidence.unavailable());
  writer.field(key_with(prefix, "residual"), evidence.residual());
  writer.field(key_with(prefix, "protected"), evidence.protected_capacity());
  writer.field(key_with(prefix, "reserved"), evidence.reserved());
  writer.field(key_with(prefix, "closure_proven"), evidence.closure_proven());
  encode_provenance(writer, prefix, evidence.provenance());
  if (include_digest) {
    writer.field(key_with(prefix, "digest"), evidence.digest());
  }
}

std::string evidence_digest(const SourceEvidence& evidence) {
  CanonicalWriter writer;
  writer.field("document", "source-evidence");
  encode_evidence(writer, "e", evidence, false);
  return writer.digest_hex();
}

Result<SourceEvidence> decode_evidence(const CanonicalReader& reader, std::string_view prefix) {
  SourceEvidenceFields fields;
  const Result<CapacitySourceId> source = take_id<CapacitySourceId>(reader, prefix, "source");
  if (!source.has_value()) {
    return source.error();
  }
  fields.source = source.value();
  const Result<CapacityDimension> dimension = reader.take_dimension(key_with(prefix, "dimension"));
  if (!dimension.has_value()) {
    return dimension.error();
  }
  fields.dimension = dimension.value();
  const Result<Unit> unit = reader.take_unit(key_with(prefix, "unit"));
  if (!unit.has_value()) {
    return unit.error();
  }
  fields.unit = unit.value();
  const Result<std::uint64_t> generation = reader.take_u64(key_with(prefix, "generation"));
  if (!generation.has_value()) {
    return generation.error();
  }
  fields.generation = EvidenceGeneration::from_value(generation.value());
  const Result<std::uint64_t> observed_at = reader.take_u64(key_with(prefix, "observed_at"));
  if (!observed_at.has_value()) {
    return observed_at.error();
  }
  fields.observed_at = Tick::from_value(observed_at.value());
  const Result<OperationalState> state = reader.take_state(key_with(prefix, "state"));
  if (!state.has_value()) {
    return state.error();
  }
  fields.state = state.value();
  const Result<ReasonCode> state_reason = reader.take_reason(key_with(prefix, "state_reason"));
  if (!state_reason.has_value()) {
    return state_reason.error();
  }
  fields.state_reason = state_reason.value();
  const Result<std::string> state_detail = reader.take_string(key_with(prefix, "state_detail"));
  if (!state_detail.has_value()) {
    return state_detail.error();
  }
  fields.state_detail = state_detail.value();

  const Result<Measured> installed = reader.take_measured(key_with(prefix, "installed"), fields.unit);
  if (!installed.has_value()) {
    return installed.error();
  }
  fields.installed = installed.value();
  const Result<Measured> observed = reader.take_measured(key_with(prefix, "observed"), fields.unit);
  if (!observed.has_value()) {
    return observed.error();
  }
  fields.observed = observed.value();
  const Result<Measured> usable = reader.take_measured(key_with(prefix, "usable"), fields.unit);
  if (!usable.has_value()) {
    return usable.error();
  }
  fields.usable = usable.value();
  const Result<Measured> unavailable = reader.take_measured(key_with(prefix, "unavailable"), fields.unit);
  if (!unavailable.has_value()) {
    return unavailable.error();
  }
  fields.unavailable = unavailable.value();
  const Result<Measured> residual = reader.take_measured(key_with(prefix, "residual"), fields.unit);
  if (!residual.has_value()) {
    return residual.error();
  }
  fields.residual = residual.value();
  const Result<Measured> protected_capacity = reader.take_measured(key_with(prefix, "protected"), fields.unit);
  if (!protected_capacity.has_value()) {
    return protected_capacity.error();
  }
  fields.protected_capacity = protected_capacity.value();
  const Result<Measured> reserved = reader.take_measured(key_with(prefix, "reserved"), fields.unit);
  if (!reserved.has_value()) {
    return reserved.error();
  }
  fields.reserved = reserved.value();
  fields.derive_residual = false;

  const Result<bool> stored_closure = reader.take_bool(key_with(prefix, "closure_proven"));
  if (!stored_closure.has_value()) {
    return stored_closure.error();
  }
  const Result<Provenance> provenance = decode_provenance(reader, prefix);
  if (!provenance.has_value()) {
    return provenance.error();
  }
  fields.provenance = provenance.value();

  const Result<std::string> stored_digest = reader.take_string(key_with(prefix, "digest"));
  if (!stored_digest.has_value()) {
    return stored_digest.error();
  }

  Result<SourceEvidence> evidence = SourceEvidence::create(std::move(fields));
  if (!evidence.has_value()) {
    return evidence.error();
  }
  if (evidence.value().closure_proven() != stored_closure.value()) {
    return corruption("the stored evidence closure flag does not reproduce", key_with(prefix, "closure_proven"));
  }
  if (evidence.value().digest() != stored_digest.value()) {
    Error error = Error::make(ErrorCode::checksum_mismatch, "the stored evidence digest does not reproduce");
    error.with_constraint(key_with(prefix, "digest"));
    return error;
  }
  return evidence;
}

void encode_reserve(CanonicalWriter& writer, std::string_view prefix, const CapacityReserve& reserve,
                    bool include_digest) {
  writer.field(key_with(prefix, "id"), reserve.id().value());
  writer.field(key_with(prefix, "source"), reserve.source().value());
  writer.field(key_with(prefix, "dimension"), reserve.dimension());
  writer.field(key_with(prefix, "unit"), reserve.unit());
  writer.field(key_with(prefix, "kind"), reserve.kind());
  writer.field(key_with(prefix, "amount"), reserve.amount());
  writer.field(key_with(prefix, "window_start"), reserve.window().start().value());
  writer.field(key_with(prefix, "window_end"), reserve.window().end().value());
  writer.field(key_with(prefix, "owner"), reserve.owner().value());
  writer.field(key_with(prefix, "service_class"), reserve.service_class().value());
  writer.field(key_with(prefix, "reason"), reserve.reason());
  writer.field(key_with(prefix, "detail"), reserve.detail());
  encode_provenance(writer, prefix, reserve.provenance());
  if (include_digest) {
    writer.field(key_with(prefix, "digest"), reserve.digest());
  }
}

std::string reserve_digest(const CapacityReserve& reserve) {
  CanonicalWriter writer;
  writer.field("document", "capacity-reserve");
  encode_reserve(writer, "r", reserve, false);
  return writer.digest_hex();
}

Result<CapacityReserve> decode_reserve(const CanonicalReader& reader, std::string_view prefix) {
  CapacityReserveFields fields;
  const Result<ReserveId> id = take_id<ReserveId>(reader, prefix, "id");
  if (!id.has_value()) {
    return id.error();
  }
  fields.id = id.value();
  const Result<CapacitySourceId> source = take_id<CapacitySourceId>(reader, prefix, "source");
  if (!source.has_value()) {
    return source.error();
  }
  fields.source = source.value();
  const Result<CapacityDimension> dimension = reader.take_dimension(key_with(prefix, "dimension"));
  if (!dimension.has_value()) {
    return dimension.error();
  }
  fields.dimension = dimension.value();
  const Result<Unit> unit = reader.take_unit(key_with(prefix, "unit"));
  if (!unit.has_value()) {
    return unit.error();
  }
  fields.unit = unit.value();
  const Result<ReserveKind> kind = reader.take_reserve_kind(key_with(prefix, "kind"));
  if (!kind.has_value()) {
    return kind.error();
  }
  fields.kind = kind.value();
  const Result<Measured> amount = reader.take_measured(key_with(prefix, "amount"), fields.unit);
  if (!amount.has_value()) {
    return amount.error();
  }
  fields.amount = amount.value();
  const Result<std::uint64_t> window_start = reader.take_u64(key_with(prefix, "window_start"));
  if (!window_start.has_value()) {
    return window_start.error();
  }
  const Result<std::uint64_t> window_end = reader.take_u64(key_with(prefix, "window_end"));
  if (!window_end.has_value()) {
    return window_end.error();
  }
  const Result<ValidityWindow> window =
      ValidityWindow::create(Tick::from_value(window_start.value()), Tick::from_value(window_end.value()));
  if (!window.has_value()) {
    return window.error();
  }
  fields.window = window.value();
  const Result<OwnerId> owner = take_optional_id<OwnerId>(reader, prefix, "owner");
  if (!owner.has_value()) {
    return owner.error();
  }
  fields.owner = owner.value();
  const Result<ServiceClassId> service_class = take_optional_id<ServiceClassId>(reader, prefix, "service_class");
  if (!service_class.has_value()) {
    return service_class.error();
  }
  fields.service_class = service_class.value();
  const Result<ReasonCode> reason = reader.take_reason(key_with(prefix, "reason"));
  if (!reason.has_value()) {
    return reason.error();
  }
  fields.reason = reason.value();
  const Result<std::string> detail = reader.take_string(key_with(prefix, "detail"));
  if (!detail.has_value()) {
    return detail.error();
  }
  fields.detail = detail.value();
  const Result<Provenance> provenance = decode_provenance(reader, prefix);
  if (!provenance.has_value()) {
    return provenance.error();
  }
  fields.provenance = provenance.value();

  const Result<std::string> stored_digest = reader.take_string(key_with(prefix, "digest"));
  if (!stored_digest.has_value()) {
    return stored_digest.error();
  }
  Result<CapacityReserve> reserve = CapacityReserve::create(std::move(fields));
  if (!reserve.has_value()) {
    return reserve.error();
  }
  if (reserve.value().digest() != stored_digest.value()) {
    Error error = Error::make(ErrorCode::checksum_mismatch, "the stored reserve digest does not reproduce");
    error.with_constraint(key_with(prefix, "digest"));
    return error;
  }
  return reserve;
}

void encode_constraint(CanonicalWriter& writer, std::string_view prefix, const ServiceConstraint& constraint,
                       bool include_digest) {
  writer.field(key_with(prefix, "id"), constraint.id().value());
  writer.field(key_with(prefix, "dimension"), constraint.dimension());
  writer.field(key_with(prefix, "unit"), constraint.unit());
  writer.field(key_with(prefix, "service_class"), constraint.service_class().value());
  writer.field(key_with(prefix, "floor"), constraint.floor_value());
  writer.field(key_with(prefix, "window_start"), constraint.window().start().value());
  writer.field(key_with(prefix, "window_end"), constraint.window().end().value());
  writer.field(key_with(prefix, "owner"), constraint.owner().value());
  writer.field(key_with(prefix, "reason"), constraint.reason());
  writer.field(key_with(prefix, "detail"), constraint.detail());
  encode_provenance(writer, prefix, constraint.provenance());
  if (include_digest) {
    writer.field(key_with(prefix, "digest"), constraint.digest());
  }
}

std::string constraint_digest(const ServiceConstraint& constraint) {
  CanonicalWriter writer;
  writer.field("document", "service-constraint");
  encode_constraint(writer, "c", constraint, false);
  return writer.digest_hex();
}

Result<ServiceConstraint> decode_constraint(const CanonicalReader& reader, std::string_view prefix) {
  ServiceConstraintFields fields;
  const Result<ConstraintId> id = take_id<ConstraintId>(reader, prefix, "id");
  if (!id.has_value()) {
    return id.error();
  }
  fields.id = id.value();
  const Result<CapacityDimension> dimension = reader.take_dimension(key_with(prefix, "dimension"));
  if (!dimension.has_value()) {
    return dimension.error();
  }
  fields.dimension = dimension.value();
  const Result<Unit> unit = reader.take_unit(key_with(prefix, "unit"));
  if (!unit.has_value()) {
    return unit.error();
  }
  fields.unit = unit.value();
  const Result<ServiceClassId> service_class = take_optional_id<ServiceClassId>(reader, prefix, "service_class");
  if (!service_class.has_value()) {
    return service_class.error();
  }
  fields.service_class = service_class.value();
  const Result<Measured> floor_value = reader.take_measured(key_with(prefix, "floor"), fields.unit);
  if (!floor_value.has_value()) {
    return floor_value.error();
  }
  fields.floor = floor_value.value();
  const Result<std::uint64_t> window_start = reader.take_u64(key_with(prefix, "window_start"));
  if (!window_start.has_value()) {
    return window_start.error();
  }
  const Result<std::uint64_t> window_end = reader.take_u64(key_with(prefix, "window_end"));
  if (!window_end.has_value()) {
    return window_end.error();
  }
  const Result<ValidityWindow> window =
      ValidityWindow::create(Tick::from_value(window_start.value()), Tick::from_value(window_end.value()));
  if (!window.has_value()) {
    return window.error();
  }
  fields.window = window.value();
  const Result<OwnerId> owner = take_optional_id<OwnerId>(reader, prefix, "owner");
  if (!owner.has_value()) {
    return owner.error();
  }
  fields.owner = owner.value();
  const Result<ReasonCode> reason = reader.take_reason(key_with(prefix, "reason"));
  if (!reason.has_value()) {
    return reason.error();
  }
  fields.reason = reason.value();
  const Result<std::string> detail = reader.take_string(key_with(prefix, "detail"));
  if (!detail.has_value()) {
    return detail.error();
  }
  fields.detail = detail.value();
  const Result<Provenance> provenance = decode_provenance(reader, prefix);
  if (!provenance.has_value()) {
    return provenance.error();
  }
  fields.provenance = provenance.value();

  const Result<std::string> stored_digest = reader.take_string(key_with(prefix, "digest"));
  if (!stored_digest.has_value()) {
    return stored_digest.error();
  }
  Result<ServiceConstraint> constraint = ServiceConstraint::create(std::move(fields));
  if (!constraint.has_value()) {
    return constraint.error();
  }
  if (constraint.value().digest() != stored_digest.value()) {
    Error error = Error::make(ErrorCode::checksum_mismatch, "the stored constraint digest does not reproduce");
    error.with_constraint(key_with(prefix, "digest"));
    return error;
  }
  return constraint;
}

void encode_requirement(CanonicalWriter& writer, std::string_view prefix, const CapacityRequirement& requirement) {
  writer.field(key_with(prefix, "dimension"), requirement.dimension);
  writer.field(key_with(prefix, "required"), requirement.required);
  writer.field(key_with(prefix, "source_count"), static_cast<std::uint64_t>(requirement.required_sources.size()));
  for (std::size_t index = 0; index < requirement.required_sources.size(); ++index) {
    writer.field(key_with(prefix, "source." + std::to_string(index)), requirement.required_sources[index].value());
  }
}

Result<CapacityRequirement> decode_requirement(const CanonicalReader& reader, std::string_view prefix) {
  CapacityRequirement requirement;
  const Result<CapacityDimension> dimension = reader.take_dimension(key_with(prefix, "dimension"));
  if (!dimension.has_value()) {
    return dimension.error();
  }
  requirement.dimension = dimension.value();
  const Result<bool> required = reader.take_bool(key_with(prefix, "required"));
  if (!required.has_value()) {
    return required.error();
  }
  requirement.required = required.value();
  const Result<std::uint64_t> count = reader.take_u64(key_with(prefix, "source_count"));
  if (!count.has_value()) {
    return count.error();
  }
  if (count.value() > limits::max_evidence_sources) {
    Error error = Error::make(ErrorCode::limit_exceeded, "the requirement declares too many sources");
    error.with_constraint(key_with(prefix, "source_count"));
    return error;
  }
  requirement.required_sources.reserve(static_cast<std::size_t>(count.value()));
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    const Result<CapacitySourceId> source =
        take_id<CapacitySourceId>(reader, prefix, "source." + std::to_string(index));
    if (!source.has_value()) {
      return source.error();
    }
    requirement.required_sources.push_back(source.value());
  }
  const Status normalized = requirement.normalize();
  if (!normalized.has_value()) {
    return normalized.error();
  }
  return requirement;
}

void encode_note(CanonicalWriter& writer, std::string_view prefix, const CapacityNote& note) {
  writer.field(key_with(prefix, "code"), note.code);
  writer.field(key_with(prefix, "has_dimension"), note.has_dimension);
  writer.field(key_with(prefix, "dimension"), note.dimension);
  writer.field(key_with(prefix, "source"), note.source.value());
  writer.field(key_with(prefix, "detail"), note.detail);
}

Result<CapacityNote> decode_note(const CanonicalReader& reader, std::string_view prefix) {
  CapacityNote note;
  const Result<ReasonCode> code = reader.take_reason(key_with(prefix, "code"));
  if (!code.has_value()) {
    return code.error();
  }
  note.code = code.value();
  const Result<bool> has_dimension = reader.take_bool(key_with(prefix, "has_dimension"));
  if (!has_dimension.has_value()) {
    return has_dimension.error();
  }
  note.has_dimension = has_dimension.value();
  const Result<CapacityDimension> dimension = reader.take_dimension(key_with(prefix, "dimension"));
  if (!dimension.has_value()) {
    return dimension.error();
  }
  note.dimension = dimension.value();
  const Result<CapacitySourceId> source = take_optional_id<CapacitySourceId>(reader, prefix, "source");
  if (!source.has_value()) {
    return source.error();
  }
  note.source = source.value();
  const Result<std::string> detail = reader.take_string(key_with(prefix, "detail"));
  if (!detail.has_value()) {
    return detail.error();
  }
  note.detail = detail.value();
  if (note.detail.size() > limits::max_detail_length) {
    Error error = Error::make(ErrorCode::limit_exceeded, "a note detail exceeds the length bound");
    error.with_constraint(key_with(prefix, "detail"));
    return error;
  }
  return note;
}

void encode_notes(CanonicalWriter& writer, std::string_view prefix, const NoteSet& notes) {
  writer.field(key_with(prefix, "count"), static_cast<std::uint64_t>(notes.size()));
  for (std::size_t index = 0; index < notes.size(); ++index) {
    encode_note(writer, key_with(prefix, std::to_string(index)), notes[index]);
  }
}

Result<NoteSet> decode_notes(const CanonicalReader& reader, std::string_view prefix) {
  const Result<std::uint64_t> count = reader.take_u64(key_with(prefix, "count"));
  if (!count.has_value()) {
    return count.error();
  }
  if (count.value() > limits::max_reasons_per_note) {
    Error error = Error::make(ErrorCode::limit_exceeded, "too many notes in one block");
    error.with_constraint(key_with(prefix, "count"));
    return error;
  }
  NoteSet notes;
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    const Result<CapacityNote> note = decode_note(reader, key_with(prefix, std::to_string(index)));
    if (!note.has_value()) {
      return note.error();
    }
    if (!notes.add(note.value())) {
      return Error::make(ErrorCode::limit_exceeded, "too many notes in one block");
    }
  }
  return notes;
}

void encode_precondition(CanonicalWriter& writer, std::string_view prefix, const CapacityPrecondition& precondition) {
  writer.field(key_with(prefix, "generation"), precondition.expected_capacity_generation.value());
  writer.field(key_with(prefix, "revision"), precondition.expected_revision.value());
  writer.field(key_with(prefix, "epoch"), precondition.expected_epoch.value());
  writer.field(key_with(prefix, "incarnation"), precondition.expected_incarnation.value());
  writer.field(key_with(prefix, "attempt"), precondition.attempt.value());
}

Result<CapacityPrecondition> decode_precondition(const CanonicalReader& reader, std::string_view prefix) {
  CapacityPrecondition precondition;
  const Result<std::uint64_t> generation = reader.take_u64(key_with(prefix, "generation"));
  if (!generation.has_value()) {
    return generation.error();
  }
  precondition.expected_capacity_generation = CapacityGeneration::from_value(generation.value());
  const Result<std::uint64_t> revision = reader.take_u64(key_with(prefix, "revision"));
  if (!revision.has_value()) {
    return revision.error();
  }
  precondition.expected_revision = Revision::from_value(revision.value());
  const Result<std::uint64_t> epoch = reader.take_u64(key_with(prefix, "epoch"));
  if (!epoch.has_value()) {
    return epoch.error();
  }
  precondition.expected_epoch = EpochId::from_value(epoch.value());
  const Result<std::uint64_t> incarnation = reader.take_u64(key_with(prefix, "incarnation"));
  if (!incarnation.has_value()) {
    return incarnation.error();
  }
  precondition.expected_incarnation = IncarnationId::from_value(incarnation.value());
  const Result<std::uint64_t> attempt = reader.take_u64(key_with(prefix, "attempt"));
  if (!attempt.has_value()) {
    return attempt.error();
  }
  precondition.attempt = AttemptId::from_value(attempt.value());
  return precondition;
}

}  // namespace dccp::facility_capacity::internal
