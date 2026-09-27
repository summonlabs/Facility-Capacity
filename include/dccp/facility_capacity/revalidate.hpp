// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Revalidation: deciding whether a previously published answer may still be
// relied on.
//
// Revalidation is a pure comparison between a snapshot and the current
// authoritative state. It never mutates either. Its whole purpose is to make
// "this answer is still current" a checkable claim rather than an assumption.

#ifndef DCCP_FACILITY_CAPACITY_REVALIDATE_HPP
#define DCCP_FACILITY_CAPACITY_REVALIDATE_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"

namespace dccp::facility_capacity {

/// How a snapshot stands against current state.
///
/// Severity is ordered and the reported status is the most severe finding:
///   `valid` < `stale` < `incomplete` < `superseded`
///   - `valid`       nothing rejects the snapshot;
///   - `stale`       the evidence it used has been superseded by newer
///                   evidence, or its validity window has passed;
///   - `incomplete`  the evidence required by the current coverage contract is
///                   not all present, so no answer can be justified;
///   - `superseded`  a newer authoritative capacity generation has already been
///                   published.
///
/// Every finding, not only the most severe one, is reported.
enum class RevalidationStatus : std::uint8_t {
  valid = 0,
  stale = 1,
  incomplete = 2,
  superseded = 3,
};

std::string_view revalidation_status_name(RevalidationStatus status) noexcept;
bool parse_revalidation_status(std::string_view name, RevalidationStatus& out) noexcept;
std::ostream& operator<<(std::ostream& out, RevalidationStatus status);

/// The relative severity used to select the reported status.
std::uint8_t revalidation_status_severity(RevalidationStatus status) noexcept;

struct RevalidationReport {
  RevalidationStatus status = RevalidationStatus::valid;
  /// Generation carried by the snapshot.
  CapacityGeneration snapshot_generation{};
  /// Current authoritative generation of the model.
  CapacityGeneration current_generation{};
  /// Current revision of the model.
  Revision current_revision{};
  NoteSet findings;

  bool is_valid() const noexcept { return status == RevalidationStatus::valid; }

  /// True when the snapshot must not be relied on.
  bool rejected() const noexcept { return status != RevalidationStatus::valid; }

  std::string describe() const;
};

std::ostream& operator<<(std::ostream& out, const RevalidationReport& report);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_REVALIDATE_HPP
