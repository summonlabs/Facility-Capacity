// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/units.hpp"

#include <array>
#include <ostream>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/checked.hpp"

namespace dccp::facility_capacity {
namespace {

struct UnitEntry {
  Unit unit;
  std::string_view name;
  std::string_view symbol;
};

constexpr std::array<UnitEntry, unit_count> kUnits{{
    {Unit::rack_unit, "rack_unit", "U"},
    {Unit::rack_slot, "rack_slot", "slot"},
    {Unit::milli_watt, "milli_watt", "mW"},
    {Unit::milli_watt_thermal, "milli_watt_thermal", "mWth"},
    {Unit::reserve_quantum, "reserve_quantum", "rq"},
}};

Error unit_error(std::string_view constraint) {
  Error error = Error::make(ErrorCode::unit_mismatch, "quantities in different units were combined");
  error.with_constraint(std::string(constraint));
  return error;
}

Error magnitude_error() {
  Error error = Error::make(ErrorCode::limit_exceeded, "quantity magnitude is outside the accepted range");
  error.with_constraint("abs(magnitude) <= max_quantity_magnitude");
  return error;
}

}  // namespace

std::string_view unit_name(Unit unit) noexcept {
  for (const UnitEntry& entry : kUnits) {
    if (entry.unit == unit) {
      return entry.name;
    }
  }
  return "unknown_unit";
}

std::string_view unit_symbol(Unit unit) noexcept {
  for (const UnitEntry& entry : kUnits) {
    if (entry.unit == unit) {
      return entry.symbol;
    }
  }
  return "?";
}

bool parse_unit(std::string_view name, Unit& out) noexcept {
  for (const UnitEntry& entry : kUnits) {
    if (entry.name == name) {
      out = entry.unit;
      return true;
    }
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, Unit unit) { return out << unit_name(unit); }

Result<Quantity> Quantity::make(Unit unit, std::int64_t magnitude) {
  if (!internal::fits_quantity(magnitude)) {
    return magnitude_error();
  }
  Quantity quantity;
  quantity.unit_ = unit;
  quantity.magnitude_ = magnitude;
  return quantity;
}

Result<Quantity> Quantity::add(const Quantity& other) const {
  if (unit_ != other.unit_) {
    return unit_error("lhs.unit == rhs.unit");
  }
  const Result<std::int64_t> sum = internal::add_i64(magnitude_, other.magnitude_, "sum within int64");
  if (!sum.has_value()) {
    return sum.error();
  }
  return Quantity::make(unit_, sum.value());
}

Result<Quantity> Quantity::subtract(const Quantity& other) const {
  if (unit_ != other.unit_) {
    return unit_error("lhs.unit == rhs.unit");
  }
  const Result<std::int64_t> difference = internal::sub_i64(magnitude_, other.magnitude_, "difference within int64");
  if (!difference.has_value()) {
    return difference.error();
  }
  return Quantity::make(unit_, difference.value());
}

Result<Quantity> Quantity::scale(std::int64_t factor) const {
  const Result<std::int64_t> product = internal::mul_i64(magnitude_, factor, "product within int64");
  if (!product.has_value()) {
    return product.error();
  }
  return Quantity::make(unit_, product.value());
}

std::string Quantity::to_string() const {
  return std::to_string(magnitude_) + " " + std::string(unit_symbol(unit_));
}

std::ostream& operator<<(std::ostream& out, const Quantity& quantity) { return out << quantity.to_string(); }

Result<Quantity> checked_add(const Quantity& lhs, const Quantity& rhs) { return lhs.add(rhs); }

Result<Quantity> checked_subtract(const Quantity& lhs, const Quantity& rhs) { return lhs.subtract(rhs); }

Result<Quantity> checked_sum(const Quantity* quantities, std::size_t count, Unit unit) {
  Quantity total = Quantity::zero(unit);
  for (std::size_t index = 0; index < count; ++index) {
    const Result<Quantity> next = total.add(quantities[index]);
    if (!next.has_value()) {
      return next.error();
    }
    total = next.value();
  }
  return total;
}

}  // namespace dccp::facility_capacity
