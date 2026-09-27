// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/constraint.hpp"

#include <algorithm>
#include <ostream>
#include <utility>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/codec.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

Status check_detail(std::string_view detail, std::string_view what) {
  if (detail.size() > limits::max_detail_length) {
    Error error = Error::make(ErrorCode::limit_exceeded, "a detail field exceeds the length bound");
    error.with_constraint(std::string(what));
    return error;
  }
  if (!internal::is_ascii_printable(detail)) {
    Error error = Error::make(ErrorCode::invalid_argument, "a detail field contains non-printable characters");
    error.with_constraint(std::string(what));
    return error;
  }
  return Status::success();
}

}  // namespace

Result<ServiceConstraint> ServiceConstraint::create(ServiceConstraintFields fields) {
  if (fields.id.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a service constraint has no identity");
  }
  const Unit canonical = canonical_unit(fields.dimension);
  if (fields.unit != canonical) {
    Error error = Error::make(ErrorCode::unit_mismatch, "constraint unit is not the dimension's canonical unit");
    error.with_constraint("unit == canonical_unit(dimension)");
    return error;
  }
  if (fields.floor.is_known() && fields.floor.unit() != fields.unit) {
    Error error = Error::make(ErrorCode::unit_mismatch, "constraint floor is not in the constraint's unit");
    error.with_constraint("floor.unit == unit");
    return error;
  }
  const Status detail_ok = check_detail(fields.detail, "detail");
  if (!detail_ok.has_value()) {
    return detail_ok.error();
  }
  const Status provenance_ok = fields.provenance.validate();
  if (!provenance_ok.has_value()) {
    return provenance_ok.error();
  }

  ServiceConstraint constraint;
  constraint.id_ = std::move(fields.id);
  constraint.dimension_ = fields.dimension;
  constraint.unit_ = fields.unit;
  constraint.service_class_ = std::move(fields.service_class);
  constraint.floor_ = fields.floor;
  constraint.window_ = fields.window;
  constraint.owner_ = std::move(fields.owner);
  constraint.reason_ = fields.reason;
  constraint.detail_ = internal::truncate(fields.detail, limits::max_detail_length);
  constraint.provenance_ = std::move(fields.provenance);
  constraint.digest_ = internal::constraint_digest(constraint);
  return constraint;
}

std::string ServiceConstraint::to_string() const {
  std::string out = id_.value();
  out.append(" dimension=");
  out.append(capacity_dimension_name(dimension_));
  out.append(" floor=");
  out.append(floor_.to_string());
  out.append(" window=");
  out.append(window_.to_string());
  if (!service_class_.empty()) {
    out.append(" service_class=");
    out.append(service_class_.value());
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const ServiceConstraint& constraint) {
  return out << constraint.to_string();
}

Status CapacityRequirement::normalize() {
  if (required_sources.size() > limits::max_evidence_sources) {
    return Error::make(ErrorCode::limit_exceeded, "a coverage requirement declares too many sources");
  }
  for (const CapacitySourceId& source : required_sources) {
    if (source.empty()) {
      return Error::make(ErrorCode::invalid_argument, "a coverage requirement declares an empty source identity");
    }
  }
  std::sort(required_sources.begin(), required_sources.end());
  required_sources.erase(std::unique(required_sources.begin(), required_sources.end()), required_sources.end());
  return Status::success();
}

std::string CapacityRequirement::to_string() const {
  std::string out(capacity_dimension_name(dimension));
  out.append(required ? " required" : " optional");
  if (!required_sources.empty()) {
    out.append(" sources=[");
    for (std::size_t index = 0; index < required_sources.size(); ++index) {
      if (index != 0) {
        out.push_back(',');
      }
      out.append(required_sources[index].value());
    }
    out.push_back(']');
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const CapacityRequirement& requirement) {
  return out << requirement.to_string();
}

}  // namespace dccp::facility_capacity
