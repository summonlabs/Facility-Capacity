// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/reserve.hpp"

#include <ostream>
#include <utility>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/codec.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

Status check_detail(std::string_view detail) {
  if (detail.size() > limits::max_detail_length) {
    return Error::make(ErrorCode::limit_exceeded, "a reserve detail exceeds the length bound");
  }
  if (!internal::is_ascii_printable(detail)) {
    return Error::make(ErrorCode::invalid_argument, "a reserve detail contains non-printable characters");
  }
  return Status::success();
}

}  // namespace

std::string_view reserve_kind_name(ReserveKind kind) noexcept {
  switch (kind) {
    case ReserveKind::protection:
      return "protection";
    case ReserveKind::operational:
      return "operational";
    case ReserveKind::service_obligation:
      return "service_obligation";
    case ReserveKind::contingency:
      return "contingency";
  }
  return "unknown";
}

bool parse_reserve_kind(std::string_view name, ReserveKind& out) noexcept {
  if (name == "protection") {
    out = ReserveKind::protection;
    return true;
  }
  if (name == "operational") {
    out = ReserveKind::operational;
    return true;
  }
  if (name == "service_obligation") {
    out = ReserveKind::service_obligation;
    return true;
  }
  if (name == "contingency") {
    out = ReserveKind::contingency;
    return true;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, ReserveKind kind) { return out << reserve_kind_name(kind); }

Result<CapacityReserve> CapacityReserve::create(CapacityReserveFields fields) {
  if (fields.id.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a reserve has no identity");
  }
  if (fields.source.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a reserve declares no source it is held against");
  }
  const Unit canonical = canonical_unit(fields.dimension);
  if (fields.unit != canonical) {
    Error error = Error::make(ErrorCode::unit_mismatch, "reserve unit is not the dimension's canonical unit");
    error.with_constraint("unit == canonical_unit(dimension)");
    return error;
  }
  if (fields.amount.is_known() && fields.amount.unit() != fields.unit) {
    Error error = Error::make(ErrorCode::unit_mismatch, "reserve amount is not in the reserve's unit");
    error.with_constraint("amount.unit == unit");
    return error;
  }
  const Status detail_ok = check_detail(fields.detail);
  if (!detail_ok.has_value()) {
    return detail_ok.error();
  }
  const Status provenance_ok = fields.provenance.validate();
  if (!provenance_ok.has_value()) {
    return provenance_ok.error();
  }
  if (fields.provenance.source != fields.source) {
    return Error::make(ErrorCode::conflict, "reserve provenance names a different source");
  }

  CapacityReserve reserve;
  reserve.id_ = std::move(fields.id);
  reserve.source_ = std::move(fields.source);
  reserve.dimension_ = fields.dimension;
  reserve.unit_ = fields.unit;
  reserve.kind_ = fields.kind;
  reserve.amount_ = fields.amount;
  reserve.window_ = fields.window;
  reserve.owner_ = std::move(fields.owner);
  reserve.service_class_ = std::move(fields.service_class);
  reserve.reason_ = fields.reason;
  reserve.detail_ = internal::truncate(fields.detail, limits::max_detail_length);
  reserve.provenance_ = std::move(fields.provenance);
  reserve.digest_ = internal::reserve_digest(reserve);
  return reserve;
}

Measured CapacityReserve::amount_at(Tick instant) const {
  if (!window_.active_at(instant)) {
    return Measured::unknown();
  }
  return amount_;
}

std::string CapacityReserve::to_string() const {
  std::string out = id_.value();
  out.append(" kind=");
  out.append(reserve_kind_name(kind_));
  out.append(" dimension=");
  out.append(capacity_dimension_name(dimension_));
  out.append(" source=");
  out.append(source_.value());
  out.append(" amount=");
  out.append(amount_.to_string());
  out.append(" window=");
  out.append(window_.to_string());
  if (!owner_.empty()) {
    out.append(" owner=");
    out.append(owner_.value());
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const CapacityReserve& reserve) { return out << reserve.to_string(); }

}  // namespace dccp::facility_capacity
