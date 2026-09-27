// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/revalidate.hpp"

#include <ostream>

#include "dccp/facility_capacity/explain.hpp"

namespace dccp::facility_capacity {

std::string_view revalidation_status_name(RevalidationStatus status) noexcept {
  switch (status) {
    case RevalidationStatus::valid:
      return "valid";
    case RevalidationStatus::stale:
      return "stale";
    case RevalidationStatus::incomplete:
      return "incomplete";
    case RevalidationStatus::superseded:
      return "superseded";
  }
  return "incomplete";
}

bool parse_revalidation_status(std::string_view name, RevalidationStatus& out) noexcept {
  if (name == "valid") {
    out = RevalidationStatus::valid;
    return true;
  }
  if (name == "stale") {
    out = RevalidationStatus::stale;
    return true;
  }
  if (name == "incomplete") {
    out = RevalidationStatus::incomplete;
    return true;
  }
  if (name == "superseded") {
    out = RevalidationStatus::superseded;
    return true;
  }
  return false;
}

std::uint8_t revalidation_status_severity(RevalidationStatus status) noexcept {
  switch (status) {
    case RevalidationStatus::valid:
      return 0;
    case RevalidationStatus::stale:
      return 1;
    case RevalidationStatus::incomplete:
      return 2;
    case RevalidationStatus::superseded:
      return 3;
  }
  return 3;
}

std::ostream& operator<<(std::ostream& out, RevalidationStatus status) {
  return out << revalidation_status_name(status);
}

std::string RevalidationReport::describe() const { return explain::render_revalidation(*this); }

std::ostream& operator<<(std::ostream& out, const RevalidationReport& report) {
  return out << report.describe();
}

}  // namespace dccp::facility_capacity
