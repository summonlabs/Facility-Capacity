// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Declared reserves.
//
// A `CapacityReserve` is a *declaration by a named owner* that a quantity of a
// dimension is held out of allocation for a reason, over an exact window. It is
// evidence, not authority: Facility Capacity composes these declarations to
// reduce the allocatable capacity of a dimension. It does not create, admit,
// release or schedule reservations; that lifecycle belongs to the reservation
// runtime, which is free to declare its committed amount here.

#ifndef DCCP_FACILITY_CAPACITY_RESERVE_HPP
#define DCCP_FACILITY_CAPACITY_RESERVE_HPP

#include <cstdint>
#include <iosfwd>
#include <string>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/provenance.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// What a declared reserve is held back for.
enum class ReserveKind : std::uint8_t {
  /// Redundancy: capacity that must stay free so that a failure can be
  /// absorbed. Contributes to the dimension's protected capacity.
  protection = 0,
  /// Operational headroom: capacity kept free for normal operating movement.
  operational = 1,
  /// Contractual or regulatory obligation to keep capacity free.
  service_obligation = 2,
  /// Contingency provision held against an unplanned event.
  contingency = 3,
};

std::string_view reserve_kind_name(ReserveKind kind) noexcept;
bool parse_reserve_kind(std::string_view name, ReserveKind& out) noexcept;
std::ostream& operator<<(std::ostream& out, ReserveKind kind);

/// True when the kind contributes to protected capacity rather than to the
/// general reserved total.
constexpr bool reserve_kind_is_protection(ReserveKind kind) noexcept {
  return kind == ReserveKind::protection;
}

struct CapacityReserveFields {
  ReserveId id;
  /// The evidence source under which this reserve is declared. A reserve only
  /// reduces the allocatable capacity of the source that declares it.
  CapacitySourceId source;
  CapacityDimension dimension = CapacityDimension::space;
  Unit unit = Unit::rack_unit;
  ReserveKind kind = ReserveKind::protection;
  /// The exact held-back quantity. May be unmeasured, in which case it makes
  /// the dimension's allocatable capacity unmeasured rather than reducing it by
  /// a guessed amount.
  Measured amount;
  ValidityWindow window;
  /// Who declares the reserve. Opaque reference.
  OwnerId owner;
  /// Optional service class the reserve serves. May be empty.
  ServiceClassId service_class;
  ReasonCode reason = ReasonCode::none;
  std::string detail;
  Provenance provenance;
};

/// One immutable, digest-bound declared reserve.
class CapacityReserve {
 public:
  CapacityReserve() = default;

  static Result<CapacityReserve> create(CapacityReserveFields fields);

  const ReserveId& id() const noexcept { return id_; }
  const CapacitySourceId& source() const noexcept { return source_; }
  CapacityDimension dimension() const noexcept { return dimension_; }
  Unit unit() const noexcept { return unit_; }
  ReserveKind kind() const noexcept { return kind_; }
  const Measured& amount() const noexcept { return amount_; }
  const ValidityWindow& window() const noexcept { return window_; }
  const OwnerId& owner() const noexcept { return owner_; }
  const ServiceClassId& service_class() const noexcept { return service_class_; }
  ReasonCode reason() const noexcept { return reason_; }
  const std::string& detail() const noexcept { return detail_; }
  const Provenance& provenance() const noexcept { return provenance_; }

  /// True when the declared window covers `instant`.
  bool active_at(Tick instant) const noexcept { return window_.active_at(instant); }

  /// True when this reserve reduces protected capacity rather than reserved
  /// capacity.
  bool supplies_protection() const noexcept { return reserve_kind_is_protection(kind_); }

  /// The amount counted at `instant`: the declared amount inside the window and
  /// an unmeasured value outside it.
  Measured amount_at(Tick instant) const;

  const std::string& digest() const noexcept { return digest_; }

  std::string to_string() const;

  friend bool operator==(const CapacityReserve& lhs, const CapacityReserve& rhs) noexcept {
    return lhs.digest_ == rhs.digest_;
  }

 private:
  ReserveId id_;
  CapacitySourceId source_;
  CapacityDimension dimension_ = CapacityDimension::space;
  Unit unit_ = Unit::rack_unit;
  ReserveKind kind_ = ReserveKind::protection;
  Measured amount_;
  ValidityWindow window_;
  OwnerId owner_;
  ServiceClassId service_class_;
  ReasonCode reason_ = ReasonCode::none;
  std::string detail_;
  Provenance provenance_;
  std::string digest_;
};

std::ostream& operator<<(std::ostream& out, const CapacityReserve& reserve);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_RESERVE_HPP
