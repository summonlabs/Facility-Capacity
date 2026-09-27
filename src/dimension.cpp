// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/dimension.hpp"

#include <array>
#include <ostream>

namespace dccp::facility_capacity {
namespace {

struct DimensionEntry {
  CapacityDimension dimension;
  std::string_view name;
  Unit unit;
};

constexpr std::array<DimensionEntry, capacity_dimension_count> kDimensions{{
    {CapacityDimension::space, "space", Unit::rack_unit},
    {CapacityDimension::rack, "rack", Unit::rack_slot},
    {CapacityDimension::power, "power", Unit::milli_watt},
    {CapacityDimension::cooling, "cooling", Unit::milli_watt_thermal},
    {CapacityDimension::operational_reserve, "operational_reserve", Unit::reserve_quantum},
    {CapacityDimension::facility_service, "facility_service", Unit::milli_watt},
}};

}  // namespace

std::string_view capacity_dimension_name(CapacityDimension dimension) noexcept {
  for (const DimensionEntry& entry : kDimensions) {
    if (entry.dimension == dimension) {
      return entry.name;
    }
  }
  return "unknown_dimension";
}

bool parse_capacity_dimension(std::string_view name, CapacityDimension& out) noexcept {
  for (const DimensionEntry& entry : kDimensions) {
    if (entry.name == name) {
      out = entry.dimension;
      return true;
    }
  }
  return false;
}

Unit canonical_unit(CapacityDimension dimension) noexcept {
  for (const DimensionEntry& entry : kDimensions) {
    if (entry.dimension == dimension) {
      return entry.unit;
    }
  }
  return Unit::rack_unit;
}

const CapacityDimension* all_capacity_dimensions() noexcept {
  static const std::array<CapacityDimension, capacity_dimension_count> kOrdered{{
      CapacityDimension::space,
      CapacityDimension::rack,
      CapacityDimension::power,
      CapacityDimension::cooling,
      CapacityDimension::operational_reserve,
      CapacityDimension::facility_service,
  }};
  return kOrdered.data();
}

std::ostream& operator<<(std::ostream& out, CapacityDimension dimension) {
  return out << capacity_dimension_name(dimension);
}

}  // namespace dccp::facility_capacity
