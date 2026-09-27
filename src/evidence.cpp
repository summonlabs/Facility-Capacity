// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/evidence.hpp"

#include <ostream>
#include <utility>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/codec.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

/// Checks that a measured value, when it is measured at all, is expressed in
/// the unit of the record it belongs to.
Status check_unit(const Measured& value, Unit unit, const char* field) {
  if (value.is_known() && value.unit() != unit) {
    Error error = Error::make(ErrorCode::unit_mismatch, "a measured value is not in the dimension's canonical unit");
    error.with_constraint(field);
    return error;
  }
  return Status::success();
}

Status check_detail(std::string_view detail, const char* field) {
  if (detail.size() > limits::max_detail_length) {
    Error error = Error::make(ErrorCode::limit_exceeded, "a detail field exceeds the length bound");
    error.with_constraint(field);
    return error;
  }
  if (!internal::is_ascii_printable(detail)) {
    Error error = Error::make(ErrorCode::invalid_argument, "a detail field contains non-printable characters");
    error.with_constraint(field);
    return error;
  }
  return Status::success();
}

}  // namespace

std::string_view operational_state_name(OperationalState state) noexcept {
  switch (state) {
    case OperationalState::nominal:
      return "nominal";
    case OperationalState::degraded:
      return "degraded";
    case OperationalState::unavailable:
      return "unavailable";
    case OperationalState::unknown:
      return "unknown";
  }
  return "unknown";
}

bool parse_operational_state(std::string_view name, OperationalState& out) noexcept {
  if (name == "nominal") {
    out = OperationalState::nominal;
    return true;
  }
  if (name == "degraded") {
    out = OperationalState::degraded;
    return true;
  }
  if (name == "unavailable") {
    out = OperationalState::unavailable;
    return true;
  }
  if (name == "unknown") {
    out = OperationalState::unknown;
    return true;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, OperationalState state) { return out << operational_state_name(state); }

Result<SourceEvidence> SourceEvidence::create(SourceEvidenceFields fields) {
  if (fields.source.empty()) {
    return Error::make(ErrorCode::invalid_argument, "evidence has no source identity");
  }
  const Unit canonical = canonical_unit(fields.dimension);
  if (fields.unit != canonical) {
    Error error = Error::make(ErrorCode::unit_mismatch, "evidence unit is not the dimension's canonical unit");
    error.with_constraint("unit == canonical_unit(dimension)");
    return error;
  }
  if (fields.generation.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "an evidence generation starts at one, never zero");
  }
  const Status detail_ok = check_detail(fields.state_detail, "state_detail");
  if (!detail_ok.has_value()) {
    return detail_ok.error();
  }
  const Status provenance_ok = fields.provenance.validate();
  if (!provenance_ok.has_value()) {
    return provenance_ok.error();
  }
  if (fields.provenance.source != fields.source) {
    return Error::make(ErrorCode::conflict, "evidence provenance names a different source");
  }

  struct NamedMeasuring {
    const Measured* value;
    const char* field;
  };
  const NamedMeasuring measured_fields[] = {
      {&fields.installed, "installed"},   {&fields.observed, "observed"},
      {&fields.usable, "usable"},         {&fields.unavailable, "unavailable"},
      {&fields.residual, "residual"},     {&fields.protected_capacity, "protected"},
      {&fields.reserved, "reserved"},
  };
  for (const NamedMeasuring& entry : measured_fields) {
    const Status ok = check_unit(*entry.value, fields.unit, entry.field);
    if (!ok.has_value()) {
      return ok.error();
    }
  }

  if (!fields.installed.is_known()) {
    return Error::make(ErrorCode::invalid_argument, "installed capacity must be measured");
  }
  if (fields.observed.is_known() && fields.observed.magnitude() > fields.installed.magnitude()) {
    Error error = Error::make(ErrorCode::account_mismatch, "observed capacity exceeds installed capacity");
    error.with_constraint("observed <= installed");
    return error;
  }

  SourceEvidence result;
  result.source_ = std::move(fields.source);
  result.dimension_ = fields.dimension;
  result.unit_ = fields.unit;
  result.generation_ = fields.generation;
  result.observed_at_ = fields.observed_at;
  result.provenance_ = std::move(fields.provenance);
  result.state_ = fields.state;
  result.state_reason_ = fields.state_reason;
  result.state_detail_ = internal::truncate(fields.state_detail, limits::max_detail_length);
  result.installed_ = fields.installed;
  result.observed_ = fields.observed;
  result.usable_ = fields.usable;
  result.unavailable_ = fields.unavailable;
  result.protected_capacity_ = fields.protected_capacity;
  result.reserved_ = fields.reserved;

  if (fields.derive_residual) {
    if (!fields.usable.is_known() || !fields.unavailable.is_known()) {
      return Error::make(ErrorCode::invalid_argument,
                         "the residual can only be derived when usable and unavailable are both measured");
    }
    const Result<Measured> partial =
        fields.installed.subtract(fields.unavailable, "installed - unavailable >= 0");
    if (!partial.has_value()) {
      return partial.error();
    }
    const Result<Measured> derived =
        partial.value().subtract(fields.usable, "residual = installed - unavailable - usable >= 0");
    if (!derived.has_value()) {
      return derived.error();
    }
    result.residual_ = derived.value();
    result.closure_proven_ = true;
  } else {
    result.residual_ = fields.residual;
    if (fields.usable.is_known() && fields.unavailable.is_known() && fields.residual.is_known()) {
      const Measured total = fields.usable.add(fields.unavailable).add(fields.residual);
      if (!total.is_known() || total.magnitude() != fields.installed.magnitude()) {
        Error error = Error::make(ErrorCode::account_mismatch,
                                  "the source's usable, unavailable and residual parts do not sum to installed");
        error.with_constraint("usable + unavailable + residual == installed");
        return error;
      }
      result.closure_proven_ = true;
    }
  }

  if (fields.usable.is_known() && fields.protected_capacity.is_known() && fields.reserved.is_known()) {
    const Measured held = fields.protected_capacity.add(fields.reserved);
    if (held.is_known() && held.magnitude() > fields.usable.magnitude()) {
      Error error = Error::make(ErrorCode::account_mismatch,
                                "a source's protected and reserved capacity exceeds its usable capacity");
      error.with_constraint("protected + reserved <= usable");
      return error;
    }
  }

  if (fields.state == OperationalState::unavailable && !fields.usable.is_known_zero()) {
    return Error::make(ErrorCode::invalid_argument,
                       "a source that reports itself unavailable must report zero usable capacity");
  }
  if (fields.state == OperationalState::nominal &&
      (fields.state_reason == ReasonCode::source_unavailable ||
       fields.state_reason == ReasonCode::source_state_unknown)) {
    return Error::make(ErrorCode::invalid_argument,
                       "a nominal source cannot carry an unavailable or unknown state reason");
  }

  result.digest_ = internal::evidence_digest(result);
  return result;
}

Measured SourceEvidence::allocatable() const {
  if (!usable_.is_known() || !protected_capacity_.is_known() || !reserved_.is_known()) {
    return Measured::unknown();
  }
  const Measured held = protected_capacity_.add(reserved_);
  if (!held.is_known()) {
    return Measured::unknown();
  }
  const Result<Measured> remaining = usable_.subtract(held, "usable - protected - reserved >= 0");
  if (!remaining.has_value()) {
    return Measured::unknown();
  }
  return remaining.value();
}

std::string SourceEvidence::to_string() const {
  std::string out = source_.value();
  out.push_back('/');
  out.append(capacity_dimension_name(dimension_));
  out.append(" gen=");
  out.append(generation_.to_string());
  out.append(" state=");
  out.append(operational_state_name(state_));
  out.append(" installed=");
  out.append(installed_.to_string());
  out.append(" usable=");
  out.append(usable_.to_string());
  out.append(" unavailable=");
  out.append(unavailable_.to_string());
  out.append(" residual=");
  out.append(residual_.to_string());
  out.append(" protected=");
  out.append(protected_capacity_.to_string());
  out.append(" reserved=");
  out.append(reserved_.to_string());
  out.append(" allocatable=");
  out.append(allocatable().to_string());
  out.append(" closure=");
  out.append(closure_proven_ ? "proven" : "unproven");
  return out;
}

std::ostream& operator<<(std::ostream& out, const SourceEvidence& evidence) {
  return out << evidence.to_string();
}

}  // namespace dccp::facility_capacity
