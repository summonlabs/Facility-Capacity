// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Exact units and exact quantities.
//
// Authoritative capacity accounting is integer-only. A quantity is an exact
// signed 64-bit magnitude in one explicit unit. Combining quantities in
// different units is a typed failure, never a silent conversion.

#ifndef DCCP_FACILITY_CAPACITY_UNITS_HPP
#define DCCP_FACILITY_CAPACITY_UNITS_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity {

/// The exact unit of a quantity.
///
/// There is no floating-point unit anywhere in this product. Each unit is an
/// exact integer scale chosen so that the smallest meaningful increment of the
/// physical resource is representable without rounding.
enum class Unit : std::uint8_t {
  /// One rack unit of vertical rack space (44.45 mm).
  rack_unit = 0,
  /// One countable rack or rack-position slot.
  rack_slot = 1,
  /// One milliwatt of electrical power.
  milli_watt = 2,
  /// One milliwatt of heat removal rate.
  milli_watt_thermal = 3,
  /// One exact quantum of declared callable operational reserve.
  ///
  /// The quantum's physical meaning is owned by the operational-reserve source.
  /// Facility Capacity composes the exact count and never reinterprets it as
  /// power, space or cooling, so operational reserve can never be silently
  /// conflated with a physical dimension.
  reserve_quantum = 4,
};

/// Number of distinct units.
inline constexpr std::size_t unit_count = 5;

/// Stable machine-readable name, e.g. `milli_watt`.
std::string_view unit_name(Unit unit) noexcept;

/// Short symbol used in explanations, e.g. `mW`.
std::string_view unit_symbol(Unit unit) noexcept;

/// Parses a name produced by `unit_name`.
bool parse_unit(std::string_view name, Unit& out) noexcept;

std::ostream& operator<<(std::ostream& out, Unit unit);

/// An exact quantity: a unit plus an exact signed magnitude.
///
/// Negative magnitudes are representable because subtraction and differencing
/// are exact operations. Negative magnitudes are never accepted as an
/// *authoritative* capacity value: `Measured` refuses them.
class Quantity {
 public:
  constexpr Quantity() noexcept = default;

  /// Builds a quantity. Fails with `limit_exceeded` when the magnitude is
  /// outside `[-max_quantity_magnitude, max_quantity_magnitude]`.
  static Result<Quantity> make(Unit unit, std::int64_t magnitude);

  static constexpr Quantity zero(Unit unit) noexcept {
    Quantity quantity;
    quantity.unit_ = unit;
    quantity.magnitude_ = 0;
    return quantity;
  }

  constexpr Unit unit() const noexcept { return unit_; }

  constexpr std::int64_t magnitude() const noexcept { return magnitude_; }

  constexpr bool is_zero() const noexcept { return magnitude_ == 0; }

  constexpr bool is_negative() const noexcept { return magnitude_ < 0; }

  /// Exact sum. Fails with `unit_mismatch` when the units differ and with
  /// `limit_exceeded` when the exact result is not representable.
  Result<Quantity> add(const Quantity& other) const;

  /// Exact difference. Fails with `unit_mismatch` when the units differ. The
  /// result may be negative; callers that need a non-negative capacity value
  /// use `Measured`.
  Result<Quantity> subtract(const Quantity& other) const;

  /// Exact scaling with a checked overflow test.
  Result<Quantity> scale(std::int64_t factor) const;

  friend constexpr bool operator==(const Quantity& lhs, const Quantity& rhs) noexcept {
    return lhs.unit_ == rhs.unit_ && lhs.magnitude_ == rhs.magnitude_;
  }

  std::string to_string() const;

 private:
  Unit unit_ = Unit::rack_unit;
  std::int64_t magnitude_ = 0;
};

std::ostream& operator<<(std::ostream& out, const Quantity& quantity);

/// Adds two quantities. Free function form used by the accounting code.
Result<Quantity> checked_add(const Quantity& lhs, const Quantity& rhs);

/// Subtracts two quantities with a checked result.
Result<Quantity> checked_subtract(const Quantity& lhs, const Quantity& rhs);

/// Adds a list of quantities that all share one unit.
Result<Quantity> checked_sum(const Quantity* quantities, std::size_t count, Unit unit);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_UNITS_HPP
