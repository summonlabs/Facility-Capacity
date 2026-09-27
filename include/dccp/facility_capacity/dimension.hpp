// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Capacity dimensions.
//
// A dimension is a distinct kind of facility capacity with exactly one
// canonical exact unit. Totals are only ever formed within a dimension:
// summing rack units into milliwatts is a typed failure, not a number.

#ifndef DCCP_FACILITY_CAPACITY_DIMENSION_HPP
#define DCCP_FACILITY_CAPACITY_DIMENSION_HPP

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string_view>

#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/units.hpp"

namespace dccp::facility_capacity {

/// The dimensions the aggregate facility-capacity model composes.
enum class CapacityDimension : std::uint8_t {
  /// Usable vertical rack space, in rack units.
  space = 0,
  /// Countable rack or rack-position capacity, in rack slots.
  rack = 1,
  /// Electrical power available to IT load, in milliwatts.
  power = 2,
  /// Heat removal available to IT load, in milliwatts thermal.
  cooling = 3,
  /// Callable operational reserve provision, in reserve quanta.
  operational_reserve = 4,
  /// Electrical capacity committed to facility services (house load), in
  /// milliwatts.
  facility_service = 5,
};

/// Number of dimensions in the model.
inline constexpr std::size_t capacity_dimension_count = 6;

/// Stable machine-readable name, e.g. `operational_reserve`.
std::string_view capacity_dimension_name(CapacityDimension dimension) noexcept;

/// Parses a name produced by `capacity_dimension_name`.
bool parse_capacity_dimension(std::string_view name, CapacityDimension& out) noexcept;

/// The one unit accepted for a dimension's evidence.
Unit canonical_unit(CapacityDimension dimension) noexcept;

/// Ordinal used for deterministic ordering.
constexpr std::size_t dimension_ordinal(CapacityDimension dimension) noexcept {
  return static_cast<std::size_t>(dimension);
}

/// Every dimension in canonical ordinal order.
const CapacityDimension* all_capacity_dimensions() noexcept;

std::ostream& operator<<(std::ostream& out, CapacityDimension dimension);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_DIMENSION_HPP
